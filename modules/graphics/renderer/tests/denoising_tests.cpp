#include "../src/internal/gpu_denoiser.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/native_compute.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;
namespace format = ludus::content::render_graph_format;
using Status = rhi::RasterStatus;
struct RhiCleanup final
{
    ~RhiCleanup() noexcept
    {
        rhi::Shutdown();
    }
};
} // namespace

TEST_CASE("OIDN GPU pass preserves HDR and metadata and retires through graph completion", "[renderer][denoising]")
{
    if (renderer::internal::IsGpuDenoisingAvailable() == false)
    {
        renderer::internal::GpuDenoiser denoiser;
        CHECK(denoiser.Prepare({}, {}, 1, 1, static_cast<uint32>(format::ProcessingQuality::Fast)) ==
              Status::Unsupported);
        return;
    }
    rhi::Shutdown();
    RhiCleanup cleanup;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    REQUIRE(rhi::CreateDevice({},
                              {
                                  .Width = 8,
                                  .Height = 8,
                              },
                              description,
                              device,
                              surface) == rhi::DeviceStatus::Ready);
    constexpr usize EXTENT = 64;
    constexpr usize PIXEL_COUNT = EXTENT * EXTENT;
    StaticArray<math::Vector4, PIXEL_COUNT> pixels{};
    float64 originalError = 0;
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        const float32 expected = index % EXTENT < EXTENT / 2 ? .25F : 2;
        const float32 noise = (index * 1664525 + 1013904223) % 257 < 128 ? .5F : -.5F;
        const float32 value = expected * (1 + noise);
        pixels[index] = {value, value, value, static_cast<float32>(index)};
        const float64 difference = static_cast<float64>(value) - static_cast<float64>(expected);
        originalError += difference * difference;
    }
    const usize bytes = sizeof(pixels);
    SECTION("filtering after pending preparation")
    {
        for (const format::ProcessingQuality quality :
             {format::ProcessingQuality::Fast, format::ProcessingQuality::Balanced, format::ProcessingQuality::High})
        {
            rhi::BufferHandle colors;
            REQUIRE(rhi::CreateBuffer(device,
                                      {rhi::BufferRole::Storage, bytes},
                                      {reinterpret_cast<const uint8*>(pixels.GetData()), bytes},
                                      colors) == Status::Ready);
            renderer::internal::GpuDenoiser denoiser;
            Status prepared = denoiser.Prepare(device, colors, EXTENT, EXTENT, static_cast<uint32>(quality));
            const uint64 preparationStart = time::NowTicks();
            while (prepared == Status::Pending &&
                   time::NowTicks() - preparationStart < 30 * time::NANOSECONDS_PER_SECOND)
            {
                prepared = denoiser.Prepare(device, colors, EXTENT, EXTENT, static_cast<uint32>(quality));
            }
            REQUIRE(prepared == Status::Ready);
            rhi::OrderedGraph graph;
            REQUIRE(rhi::CreateOrderedGraph(device, graph) == Status::Ready);
            rhi::GraphVersion input;
            rhi::GraphVersion output;
            REQUIRE(rhi::ImportGraphBuffer(device, graph, colors, input) == Status::Ready);
            REQUIRE(rhi::NextGraphVersion(device, graph, input, output) == Status::Ready);
            const rhi::GraphUse use{output,
                                    rhi::GraphAccessMode::StorageReadWrite,
                                    0,
                                    bytes,
                                    rhi::RasterVisibility::Compute};
            const rhi::ComputeDispatch dispatch = denoiser.GetDispatch();
            rhi::GraphPassDescription pass;
            pass.Name = "OIDN acceptance";
            pass.Dispatch = &dispatch;
            pass.Uses = &use;
            pass.UseCount = 1;
            REQUIRE(rhi::AddGraphPass(device, graph, pass) == Status::Ready);
            REQUIRE(rhi::AddGraphRoot(device, graph, output, rhi::GraphRoot::Readback) == Status::Ready);
            rhi::GraphReport report;
            REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
            denoiser.Reset(); // The graph, then its submission, must retain filter and native buffers.
            rhi::SubmissionToken completion;
            REQUIRE(rhi::ExecuteOrderedGraph(device, surface, graph, completion) == Status::Ready);
            rhi::ReadbackTicket readback;
            REQUIRE(rhi::RequestBufferReadback(device, colors, 0, bytes, readback) == Status::Pending);
            const uint64 start = time::NowTicks();
            while (rhi::GetStatus(device, readback) == Status::Pending &&
                   time::NowTicks() - start < 30 * time::NANOSECONDS_PER_SECOND)
            {
                static_cast<void>(rhi::PollLifetime(device));
            }
            REQUIRE(rhi::GetStatus(device, readback) == Status::Ready);
            StaticArray<math::Vector4, PIXEL_COUNT> filteredPixels{};
            REQUIRE(rhi::CopyReadback(device, readback, reinterpret_cast<uint8*>(filteredPixels.GetData()), bytes) ==
                    Status::Ready);
            float64 denoisedError = 0;
            for (usize index = 0; index < pixels.GetSize(); ++index)
            {
                const float32 expected = index % EXTENT < EXTENT / 2 ? .25F : 2;
                CHECK(math::IsFinite(filteredPixels[index].X) == true);
                CHECK(filteredPixels[index].W == static_cast<float32>(index));
                const float64 difference =
                    static_cast<float64>(filteredPixels[index].X) - static_cast<float64>(expected);
                denoisedError += difference * difference;
            }
            CHECK(denoisedError < originalError);
            CHECK(filteredPixels[EXTENT - 1].X > 1); // No tone mapping or RGBA8 clipping entered the filter.
            REQUIRE(rhi::Release(device, readback) == Status::Ready);
            REQUIRE(rhi::Destroy(device, colors) == Status::Ready);
        }
    }
    SECTION("cancelling preparation retains native inputs until the worker completes")
    {
        rhi::BufferHandle colors;
        REQUIRE(rhi::CreateBuffer(device,
                                  {rhi::BufferRole::Storage, bytes},
                                  {reinterpret_cast<const uint8*>(pixels.GetData()), bytes},
                                  colors) == Status::Ready);
        renderer::internal::GpuDenoiser denoiser;
        const Status prepared =
            denoiser.Prepare(device, colors, EXTENT, EXTENT, static_cast<uint32>(format::ProcessingQuality::High));
        REQUIRE((prepared == Status::Pending || prepared == Status::Ready));
        denoiser.Reset();
        REQUIRE(rhi::Destroy(device, colors) == Status::Ready);
        rhi::StorageUsage usage;
        const uint64 start = time::NowTicks();
        do
        {
            REQUIRE(rhi::PollLifetime(device) == Status::Ready);
            REQUIRE(rhi::GetStorageUsage(device, usage) == Status::Ready);
        } while (usage.ResourceBytes != 0 && time::NowTicks() - start < 30 * time::NANOSECONDS_PER_SECOND);
        CHECK(usage.ResourceBytes == 0);
    }
    SECTION("device shutdown drains preparation while the caller retains the filter")
    {
        rhi::BufferHandle colors;
        REQUIRE(rhi::CreateBuffer(device,
                                  {rhi::BufferRole::Storage, bytes},
                                  {reinterpret_cast<const uint8*>(pixels.GetData()), bytes},
                                  colors) == Status::Ready);
        renderer::internal::GpuDenoiser denoiser;
        const Status prepared =
            denoiser.Prepare(device, colors, EXTENT, EXTENT, static_cast<uint32>(format::ProcessingQuality::High));
        REQUIRE((prepared == Status::Pending || prepared == Status::Ready));
        REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
        denoiser.Reset();
        return;
    }
    SECTION("native dispatch capacity rejects before submitting GPU work")
    {
        rhi::BufferHandle colors;
        REQUIRE(rhi::CreateBuffer(device,
                                  {rhi::BufferRole::Storage, bytes},
                                  {reinterpret_cast<const uint8*>(pixels.GetData()), bytes},
                                  colors) == Status::Ready);
        renderer::internal::GpuDenoiser denoiser;
        Status prepared =
            denoiser.Prepare(device, colors, EXTENT, EXTENT, static_cast<uint32>(format::ProcessingQuality::Fast));
        const uint64 start = time::NowTicks();
        while (prepared == Status::Pending && time::NowTicks() - start < 30 * time::NANOSECONDS_PER_SECOND)
        {
            prepared =
                denoiser.Prepare(device, colors, EXTENT, EXTENT, static_cast<uint32>(format::ProcessingQuality::Fast));
        }
        REQUIRE(prepared == Status::Ready);
        rhi::OrderedGraph graph;
        REQUIRE(rhi::CreateOrderedGraph(device, graph) == Status::Ready);
        rhi::GraphVersion version;
        REQUIRE(rhi::ImportGraphBuffer(device, graph, colors, version) == Status::Ready);
        const rhi::ComputeDispatch dispatch = denoiser.GetDispatch();
        for (usize index = 0; index <= rhi::MAX_NATIVE_COMPUTE_PASSES_PER_FRAME; ++index)
        {
            rhi::GraphVersion next;
            REQUIRE(rhi::NextGraphVersion(device, graph, version, next) == Status::Ready);
            const rhi::GraphUse use{next,
                                    rhi::GraphAccessMode::StorageReadWrite,
                                    0,
                                    bytes,
                                    rhi::RasterVisibility::Compute};
            rhi::GraphPassDescription pass;
            pass.Name = "Native capacity acceptance";
            pass.Dispatch = &dispatch;
            pass.Uses = &use;
            pass.UseCount = 1;
            REQUIRE(rhi::AddGraphPass(device, graph, pass) == Status::Ready);
            version = next;
        }
        REQUIRE(rhi::AddGraphRoot(device, graph, version, rhi::GraphRoot::Readback) == Status::Ready);
        rhi::GraphReport report;
        REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
        rhi::SubmissionToken completion;
        CHECK(rhi::ExecuteOrderedGraph(device, surface, graph, completion) == Status::CapacityExceeded);
        CHECK(completion.IsNull() == true);
        REQUIRE(rhi::DiscardOrderedGraph(device, graph) == Status::Ready);
        denoiser.Reset();
        REQUIRE(rhi::Destroy(device, colors) == Status::Ready);
    }
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
}
