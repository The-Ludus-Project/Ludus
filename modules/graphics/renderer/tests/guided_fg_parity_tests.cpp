#include "guided_fg_oracle.h"
#include "guided_fg_parity.h"
#include <cstring>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

// Native execution of the template's guiding, density, reconnection and projection math.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;

template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    RasterStatus status = rhi::GetStatus(device, handle);
    while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == RasterStatus::Ready);
}

bool IsAccepted(RasterStatus status) noexcept
{
    return status == RasterStatus::Ready || status == RasterStatus::Pending;
}

} // namespace

TEST_CASE("Guided FG shader weights, density and backprojection match the CPU oracle",
          "[renderer][gpu][guided-fg][parity]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    constexpr usize INVOCATION_COUNT = 512;
    constexpr usize RECORDS_PER_INVOCATION = 3;
    constexpr usize WORDS_PER_RECORD = 4;
    constexpr usize RESULT_BYTES = INVOCATION_COUNT * RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
    constexpr usize GROUP_WIDTH = 64;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Compute.MinStorageRange = RESULT_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No native GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(RESULT_BYTES, uint8{0}) == true);
    rhi::BufferHandle results;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = RESULT_BYTES },
                                         {bytes.GetData(), bytes.GetSize()},
                                         results)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::guided_fg_parity::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    const rhi::RasterBindingResource resource{ .Binding = 0, .Buffer = results, .Size = RESULT_BYTES };
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, {&resource, 1}, set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);
    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == RasterStatus::Ready);
    rhi::GraphVersion original;
    rhi::GraphVersion version;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, original) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, original, version) == RasterStatus::Ready);
    const rhi::GraphUse use{version,
                            rhi::GraphAccessMode::StorageReadWrite,
                            0,
                            RESULT_BYTES,
                            rhi::RasterVisibility::Compute};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {INVOCATION_COUNT / GROUP_WIDTH, 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "Guided FG production math parity";
    pass.Uses = &use;
    pass.UseCount = 1;
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == RasterStatus::Ready);
    rhi::GraphReport graphReport;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, graphReport) == RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    RasterStatus executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    const uint64 beginning = time::NowTicks();
    while (executed == RasterStatus::NotReady && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == RasterStatus::Ready);
    WaitReady(device, completion);
    for (usize offset = 0; offset < RESULT_BYTES; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = RESULT_BYTES - offset < READBACK_CHUNK_BYTES ? RESULT_BYTES - offset : READBACK_CHUNK_BYTES;
        rhi::ReadbackTicket ticket;
        RasterStatus status = rhi::RequestBufferReadback(device, results, offset, size, ticket);
        const uint64 start = time::NowTicks();
        while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
        {
            REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
            status = rhi::GetStatus(device, ticket);
        }
        REQUIRE(status == RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, bytes.GetData() + offset, size) == RasterStatus::Ready);
        REQUIRE(rhi::Release(device, ticket) == RasterStatus::Ready);
    }
    using namespace ludus::graphics::renderer::testing;
    constexpr float64 RELATIVE_TOLERANCE = 2e-5;
    constexpr usize PIXEL_ROW = 30;
    constexpr usize PROJECTION_WIDTH = 64;
    constexpr float64 PROJECTION_JACOBIAN = 256;
    for (usize index = 0; index < INVOCATION_COUNT; ++index)
    {
        CAPTURE(index);
        StaticArray<math::Vector4, RECORDS_PER_INVOCATION> records{};
        const usize offset = index * sizeof(records);
        std::memcpy(records.GetData(), bytes.GetData() + offset, sizeof(records));
        const float64 scale =
            ComputeFgOracleFluxScale(static_cast<uint32>(index % 17 + 1), static_cast<uint32>(index % 7));
        const float64 jacobian =
            ComputeFgOracleJacobian({0, 0, 1}, {1 + static_cast<float64>(index % 5) * 0.1, 0, 2}, {0, 0, 1});
        const float64 weight = ComputeFgOraclePairWeight(2, 3, 0.5 + static_cast<float64>(index % 11) * 0.1, 1);
        CHECK(math::Abs(static_cast<float64>(records[0].X) - scale) <= RELATIVE_TOLERANCE * scale);
        CHECK(math::Abs(static_cast<float64>(records[0].Y) - jacobian) <= RELATIVE_TOLERANCE * jacobian);
        CHECK(math::Abs(static_cast<float64>(records[0].Z) - weight) <= RELATIVE_TOLERANCE * weight);
        StaticArray<uint32, WORDS_PER_RECORD> projection{};
        std::memcpy(projection.GetData(), &records[1], sizeof(projection));
        const bool shouldProject = index % 13 != 0 && index % 7 != 0;
        CHECK(projection[2] == (shouldProject == true ? 1 : 0));
        if (shouldProject == true)
        {
            const float64 x = static_cast<float64>(index % 19) * 0.1 - 0.91;
            const usize pixelX = static_cast<usize>((x * 0.5 + 1) * (PROJECTION_WIDTH / 2));
            CHECK(projection[0] == PIXEL_ROW * PROJECTION_WIDTH + pixelX);
            CHECK(math::Abs(static_cast<float64>(records[1].Y) - PROJECTION_JACOBIAN) <
                  RELATIVE_TOLERANCE * PROJECTION_JACOBIAN);
        }
        CHECK(projection[3] == (index % 2 == 0 ? 16 : 0));
        // Lambertian f=1/pi times photon flux / disc area, in raw linear RGB.
        const float64 density = 32 / (math::kPi * math::kPi);
        CHECK(math::Abs(static_cast<float64>(records[2].X) - density) < RELATIVE_TOLERANCE * density);
        CHECK(math::Abs(static_cast<float64>(records[2].Y) - density * 1.5) < RELATIVE_TOLERANCE * density);
        CHECK(math::Abs(static_cast<float64>(records[2].Z) - density * 2) < RELATIVE_TOLERANCE * density);
    }
}
