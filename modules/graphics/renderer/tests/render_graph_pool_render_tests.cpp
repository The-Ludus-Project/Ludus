// RG4 exact-descriptor transient pooling on the GPU: the pooled path and the serial,
// non-pooled control must export identical bytes through repeated generation reloads,
// alternating extents (resize) and generations retired with frames still in flight.
#include "render_graph_scratch.h"
#include "renderer_composite.h"
#include "rg2_bound.h"
#include "rg2_depth.h"
#include "rg2_masked_color.h"
#include "rg2_masked_depth.h"
#include "rg2_offscreen.h"
#include "rg2_surface.h"

#include <ludus/foundation/base/compiler.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <new>
#include <string_view>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace scratch = ludus::render_graph_scratch;
namespace
{
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr usize BYTES_PER_PIXEL = 4;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
/// Unexported scratch backings in every qualified generation.
constexpr uint32 SCRATCH_COUNT = 2;
/// Reloads whose exports are read back and compared, alternating two extents.
constexpr usize CAPTURED_RELOADS = 6;
/// Reloads rendered without waiting, so each generation retires with its frame in flight.
constexpr usize IN_FLIGHT_RELOADS = 4;
constexpr usize STEP_COUNT = CAPTURED_RELOADS + 1;
constexpr uint64 TIMEOUT_SECONDS = 20;
constexpr usize MODE_COUNT = 2;
/// Every admitted cooked RG2 fixture: surface, offscreen, depth, bound, masked color and masked depth.
constexpr usize COOKED_FIXTURE_COUNT = 6;
/// The exported color is logical resource zero of the offscreen fixture.
constexpr uint32 EXPORTED_COLOR = 0;
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
bool HasTimedOut(uint64 start) noexcept
{
    return time::NowTicks() - start > TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
}
struct RhiGuard final
{
    LUDUS_INLINE RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
    LUDUS_INLINE ~RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
};
/// Build-tree location of one runtime generation cooked by ludus_compile_render_graph(... RUNTIME_PACKAGE).
struct PackageLocation final
{
    std::string_view Directory; ///< Absolute output directory of the graph.
    std::string_view File;      ///< `<NAME>.lurgrt` inside Directory.
};
Array<uint8> ReadPackage(const PackageLocation& location)
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(location.Directory).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead(location.File, file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    const filesystem::ReadResult read = file.ReadAt(0, bytes.AsSpan());
    REQUIRE(read.Outcome.Succeeded() == true);
    REQUIRE(read.BytesRead == bytes.GetSize());
    return bytes;
}
struct PoolGpuFixture final
{
    RhiGuard Session;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    Array<uint8> Offscreen;
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    uint64 Revision = 0;
    PoolGpuFixture()
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            SKIP("No GPU adapter on this host");
        }
        REQUIRE(started == rhi::DeviceStatus::Ready);
        Offscreen = ReadPackage({LUDUS_RG3_OFFSCREEN_DIRECTORY, "rg2_offscreen.lurgrt"});
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Items[0].SourceId = 1;
        Items[0].Material.Color = {.25F, .5F, .75F, 1};
    }
    core::UniquePtr<renderer::RenderGraphPackage> MakeGeneration(uint32 width, uint32 height)
    {
        Array<uint8> bytes;
        REQUIRE(scratch::WriteScratchGeneration({Offscreen.GetData(), Offscreen.GetSize()},
                                                {SCRATCH_COUNT, width, height},
                                                bytes) == true);
        renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
        core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
        REQUIRE(package.Get() != nullptr);
        REQUIRE(package->Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
        return package;
    }
    rhi::RasterStatus Update(renderer::RenderGraphActivation& inoutActivation)
    {
        renderer::RenderGraphReport report;
        return inoutActivation.Update({Meshes.GetData(), Meshes.GetSize()},
                                      {Items.GetData(), Items.GetSize()},
                                      {Cameras.GetData(), Cameras.GetSize()},
                                      ++Revision,
                                      renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                      report);
    }
    /// Stage a generation and poll until it is active with a drawable extraction.
    renderer::RenderGraph& Reload(renderer::RenderGraphActivation& inoutActivation, uint32 width, uint32 height)
    {
        const uint64 serial = inoutActivation.GetReport().CandidateGeneration + 1;
        REQUIRE(IsAccepted(inoutActivation.Stage(MakeGeneration(width, height))) == true);
        uint64 updated = 0;
        const uint64 start = time::NowTicks();
        renderer::RenderGraph* activeOrNull = nullptr;
        while (activeOrNull == nullptr || activeOrNull->GetDrawableSceneRevision() == 0 ||
               inoutActivation.GetReport().ActiveGeneration != serial)
        {
            REQUIRE(HasTimedOut(start) == false);
            static_cast<void>(rhi::PollLifetime(Device));
            static_cast<void>(inoutActivation.Poll());
            const renderer::RenderGraphActivationReport report = inoutActivation.GetReport();
            REQUIRE(report.Candidate != renderer::RenderGraphCandidateState::Failed);
            activeOrNull = inoutActivation.GetActiveOrNull();
            const bool isActive = report.ActiveGeneration == serial && activeOrNull != nullptr;
            // Update the newly ready candidate (or the first active generation) once, then wait.
            if ((report.Candidate == renderer::RenderGraphCandidateState::NeedsUpdate ||
                 (isActive == true && activeOrNull->GetDrawableSceneRevision() == 0)) &&
                updated != serial)
            {
                const rhi::RasterStatus status = Update(inoutActivation);
                REQUIRE((IsAccepted(status) == true || status == rhi::RasterStatus::NotReady));
                updated = IsAccepted(status) == true ? serial : updated;
            }
        }
        return *activeOrNull;
    }
    void Render(renderer::RenderGraph& inoutGraph)
    {
        rhi::SubmissionToken completion;
        REQUIRE(inoutGraph.Render(Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    }
    /// Wait for the producer and copy every exported RGBA8 byte through the RHI.
    void ReadExport(const renderer::RenderGraph& graph, uint32 width, uint32 height, Array<uint8>& outPixels)
    {
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect = rhi::TextureAspect::Color;
        const uint64 start = time::NowTicks();
        while (graph.GetExport(EXPORTED_COLOR, texture, aspect) != rhi::RasterStatus::Ready)
        {
            REQUIRE(HasTimedOut(start) == false);
            static_cast<void>(rhi::PollLifetime(Device));
        }
        REQUIRE(aspect == rhi::TextureAspect::Color);
        rhi::ReadbackTicket ticket;
        const usize rowPitch = usize{width} * BYTES_PER_PIXEL;
        REQUIRE(
            IsAccepted(
                rhi::RequestTextureReadback(Device, texture, { .Width = width, .Height = height }, rowPitch, ticket)) ==
            true);
        Array<uint8> pixels;
        REQUIRE(pixels.TryResize(rowPitch * height) == true);
        rhi::RasterStatus copied = rhi::RasterStatus::Pending;
        while (copied == rhi::RasterStatus::Pending || copied == rhi::RasterStatus::NotReady)
        {
            REQUIRE(HasTimedOut(start) == false);
            static_cast<void>(rhi::PollLifetime(Device));
            copied = rhi::CopyReadback(Device, ticket, pixels.GetData(), pixels.GetSize());
        }
        REQUIRE(copied == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(Device, ticket) == rhi::RasterStatus::Ready);
        outPixels = Move(pixels);
    }
};
/// One whole qualification run in one allocation mode.
struct ModeRun final
{
    StaticArray<Array<uint8>, STEP_COUNT> Exports{};
    renderer::RenderGraphPoolReport Pool{};
    uint32 Reused = 0;
};
void RunMode(PoolGpuFixture& inoutFixture, renderer::RenderGraphAllocation allocation, ModeRun& outRun)
{
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(inoutFixture.Device,
                                  {packages::rg2_offscreen::INTERFACE_IDENTITY.GetData(),
                                   packages::rg2_offscreen::INTERFACE_IDENTITY.GetSize()},
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment(),
                                  allocation) == rhi::RasterStatus::Ready);
    for (usize step = 0; step < CAPTURED_RELOADS; ++step)
    {
        // Alternate the extent: each resize is a reload of a generation with the other extent.
        const uint32 width = step % 2 == 0 ? WIDTH : HEIGHT;
        const uint32 height = step % 2 == 0 ? HEIGHT : WIDTH;
        renderer::RenderGraph& graph = inoutFixture.Reload(activation, width, height);
        outRun.Reused += graph.GetAllocationReport().ReusedAllocations;
        inoutFixture.Render(graph);
        inoutFixture.ReadExport(graph, width, height, outRun.Exports[step]);
    }
    for (usize step = 0; step < IN_FLIGHT_RELOADS; ++step)
    {
        renderer::RenderGraph& graph = inoutFixture.Reload(activation, WIDTH, HEIGHT);
        outRun.Reused += graph.GetAllocationReport().ReusedAllocations;
        // No wait: the next reload retires this generation while its frame may be in flight.
        inoutFixture.Render(graph);
    }
    renderer::RenderGraph& last = inoutFixture.Reload(activation, WIDTH, HEIGHT);
    outRun.Reused += last.GetAllocationReport().ReusedAllocations;
    inoutFixture.Render(last);
    inoutFixture.ReadExport(last, WIDTH, HEIGHT, outRun.Exports[CAPTURED_RELOADS]);
    outRun.Pool = activation.GetPoolReport();
}
} // namespace
TEST_CASE("RG4 pooled and serial-control generations export identical bytes through reload, resize and frames in "
          "flight",
          "[render-graph][gpu][pool]")
{
    PoolGpuFixture fixture;
    StaticArray<ModeRun, MODE_COUNT> runs{};
    RunMode(fixture, renderer::RenderGraphAllocation::SerialControl, runs[0]);
    RunMode(fixture, renderer::RenderGraphAllocation::ExactPool, runs[1]);
    for (usize step = 0; step < STEP_COUNT; ++step)
    {
        const Array<uint8>& control = runs[0].Exports[step];
        const Array<uint8>& pooled = runs[1].Exports[step];
        REQUIRE(control.GetSize() == pooled.GetSize());
        usize mismatches = 0;
        bool hasCoverage = false;
        for (usize i = 0; i < control.GetSize(); ++i)
        {
            mismatches += control[i] != pooled[i] ? 1U : 0U;
            hasCoverage = hasCoverage == true || control[i] != control[i % BYTES_PER_PIXEL];
        }
        CHECK(mismatches == 0);
        // Some pixel differs from the clear color, so the comparison is over a drawn image.
        CHECK(hasCoverage == true);
    }
    CHECK(runs[0].Reused == 0);
    CHECK(runs[0].Pool.CreatedAllocations == 0);
    // Alternating extents reuse each extent's own set; the serial control never does.
    CHECK(runs[1].Reused > 0);
    CHECK(runs[1].Pool.ReusedAllocations == runs[1].Reused);
    // The last active generation still holds its scratch set.
    CHECK(runs[1].Pool.LeasedAllocations == SCRATCH_COUNT);
}
TEST_CASE("RG4 no admitted cooked graph has a transient attachment for the pool", "[render-graph][gpu][pool]")
{
    PoolGpuFixture fixture;
    renderer::RenderGraphAttachmentPool pool;
    REQUIRE(pool.Initialize(fixture.Device) == rhi::RasterStatus::Ready);
    const rhi::RasterShaderDescription vertex = shaders::renderer_composite::Vertex();
    const rhi::RasterShaderDescription fragment = shaders::renderer_composite::Fragment();
    struct Fixture final
    {
        renderer::RenderGraphPlan Plan;
        renderer::RenderGraphPrograms Programs;
        uint32 Exports = 0;
    };
    const StaticArray<Fixture, COOKED_FIXTURE_COUNT> fixtures{{
        {packages::rg2_surface::MakePlan(), packages::rg2_surface::MakePrograms(vertex, fragment), 0},
        {packages::rg2_offscreen::MakePlan(), packages::rg2_offscreen::MakePrograms(vertex, fragment), 1},
        {packages::rg2_depth::MakePlan(), packages::rg2_depth::MakePrograms(vertex, fragment), 1},
        {packages::rg2_bound::MakePlan(), packages::rg2_bound::MakePrograms(vertex, fragment), 0},
        {packages::rg2_masked_color::MakePlan(), packages::rg2_masked_color::MakePrograms(vertex, fragment), 1},
        {packages::rg2_masked_depth::MakePlan(), packages::rg2_masked_depth::MakePrograms(vertex, fragment), 1},
    }};
    for (const Fixture& cooked : fixtures)
    {
        renderer::RenderGraph graph;
        REQUIRE(IsAccepted(graph.Initialize(fixture.Device, cooked.Plan, cooked.Programs, &pool)) == true);
        const renderer::RenderGraphAllocationReport report = graph.GetAllocationReport();
        CHECK(report.IsPooled == true);
        // Surfaces allocate nothing; a depth-only export keeps its RGBA8 compatibility color.
        CHECK(report.ExportAllocations == cooked.Exports);
        CHECK(report.TransientAllocations == 0);
    }
    CHECK(pool.GetReport().CreatedAllocations == 0);
    CHECK(pool.GetReport().IdleAllocations == 0);
}
