// RG4 exact-descriptor transient pooling on the deterministic reference RHI backend.
// The reference backend completes submissions only when a test advances
// reference::Completed, so frames in flight and completion proof are exercised
// exactly rather than by timing.
#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace ludus::graphics::rhi::backend
{
extern uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
extern bool SelectionSupported;
} // namespace ludus::graphics::rhi::backend
namespace
{
constexpr usize INPUT_COUNT = 6;
constexpr usize COMPOSITE_INPUT_COUNT = 1;
constexpr usize COMPOSITE_BINDING_COUNT = 2;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
/// Transient backings in the default scratch plan, as in the cooked rg2/scratch fixture.
constexpr uint32 SCRATCH_COUNT = 2;
constexpr usize TEXTURE = static_cast<usize>(rhi::internal::RasterKind::Texture);
/// Exported color + depth, then `scratch` unexported color + depth backings, each written by
/// its own ColorAndDepth pass of the single camera. Scratch passes run before the exported one.
/// Every pass approves repeated coverage, the numeric plan Renderer admits for this workload;
/// lowering never emits the approval, so no cooked graph carries such transients today.
struct ScratchPlan final
{
    StaticArray<format::Resource, format::MAX_RESOURCES> Resources{};
    StaticArray<format::Pass, format::MAX_PASSES> Passes{};
    StaticArray<format::Camera, 1> Cameras{{{0}}};
    uint32 ResourceCount = 0;
    uint32 PassCount = 0;
    ScratchPlan(uint32 scratch, uint32 width, uint32 height) noexcept
    {
        const auto addBacking = [&](bool isExported) noexcept {
            const uint32 color = ResourceCount;
            Resources[ResourceCount++] = {width, height, format::Aspect::Color, color, 0, isExported == true ? 1U : 0U};
            Resources[ResourceCount++] = {width, height, format::Aspect::Depth, color, 0, 0};
            return color;
        };
        const uint32 exported = addBacking(true);
        for (uint32 i = 0; i < scratch; ++i)
        {
            const uint32 color = addBacking(false);
            Passes[PassCount++] = {format::Technique::ColorAndDepth,
                                   0,
                                   color,
                                   color + 1,
                                   format::Load::Clear,
                                   format::Store::Store,
                                   format::Load::Clear,
                                   format::Store::Store,
                                   1};
        }
        Passes[PassCount++] = {format::Technique::ColorAndDepth,
                               0,
                               exported,
                               exported + 1,
                               format::Load::Clear,
                               format::Store::Store,
                               format::Load::Clear,
                               format::Store::Store,
                               1};
    }
    [[nodiscard]] renderer::RenderGraphPlan MakePlan() const noexcept
    {
        return {{format::VERSION, ResourceCount, PassCount, 1, 0, 0, 0, 0},
                {Resources.GetData(), ResourceCount},
                {Cameras.GetData(), Cameras.GetSize()},
                {Passes.GetData(), PassCount},
                {},
                {},
                {}};
    }
};
struct PoolFixture final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    renderer::RenderGraphAttachmentPool Pool;
    StaticArray<rhi::RasterShaderInput, INPUT_COUNT> Inputs{};
    StaticArray<rhi::RasterShaderInput, COMPOSITE_INPUT_COUNT> CompositeInputs{{{0, rhi::RasterVertexFormat::Float4}}};
    StaticArray<rhi::RasterBinding, COMPOSITE_BINDING_COUNT> CompositeBindings{
        {{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
         {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}}};
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{{{-.1F, -.1F, .5F}, {.1F, -.1F, .5F}, {0, .1F, .5F}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    uint64 Revision = 0;
    PoolFixture()
    {
        StartDevice();
        for (uint32 i = 0; i < INPUT_COUNT; ++i)
        {
            Inputs[i] = {i, i == 0 ? rhi::RasterVertexFormat::Float3 : rhi::RasterVertexFormat::Float4};
        }
        Meshes[0].Geometry.Positions = Positions.GetData();
        Meshes[0].Geometry.VertexCount = Positions.GetSize();
        Meshes[0].Geometry.Indices = Indices.GetData();
        Meshes[0].Geometry.IndexCount = Indices.GetSize();
        Items[0].SourceId = 42;
        Items[0].Material.Color = {1, 1, 1, 1};
        REQUIRE(Pool.Initialize(Device) == rhi::RasterStatus::Ready);
    }
    ~PoolFixture()
    {
        Pool.Reset();
        rhi::Shutdown();
    }
    void StartDevice()
    {
        rhi::Shutdown();
        rhi::reference::Reset();
        Device = {};
        Surface = {};
        rhi::reference::ProfileAvailable = true;
        rhi::backend::ImmediateError = rhi::StartupError::None;
        rhi::backend::NextFrame = rhi::FrameStatus::Ready;
        rhi::backend::ActiveKind = rhi::Backend::WebGPU;
        rhi::backend::SelectionSupported = true;
        REQUIRE(rhi::CreateDevice({}, {}, {}, Device, Surface) == rhi::DeviceStatus::Pending);
        rhi::internal::Complete(rhi::backend::PendingToken, rhi::StartupError::None, {4096, 16384});
    }
    [[nodiscard]] renderer::RenderGraphPrograms MakePrograms() const
    {
        renderer::RenderGraphPrograms programs;
        programs.ColorVertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        programs.ColorVertex.Artifact.Wgsl = "flat vertex fixture";
        programs.ColorVertex.Artifact.WgslEntry = "VertexMain";
        programs.ColorVertex.Inputs = Inputs.AsSpan();
        programs.ColorFragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        programs.ColorFragment.Artifact.Wgsl = "flat fragment fixture";
        programs.ColorFragment.Artifact.WgslEntry = "FragmentMain";
        programs.CompositeVertex.Artifact.Stage = rhi::ShaderStage::Vertex;
        programs.CompositeVertex.Artifact.Wgsl = "composite vertex fixture";
        programs.CompositeVertex.Artifact.WgslEntry = "VertexMain";
        programs.CompositeVertex.Inputs = CompositeInputs.AsSpan();
        programs.CompositeFragment.Artifact.Stage = rhi::ShaderStage::Fragment;
        programs.CompositeFragment.Artifact.Wgsl = "composite fragment fixture";
        programs.CompositeFragment.Artifact.WgslEntry = "FragmentMain";
        programs.CompositeFragment.Bindings = CompositeBindings.AsSpan();
        return programs;
    }
    rhi::RasterStatus Initialize(renderer::RenderGraph& inoutGraph,
                                 const ScratchPlan& plan,
                                 renderer::RenderGraphAttachmentPool* poolOrNull)
    {
        return inoutGraph.Initialize(Device, plan.MakePlan(), MakePrograms(), poolOrNull);
    }
    /// Publish a new extraction and submit one frame; the submission stays in flight.
    void Draw(renderer::RenderGraph& inoutGraph)
    {
        renderer::RenderGraphReport report;
        REQUIRE(inoutGraph.Update({Meshes.GetData(), Meshes.GetSize()},
                                  {Items.GetData(), Items.GetSize()},
                                  {Cameras.GetData(), Cameras.GetSize()},
                                  ++Revision,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(inoutGraph.Render(Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    }
    static void CompleteSubmissions() noexcept
    {
        rhi::reference::Completed = rhi::reference::Submitted;
    }
};
} // namespace
TEST_CASE("RG4 scratch transients need approved repeated coverage, which lowering never emits", "[render-graph][pool]")
{
    PoolFixture fixture;
    ScratchPlan plan{SCRATCH_COUNT, WIDTH, HEIGHT};
    for (usize pass = 0; pass < plan.PassCount; ++pass)
    {
        plan.Passes[pass].AllowsRepeatedCoverage = 0;
    }
    renderer::RenderGraph graph;
    REQUIRE(fixture.Initialize(graph, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
    renderer::RenderGraphReport report;
    CHECK(graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                       {fixture.Items.GetData(), fixture.Items.GetSize()},
                       {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                       1,
                       renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                       report) == rhi::RasterStatus::Unsupported);
    CHECK(report.Conflicting == 2);
}
TEST_CASE("RG4 serial control creates every attachment and pools nothing", "[render-graph][pool]")
{
    PoolFixture fixture;
    const ScratchPlan plan{SCRATCH_COUNT, WIDTH, HEIGHT};
    for (usize generation = 0; generation < 3; ++generation)
    {
        const usize creates = rhi::reference::Creates[TEXTURE];
        renderer::RenderGraph graph;
        REQUIRE(fixture.Initialize(graph, plan, nullptr) == rhi::RasterStatus::Ready);
        const renderer::RenderGraphAllocationReport report = graph.GetAllocationReport();
        CHECK(report.IsPooled == false);
        CHECK(report.ExportAllocations == 1);
        CHECK(report.TransientAllocations == SCRATCH_COUNT);
        CHECK(report.CreatedAllocations == SCRATCH_COUNT + 1);
        CHECK(report.ReusedAllocations == 0);
        CHECK(rhi::reference::Creates[TEXTURE] == creates + SCRATCH_COUNT + 1);
        fixture.Draw(graph);
        PoolFixture::CompleteSubmissions();
    }
    CHECK(fixture.Pool.GetReport().CreatedAllocations == 0);
    CHECK(fixture.Pool.GetReport().IdleAllocations == 0);
}
TEST_CASE("RG4 pooled transients are reused only after RHI completion of their last frame", "[render-graph][pool]")
{
    PoolFixture fixture;
    const ScratchPlan plan{SCRATCH_COUNT, WIDTH, HEIGHT};
    {
        renderer::RenderGraph first;
        REQUIRE(fixture.Initialize(first, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
        CHECK(first.GetAllocationReport().IsPooled == true);
        CHECK(first.GetAllocationReport().CreatedAllocations == SCRATCH_COUNT + 1);
        CHECK(fixture.Pool.GetReport().LeasedAllocations == SCRATCH_COUNT);
        // Submitted but incomplete: this frame stays in flight while its graph is reset.
        fixture.Draw(first);
    }
    CHECK(fixture.Pool.GetReport().IdleAllocations == SCRATCH_COUNT);
    CHECK(fixture.Pool.GetReport().LeasedAllocations == 0);
    {
        // A second frame in flight: completion of the first frame's uses is still unproven.
        const usize creates = rhi::reference::Creates[TEXTURE];
        renderer::RenderGraph second;
        REQUIRE(fixture.Initialize(second, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
        CHECK(second.GetAllocationReport().ReusedAllocations == 0);
        CHECK(fixture.Pool.GetReport().DeferredAcquisitions == SCRATCH_COUNT);
        CHECK(rhi::reference::Creates[TEXTURE] == creates + SCRATCH_COUNT + 1);
    }
    CHECK(fixture.Pool.GetReport().IdleAllocations == 2 * SCRATCH_COUNT);
    PoolFixture::CompleteSubmissions();
    const usize creates = rhi::reference::Creates[TEXTURE];
    renderer::RenderGraph third;
    REQUIRE(fixture.Initialize(third, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
    CHECK(third.GetAllocationReport().ReusedAllocations == SCRATCH_COUNT);
    CHECK(third.GetAllocationReport().CreatedAllocations == 1);
    // Only the export is created; exports are never pooled.
    CHECK(rhi::reference::Creates[TEXTURE] == creates + 1);
    fixture.Draw(third);
    PoolFixture::CompleteSubmissions();
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    CHECK(third.GetExport(0, texture, aspect) == rhi::RasterStatus::Ready);
    const renderer::RenderGraphPoolReport report = fixture.Pool.GetReport();
    CHECK(report.CreatedAllocations == 2 * SCRATCH_COUNT);
    CHECK(report.ReusedAllocations == SCRATCH_COUNT);
    CHECK(report.LeasedAllocations == SCRATCH_COUNT);
    CHECK(report.IdleAllocations == SCRATCH_COUNT);
}
TEST_CASE("RG4 pool reuses only exact descriptors across repeated resize", "[render-graph][pool]")
{
    PoolFixture fixture;
    const ScratchPlan wide{SCRATCH_COUNT, WIDTH, HEIGHT};
    const ScratchPlan tall{SCRATCH_COUNT, HEIGHT, WIDTH};
    constexpr usize CYCLES = 4;
    for (usize cycle = 0; cycle < CYCLES; ++cycle)
    {
        for (const ScratchPlan* plan : {&wide, &tall})
        {
            const usize creates = rhi::reference::Creates[TEXTURE];
            renderer::RenderGraph graph;
            REQUIRE(fixture.Initialize(graph, *plan, &fixture.Pool) == rhi::RasterStatus::Ready);
            const uint32 reused = graph.GetAllocationReport().ReusedAllocations;
            // The other extent's idle entries never match; each extent reuses its own after warm-up.
            CHECK(reused == (cycle == 0 ? 0U : SCRATCH_COUNT));
            CHECK(rhi::reference::Creates[TEXTURE] == creates + 1 + SCRATCH_COUNT - reused);
            fixture.Draw(graph);
            PoolFixture::CompleteSubmissions();
        }
    }
    const renderer::RenderGraphPoolReport report = fixture.Pool.GetReport();
    CHECK(report.CreatedAllocations == 2 * SCRATCH_COUNT);
    CHECK(report.ReusedAllocations == (2 * CYCLES - 2) * SCRATCH_COUNT);
    CHECK(report.DeferredAcquisitions == 0);
    CHECK(report.IdleAllocations == 2 * SCRATCH_COUNT);
}
TEST_CASE("RG4 exported backings are never pooled, whichever aspect is exported", "[render-graph][pool]")
{
    PoolFixture fixture;
    ScratchPlan plan{SCRATCH_COUNT, WIDTH, HEIGHT};
    // Export the first scratch color and the second scratch's private depth instead.
    plan.Resources[2].IsExported = 1;
    plan.Resources[5].IsExported = 1;
    {
        renderer::RenderGraph graph;
        REQUIRE(fixture.Initialize(graph, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
        const renderer::RenderGraphAllocationReport report = graph.GetAllocationReport();
        CHECK(report.ExportAllocations == SCRATCH_COUNT + 1);
        CHECK(report.TransientAllocations == 0);
        CHECK(report.CreatedAllocations == SCRATCH_COUNT + 1);
        fixture.Draw(graph);
        PoolFixture::CompleteSubmissions();
    }
    const renderer::RenderGraphPoolReport report = fixture.Pool.GetReport();
    CHECK(report.CreatedAllocations == 0);
    CHECK(report.IdleAllocations == 0);
}
TEST_CASE("RG4 pool evicts the least recently released allocation at capacity", "[render-graph][pool]")
{
    PoolFixture fixture;
    constexpr uint32 CAPACITY = 2 * SCRATCH_COUNT;
    renderer::RenderGraphAttachmentPool pool;
    CHECK(pool.Initialize(fixture.Device, 0) == rhi::RasterStatus::InvalidDescription);
    CHECK(pool.Initialize(fixture.Device, renderer::RENDER_GRAPH_POOL_CAPACITY + 1) ==
          rhi::RasterStatus::InvalidDescription);
    REQUIRE(pool.Initialize(fixture.Device, CAPACITY) == rhi::RasterStatus::Ready);
    constexpr usize GRAPH_COUNT = 3;
    for (usize i = 0; i < GRAPH_COUNT; ++i)
    {
        // Distinct extents: no graph's transients match another's, so every release adds an entry.
        renderer::RenderGraph graph;
        const ScratchPlan plan{SCRATCH_COUNT, WIDTH + static_cast<uint32>(i), HEIGHT};
        REQUIRE(fixture.Initialize(graph, plan, &pool) == rhi::RasterStatus::Ready);
        CHECK(graph.GetAllocationReport().ReusedAllocations == 0);
    }
    CHECK(pool.GetReport().IdleAllocations == CAPACITY);
    CHECK(pool.GetReport().DestroyedAllocations == GRAPH_COUNT * SCRATCH_COUNT - CAPACITY);
    {
        // The first graph released first, so its entries were the ones evicted.
        renderer::RenderGraph first;
        REQUIRE(fixture.Initialize(first, {SCRATCH_COUNT, WIDTH, HEIGHT}, &pool) == rhi::RasterStatus::Ready);
        CHECK(first.GetAllocationReport().ReusedAllocations == 0);
    }
    renderer::RenderGraph third;
    REQUIRE(fixture.Initialize(third, {SCRATCH_COUNT, WIDTH + 2, HEIGHT}, &pool) == rhi::RasterStatus::Ready);
    CHECK(third.GetAllocationReport().ReusedAllocations == SCRATCH_COUNT);
}
TEST_CASE("RG4 trimming destroys idle attachments and leaves leased ones", "[render-graph][pool]")
{
    PoolFixture fixture;
    const ScratchPlan plan{SCRATCH_COUNT, WIDTH, HEIGHT};
    renderer::RenderGraph kept;
    REQUIRE(fixture.Initialize(kept, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
    {
        renderer::RenderGraph released;
        REQUIRE(fixture.Initialize(released, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
    }
    CHECK(fixture.Pool.GetReport().IdleAllocations == SCRATCH_COUNT);
    const usize destroys = rhi::reference::Destroys[TEXTURE];
    fixture.Pool.Trim();
    CHECK(fixture.Pool.GetReport().IdleAllocations == 0);
    CHECK(fixture.Pool.GetReport().LeasedAllocations == SCRATCH_COUNT);
    CHECK(fixture.Pool.GetReport().DestroyedAllocations == SCRATCH_COUNT);
    CHECK(rhi::reference::Destroys[TEXTURE] == destroys + SCRATCH_COUNT);
    fixture.Draw(kept);
}
TEST_CASE("RG4 graphs reject a pool that is unbound or outlived its device", "[render-graph][pool]")
{
    PoolFixture fixture;
    const ScratchPlan plan{SCRATCH_COUNT, WIDTH, HEIGHT};
    renderer::RenderGraphAttachmentPool unbound;
    renderer::RenderGraph graph;
    CHECK(fixture.Initialize(graph, plan, &unbound) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Pool.Initialize(fixture.Device) == rhi::RasterStatus::InvalidState);
    // A replacement device session: the old pool's device is no longer the RHI's owner.
    renderer::RenderGraphAttachmentPool stale;
    REQUIRE(stale.Initialize(fixture.Device) == rhi::RasterStatus::Ready);
    fixture.Pool.Reset();
    fixture.StartDevice();
    CHECK(fixture.Initialize(graph, plan, &stale) == rhi::RasterStatus::InvalidDescription);
    stale.Reset();
    REQUIRE(fixture.Pool.Initialize(fixture.Device) == rhi::RasterStatus::Ready);
    CHECK(fixture.Initialize(graph, plan, &fixture.Pool) == rhi::RasterStatus::Ready);
}
TEST_CASE("RG4 pooled and serial-control graphs issue identical commands", "[render-graph][pool]")
{
    PoolFixture fixture;
    const ScratchPlan plan{SCRATCH_COUNT, WIDTH, HEIGHT};
    constexpr usize GENERATIONS = 3;
    StaticArray<usize, 2> draws{};
    StaticArray<usize, 2> passes{};
    for (usize mode = 0; mode < 2; ++mode)
    {
        const usize drawsBefore = rhi::reference::Draws;
        const usize passesBefore = rhi::reference::Passes;
        for (usize generation = 0; generation < GENERATIONS; ++generation)
        {
            renderer::RenderGraph graph;
            REQUIRE(fixture.Initialize(graph, plan, mode == 0 ? nullptr : &fixture.Pool) == rhi::RasterStatus::Ready);
            fixture.Draw(graph);
            PoolFixture::CompleteSubmissions();
            CHECK(graph.GetOutputSceneRevision() == fixture.Revision);
        }
        draws[mode] = rhi::reference::Draws - drawsBefore;
        passes[mode] = rhi::reference::Passes - passesBefore;
    }
    CHECK(fixture.Pool.GetReport().ReusedAllocations == (GENERATIONS - 1) * SCRATCH_COUNT);
    CHECK(draws[0] == draws[1]);
    CHECK(passes[0] == passes[1]);
    CHECK(draws[0] == GENERATIONS * (SCRATCH_COUNT + 1));
}
