// Ordered-graph execution pilot (RenderGraphExecution::OrderedGraph): the cooked offscreen ColorAndDepth
// fixture lowered to one RHI ordered graph per frame must reproduce the direct path's color and depth bytes,
// report its cooked-to-RHI pass and logical-to-physical resource mapping, reject before recording when a
// scene exceeds the device's graph limits, and serve same-frame and delayed export consumers through frames
// in flight and resized generations. See "Logical resource uses and submitted work" in
// docs/architecture/render-graph-authoring.md.

#include "render_graph_gpu_fixture.h"
#include "render_graph_scratch.h"
#include "renderer_composite.h"
#include "rg2_depth_bits.h"
#include "rg2_offscreen.h"
#include "rg2_surface.h"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

#include <new>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
namespace scratch = ludus::render_graph_scratch;
namespace
{
constexpr uint64 TIMEOUT_SECONDS = 20;
/// The exported color is logical resource zero of the offscreen fixture.
constexpr uint32 EXPORTED_COLOR = 0;
/// One attachment backing, the view's instance table and one mesh's vertex and index buffers.
constexpr uint32 SINGLE_MESH_RESOURCES = 4;
/// Renders submitted back to back without waiting, so each may find its predecessor in flight.
constexpr usize IN_FLIGHT_RENDERS = 8;
constexpr usize EXECUTION_COUNT = 2;
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
bool HasTimedOut(uint64 start) noexcept
{
    return time::NowTicks() - start > TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
}
/// Wait for the graph's producer and copy every byte of one RGBA8 texture through the RHI.
void ReadColor(rhi::DeviceHandle device,
               rhi::TextureHandle texture,
               uint32 width,
               uint32 height,
               Array<uint8>& outPixels)
{
    const uint64 start = time::NowTicks();
    rhi::ReadbackTicket ticket;
    const usize rowPitch = usize{width} * TEXEL_BYTES;
    REQUIRE(IsAccepted(
                rhi::RequestTextureReadback(device, texture, { .Width = width, .Height = height }, rowPitch, ticket)) ==
            true);
    Array<uint8> pixels;
    REQUIRE(pixels.TryResize(rowPitch * height) == true);
    rhi::RasterStatus copied = rhi::RasterStatus::Pending;
    while (copied == rhi::RasterStatus::Pending || copied == rhi::RasterStatus::NotReady)
    {
        REQUIRE(HasTimedOut(start) == false);
        static_cast<void>(rhi::PollLifetime(device));
        copied = rhi::CopyReadback(device, ticket, pixels.GetData(), pixels.GetSize());
    }
    REQUIRE(copied == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(device, ticket) == rhi::RasterStatus::Ready);
    outPixels = Move(pixels);
}
/// Wait for a completed export and read its color bytes.
void ReadExport(rhi::DeviceHandle device,
                const renderer::RenderGraph& graph,
                uint32 width,
                uint32 height,
                Array<uint8>& outPixels)
{
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect = rhi::TextureAspect::Depth;
    const uint64 start = time::NowTicks();
    while (graph.GetExport(EXPORTED_COLOR, texture, aspect) != rhi::RasterStatus::Ready)
    {
        REQUIRE(HasTimedOut(start) == false);
        static_cast<void>(rhi::PollLifetime(device));
    }
    REQUIRE(aspect == rhi::TextureAspect::Color);
    ReadColor(device, texture, width, height, outPixels);
}
/// Count differing bytes, and require some pixel to differ from the clear color so a drawn image is compared.
usize CountMismatches(const Array<uint8>& expected, const Array<uint8>& actual)
{
    REQUIRE(expected.GetSize() == actual.GetSize());
    usize mismatches = 0;
    bool hasCoverage = false;
    for (usize i = 0; i < expected.GetSize(); ++i)
    {
        mismatches += expected[i] != actual[i] ? 1U : 0U;
        hasCoverage = hasCoverage == true || expected[i] != expected[i % TEXEL_BYTES];
    }
    CHECK(hasCoverage == true);
    return mismatches;
}
/// The exported color and the stored depth bits of one rendered frame.
struct Capture final
{
    Array<uint8> Color;
    StaticArray<uint8, PIXEL_BYTES> Depth{};
};
void RenderAndCapture(GraphGpuFixture& inoutFixture, uint32 depthResource, Capture& outCapture)
{
    rhi::SubmissionToken completion;
    REQUIRE(inoutFixture.Graph.Render(inoutFixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    ReadExport(inoutFixture.Device, inoutFixture.Graph, WIDTH, HEIGHT, outCapture.Color);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect = rhi::TextureAspect::Color;
    REQUIRE(inoutFixture.Graph.GetResourceAllocation(depthResource, texture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Depth);
    DepthDisplay display{inoutFixture.Device, shaders::rg2_depth_bits::Vertex(), shaders::rg2_depth_bits::Fragment()};
    display.Show(inoutFixture.Surface, texture, outCapture.Depth);
}
void InitializeOffscreen(GraphGpuFixture& inoutFixture)
{
    REQUIRE(inoutFixture.Graph.Initialize(inoutFixture.Device,
                                          packages::rg2_offscreen::MakePlan(),
                                          packages::rg2_offscreen::MakePrograms(
                                              shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment())) == rhi::RasterStatus::Ready);
}
} // namespace
TEST_CASE("Ordered-graph pilot reproduces the direct offscreen color and depth and reports its mapping",
          "[render-graph][gpu][ordered-graph]")
{
    GraphGpuFixture fixture;
    fixture.Items[0].Material.Color = {.25F, .5F, .75F, 1};
    InitializeOffscreen(fixture);
    const renderer::RenderGraphPlan plan = packages::rg2_offscreen::MakePlan();
    REQUIRE(plan.Passes.GetSize() == 1);
    const uint32 depthResource = plan.Passes[0].Depth;
    const uint32 colorResource = plan.Passes[0].Color;
    CHECK(fixture.Graph.GetExecutionReport().Execution == renderer::RenderGraphExecution::Direct);
    CHECK(fixture.Graph.GetExecutionReport().Result == rhi::RasterStatus::NotReady);
    for (uint64 revision = 1; revision <= 2; ++revision)
    {
        // The second revision moves the triangle: both paths follow the same edit.
        fixture.Items[0].MeshToWorld.Translation.X = revision == 1 ? 0.0F : 0.25F;
        fixture.Update(revision);
        Capture direct;
        REQUIRE(fixture.Graph.SetExecution(renderer::RenderGraphExecution::Direct) == rhi::RasterStatus::Ready);
        RenderAndCapture(fixture, depthResource, direct);
        Capture ordered;
        REQUIRE(fixture.Graph.SetExecution(renderer::RenderGraphExecution::OrderedGraph) == rhi::RasterStatus::Ready);
        RenderAndCapture(fixture, depthResource, ordered);
        CHECK(CountMismatches(direct.Color, ordered.Color) == 0);
        bool hasGeometryDepth = false;
        usize depthMismatches = 0;
        for (usize row = 0; row < HEIGHT; ++row)
        {
            for (usize column = 0; column < WIDTH; ++column)
            {
                const float32 stored = ReadDepthBits(ordered.Depth, column, row);
                hasGeometryDepth = hasGeometryDepth == true || stored != 1.0F;
                depthMismatches += stored != ReadDepthBits(direct.Depth, column, row) ? 1U : 0U;
            }
        }
        CHECK(depthMismatches == 0);
        CHECK(hasGeometryDepth == true);
        CHECK(fixture.Graph.GetOutputSceneRevision() == revision);
    }
    const renderer::RenderGraphExecutionReport report = fixture.Graph.GetExecutionReport();
    CHECK(report.Execution == renderer::RenderGraphExecution::OrderedGraph);
    CHECK(report.Result == rhi::RasterStatus::Ready);
    CHECK(report.FailedPass == format::ABSENT);
    CHECK(report.DrawCount == 1);
    CHECK(report.ResourceCount == SINGLE_MESH_RESOURCES);
    CHECK(report.DrawLimit == rhi::DEFAULT_GRAPH_PACKET_COUNT);
    CHECK(report.ResourceLimit == rhi::DEFAULT_GRAPH_RESOURCE_COUNT);
    CHECK(report.RasterPasses == 1);
    CHECK(report.DependencyCount > 0);
    CHECK(report.GraphPasses[0] == 0);
    for (usize pass = 1; pass < format::MAX_PASSES; ++pass)
    {
        CHECK(report.GraphPasses[pass] == format::ABSENT);
    }
    // Logical color and depth share one physical backing: RHI resource zero, live in cooked pass zero.
    const StaticArray<uint32, 2> attachments{{colorResource, depthResource}};
    for (const uint32 resource : attachments)
    {
        CHECK(report.GraphResources[resource] == 0);
        CHECK(report.FirstPass[resource] == 0);
        CHECK(report.LastPass[resource] == 0);
    }
    for (usize resource = plan.Resources.GetSize(); resource < format::MAX_RESOURCES; ++resource)
    {
        CHECK(report.GraphResources[resource] == format::ABSENT);
    }
}
TEST_CASE("Ordered-graph pilot rejects a plan or scene before recording and keeps the last output",
          "[render-graph][gpu][ordered-graph]")
{
    // One mesh fits exactly; a second mesh adds two buffers beyond the graph's resource limit.
    rhi::StorageLimits storage;
    storage.ResourcesPerGraph = SINGLE_MESH_RESOURCES;
    GraphGpuFixture fixture{storage};
    SECTION("a plan outside the pilot profile keeps direct execution")
    {
        REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                         packages::rg2_surface::MakePlan(),
                                         packages::rg2_surface::MakePrograms(
                                             shaders::renderer_composite::Vertex(),
                                             shaders::renderer_composite::Fragment())) == rhi::RasterStatus::Ready);
        CHECK(fixture.Graph.SetExecution(renderer::RenderGraphExecution::OrderedGraph) ==
              rhi::RasterStatus::Unsupported);
        fixture.Update(1);
        fixture.Render();
        CHECK(fixture.Graph.GetExecutionReport().Execution == renderer::RenderGraphExecution::Direct);
    }
    SECTION("a scene beyond the device's graph limits names the counts and recovers after repair")
    {
        renderer::RenderGraph uninitialized;
        CHECK(uninitialized.SetExecution(renderer::RenderGraphExecution::OrderedGraph) ==
              rhi::RasterStatus::InvalidState);
        InitializeOffscreen(fixture);
        REQUIRE(fixture.Graph.SetExecution(renderer::RenderGraphExecution::OrderedGraph) == rhi::RasterStatus::Ready);
        fixture.Update(1);
        Array<uint8> before;
        rhi::SubmissionToken completion;
        REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        ReadExport(fixture.Device, fixture.Graph, WIDTH, HEIGHT, before);
        // A second item on its own mesh: three buffers per pass plus the attachment exceed the limit.
        StaticArray<math::Vector3, VERTEX_COUNT> shifted = fixture.Positions;
        for (math::Vector3& position : shifted)
        {
            position.X += .25F;
        }
        StaticArray<renderer::RenderGraphMesh, 2> meshes{};
        meshes[0] = fixture.Meshes[0];
        meshes[1].Geometry = {shifted.GetData(),
                              shifted.GetSize(),
                              fixture.Indices.GetData(),
                              fixture.Indices.GetSize()};
        StaticArray<renderer::RenderGraphItem, 2> items{};
        items[0] = fixture.Items[0];
        items[1] = fixture.Items[0];
        items[1].SourceId = 2;
        items[1].MeshIndex = 1;
        renderer::RenderGraphReport scene;
        REQUIRE(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                     {items.GetData(), items.GetSize()},
                                     {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                     2,
                                     renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                     scene) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken rejected;
        CHECK(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, rejected) == rhi::RasterStatus::CapacityExceeded);
        CHECK(rejected.IsNull() == true);
        const renderer::RenderGraphExecutionReport report = fixture.Graph.GetExecutionReport();
        CHECK(report.Result == rhi::RasterStatus::CapacityExceeded);
        CHECK(report.FailedPass == 0);
        CHECK(report.DrawCount == 2);
        CHECK(report.ResourceCount == SINGLE_MESH_RESOURCES + 2);
        CHECK(report.ResourceLimit == SINGLE_MESH_RESOURCES);
        CHECK(report.GraphPasses[0] == format::ABSENT);
        // No command was recorded: the previous scene's export is still the valid output.
        CHECK(fixture.Graph.GetOutputSceneRevision() == 1);
        Array<uint8> kept;
        ReadExport(fixture.Device, fixture.Graph, WIDTH, HEIGHT, kept);
        CHECK(CountMismatches(before, kept) == 0);
        // Repair: one mesh again renders through the ordered graph.
        fixture.Update(3);
        rhi::SubmissionToken repaired;
        REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, repaired) == rhi::RasterStatus::Ready);
        CHECK(fixture.Graph.GetExecutionReport().Result == rhi::RasterStatus::Ready);
        Array<uint8> after;
        ReadExport(fixture.Device, fixture.Graph, WIDTH, HEIGHT, after);
        CHECK(CountMismatches(before, after) == 0);
        CHECK(fixture.Graph.GetOutputSceneRevision() == 3);
    }
}
TEST_CASE("Ordered-graph pilot exports serve same-frame and delayed consumers through frames in flight",
          "[render-graph][gpu][ordered-graph]")
{
    GraphGpuFixture fixture;
    fixture.Items[0].Material.Color = {.25F, .5F, .75F, 1};
    InitializeOffscreen(fixture);
    fixture.Update(1);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect = rhi::TextureAspect::Depth;
    REQUIRE(fixture.Graph.GetExportAllocation(EXPORTED_COLOR, texture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Color);
    renderer::Renderer compositor;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_offscreen::MakePrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
    REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
            rhi::RasterStatus::Ready);
    REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                       shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::Presentation presentation;
    REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, EXECUTION_COUNT> sameFrame{};
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, EXECUTION_COUNT> delayed{};
    StaticArray<Array<uint8>, EXECUTION_COUNT> inFlight{};
    const StaticArray<renderer::RenderGraphExecution, EXECUTION_COUNT> executions{
        {renderer::RenderGraphExecution::Direct, renderer::RenderGraphExecution::OrderedGraph}};
    for (usize mode = 0; mode < EXECUTION_COUNT; ++mode)
    {
        REQUIRE(fixture.Graph.SetExecution(executions[mode]) == rhi::RasterStatus::Ready);
        // Same-frame consumer: the compositor samples the export after the graph writes it in one frame.
        const uint64 start = time::NowTicks();
        rhi::RasterStatus prepared = fixture.Graph.PrepareEncoding();
        while (prepared == rhi::RasterStatus::NotReady)
        {
            REQUIRE(HasTimedOut(start) == false);
            static_cast<void>(rhi::PollLifetime(fixture.Device));
            prepared = fixture.Graph.PrepareEncoding();
        }
        REQUIRE(prepared == rhi::RasterStatus::Ready);
        REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
        REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
        REQUIRE(IsAccepted(fixture.Graph.ConfirmSubmission(completion)) == true);
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(sameFrame[mode])) == true);
        // Delayed consumer: a later frame samples the completed export; the next graph frame overwrites it
        // after that read, ordered by the shared RHI ledger without a host wait for the consumer.
        rhi::TextureHandle completed;
        while (fixture.Graph.GetExport(EXPORTED_COLOR, completed, aspect) != rhi::RasterStatus::Ready)
        {
            REQUIRE(HasTimedOut(start) == false);
            static_cast<void>(rhi::PollLifetime(fixture.Device));
        }
        REQUIRE(completed == texture);
        REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken consumed;
        REQUIRE(rhi::EndFrame(fixture.Device, consumed) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(MakeSpan(delayed[mode])) == true);
        // Frames in flight: render again and again without waiting. A frame whose predecessor is still in
        // flight is refused with NotReady before recording; the graph never overwrites a pending export.
        usize rendered = 0;
        for (usize frame = 0; frame < IN_FLIGHT_RENDERS; ++frame)
        {
            rhi::SubmissionToken next;
            const rhi::RasterStatus status = fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, next);
            REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::NotReady));
            rendered += status == rhi::RasterStatus::Ready ? 1U : 0U;
        }
        CHECK(rendered > 0);
        ReadExport(fixture.Device, fixture.Graph, WIDTH, HEIGHT, inFlight[mode]);
    }
    usize sameFrameMismatches = 0;
    usize delayedMismatches = 0;
    for (usize i = 0; i < PIXEL_BYTES; ++i)
    {
        sameFrameMismatches += sameFrame[0][i] != sameFrame[1][i] ? 1U : 0U;
        delayedMismatches += delayed[0][i] != delayed[1][i] ? 1U : 0U;
    }
    CHECK(sameFrameMismatches == 0);
    CHECK(delayedMismatches == 0);
    CHECK(CountMismatches(inFlight[0], inFlight[1]) == 0);
    REQUIRE(compositor.Release(presentation) == rhi::RasterStatus::Ready);
}
TEST_CASE("Ordered-graph pilot follows resized generations retired with frames in flight",
          "[render-graph][gpu][ordered-graph]")
{
    GraphGpuFixture fixture;
    fixture.Items[0].Material.Color = {.25F, .5F, .75F, 1};
    filesystem::Directory directory;
    REQUIRE(directory.Open(LUDUS_RG3_OFFSCREEN_DIRECTORY).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead("rg2_offscreen.lurgrt", file).Succeeded() == true);
    Array<uint8> offscreen;
    REQUIRE(offscreen.TryResize(static_cast<usize>(file.Size())) == true);
    REQUIRE(file.ReadAt(0, MakeSpan(offscreen)).BytesRead == offscreen.GetSize());
    // Alternate two extents; each resize is a reload of a generation with the other extent.
    constexpr usize STEP_COUNT = 4;
    StaticArray<StaticArray<Array<uint8>, STEP_COUNT>, EXECUTION_COUNT> exports{};
    const StaticArray<renderer::RenderGraphExecution, EXECUTION_COUNT> executions{
        {renderer::RenderGraphExecution::Direct, renderer::RenderGraphExecution::OrderedGraph}};
    for (usize mode = 0; mode < EXECUTION_COUNT; ++mode)
    {
        renderer::RenderGraphActivation activation;
        REQUIRE(activation.Initialize(fixture.Device,
                                      {packages::rg2_offscreen::INTERFACE_IDENTITY.GetData(),
                                       packages::rg2_offscreen::INTERFACE_IDENTITY.GetSize()},
                                      shaders::renderer_composite::Vertex(),
                                      shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
        uint64 revision = 0;
        for (usize step = 0; step < STEP_COUNT; ++step)
        {
            const uint32 width = step % 2 == 0 ? WIDTH : HEIGHT;
            const uint32 height = step % 2 == 0 ? HEIGHT : WIDTH;
            Array<uint8> bytes;
            REQUIRE(scratch::WriteScratchGeneration({offscreen.GetData(), offscreen.GetSize()},
                                                    {0, width, height},
                                                    bytes) == true);
            renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
            core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
            REQUIRE(package.Get() != nullptr);
            REQUIRE(package->Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
            const uint64 serial = activation.GetReport().CandidateGeneration + 1;
            REQUIRE(IsAccepted(activation.Stage(Move(package))) == true);
            uint64 updated = 0;
            const uint64 start = time::NowTicks();
            renderer::RenderGraph* activeOrNull = nullptr;
            while (activeOrNull == nullptr || activeOrNull->GetDrawableSceneRevision() == 0 ||
                   activation.GetReport().ActiveGeneration != serial)
            {
                REQUIRE(HasTimedOut(start) == false);
                static_cast<void>(rhi::PollLifetime(fixture.Device));
                static_cast<void>(activation.Poll());
                const renderer::RenderGraphActivationReport report = activation.GetReport();
                REQUIRE(report.Candidate != renderer::RenderGraphCandidateState::Failed);
                activeOrNull = activation.GetActiveOrNull();
                const bool isActive = report.ActiveGeneration == serial && activeOrNull != nullptr;
                if ((report.Candidate == renderer::RenderGraphCandidateState::NeedsUpdate ||
                     (isActive == true && activeOrNull->GetDrawableSceneRevision() == 0)) &&
                    updated != serial)
                {
                    renderer::RenderGraphReport scene;
                    const rhi::RasterStatus status =
                        activation.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                          {fixture.Items.GetData(), fixture.Items.GetSize()},
                                          {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                          ++revision,
                                          renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                          scene);
                    REQUIRE((IsAccepted(status) == true || status == rhi::RasterStatus::NotReady));
                    updated = IsAccepted(status) == true ? serial : updated;
                }
            }
            renderer::RenderGraph& graph = *activeOrNull;
            REQUIRE(graph.SetExecution(executions[mode]) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
            // Without a wait, the next reload retires this generation while its frame may be in flight.
            if (step + 1 == STEP_COUNT || step % 2 == 0)
            {
                ReadExport(fixture.Device, graph, width, height, exports[mode][step]);
            }
            if (executions[mode] == renderer::RenderGraphExecution::OrderedGraph)
            {
                CHECK(graph.GetExecutionReport().Result == rhi::RasterStatus::Ready);
            }
        }
    }
    for (usize step = 0; step < STEP_COUNT; ++step)
    {
        if (exports[0][step].IsEmpty() == false)
        {
            CHECK(CountMismatches(exports[0][step], exports[1][step]) == 0);
        }
    }
}
