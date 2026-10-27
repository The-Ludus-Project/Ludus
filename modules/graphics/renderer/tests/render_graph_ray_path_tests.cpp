#include "renderer_composite.h"
#include "rg2_ordered_ray_capture.h"
#include "rg2_ordered_ray_plain.h"
#include "rg2_ray_path_capture.h"
#include "rg2_ray_path_plain.h"
#include "rg2_raytracing_capture.h"
#include "rg2_raytracing_plain.h"
#include "rg2_scene_ray_capture.h"
#include "rg2_scene_ray_plain.h"
#include "rg2_visibility_capture.h"
#include "rg2_visibility_plain.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

// Ray path capture acceptance (docs/architecture/render-graph-authoring.md#ray-path-inspection-proposed):
// every node form's LUDUS_RAY_PATH_CAPTURE variant keeps the plain kernel's output and records known rays.
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
namespace
{
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr usize PIXEL_BYTES = static_cast<usize>(WIDTH) * HEIGHT * 4;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
/// The ray_path fixture traces a camera query and three visibility queries per hit pixel.
constexpr uint32 FIXTURE_EVENTS_PER_HIT = 4;
/// The ray_path fixture's red channel: (sample + 1) * SAMPLE_COLOR_SCALE.
constexpr float32 SAMPLE_COLOR_SCALE = 0.125F;
/// Orthographic identity camera: rays start on the z = 0 near plane and end on z = 1.
constexpr float32 TRIANGLE_DEPTH_WORLD = 0.5F;
constexpr float32 OCCLUDER_DEPTH_WORLD = 0.75F;
constexpr float32 DISTANCE_TOLERANCE_WORLD = 0.0001F;
constexpr uint32 CENTER_X = WIDTH / 2;
constexpr uint32 CENTER_Y = HEIGHT / 2;

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
/// One device, the plain and capture graphs, and a World of up to two copies of one triangle.
struct CaptureFixture final
{
    RhiGuard Session;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    renderer::RenderGraph Plain;
    renderer::RenderGraph Capture;
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{
        {{-.5F, -.5F, TRIANGLE_DEPTH_WORLD}, {.5F, -.5F, TRIANGLE_DEPTH_WORLD}, {0, .5F, TRIANGLE_DEPTH_WORLD}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<uint32, 1> MaterialBinding{{0}};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 2> Items{};
    usize ItemCount = 1;
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    StaticArray<renderer::SceneShaderMaterial, 1> Materials{};
    StaticArray<renderer::DirectLight, 1> Lights{};
    renderer::SceneShaderInputs Inputs{};
    StaticArray<uint8, PIXEL_BYTES> PlainPixels{};
    StaticArray<uint8, PIXEL_BYTES> CapturePixels{};
    CaptureFixture()
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
        description.Preferred.RayQueries = true;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            FAIL("Ray path capture acceptance requires a GPU adapter");
        }
        REQUIRE(started == rhi::DeviceStatus::Ready);
        rhi::RayCapabilities capabilities;
        if (rhi::GetRayCapabilities(Device, capabilities) == rhi::RasterStatus::Unsupported)
        {
            SKIP("Hardware raytracing is unavailable; capture reports that it is unavailable");
        }
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Items[0].SourceId = 1;
        Items[0].Material.Color = {1, 0, 0, 1};
        Items[1] = Items[0];
        Items[1].SourceId = 2;
        Items[1].MeshToWorld.Translation.Z = OCCLUDER_DEPTH_WORLD - TRIANGLE_DEPTH_WORLD;
        Cameras[0].OrthographicCamera = true;
        Materials[0].BaseColor = {1, 0, 0};
        Lights[0].SourceId = 1;
        Lights[0].Kind = renderer::LightKind::Point;
        Lights[0].Position = {0, 0, -1};
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        Inputs.Lights = {Lights.GetData(), Lights.GetSize()};
        Inputs.Environment = {0, 0, 1};
    }
    ~CaptureFixture()
    {
        Capture.Reset();
        Plain.Reset();
    }
};
void Finish(renderer::RenderGraph& inoutGraph, rhi::RasterStatus status)
{
    const uint64 beginning = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        status = inoutGraph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
}
void UpdateScene(CaptureFixture& inoutFixture, renderer::RenderGraph& inoutGraph, bool hasSceneInterface)
{
    // Triangle-profile graphs take flat item colors; only the scene interface binds materials.
    inoutFixture.Meshes[0].MaterialBindings =
        hasSceneInterface == true
            ? BorrowedView<const uint32>{inoutFixture.MaterialBinding.GetData(), inoutFixture.MaterialBinding.GetSize()}
            : BorrowedView<const uint32>{};
    renderer::RenderGraphReport report;
    Finish(inoutGraph,
           inoutGraph.Update({inoutFixture.Meshes.GetData(), inoutFixture.Meshes.GetSize()},
                             {inoutFixture.Items.GetData(), inoutFixture.ItemCount},
                             {inoutFixture.Cameras.GetData(), inoutFixture.Cameras.GetSize()},
                             1,
                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                             report,
                             hasSceneInterface == true ? &inoutFixture.Inputs : nullptr));
}
uint32 FindExport(const renderer::RenderGraphPlan& plan)
{
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        if (plan.Resources[resource].IsExported == 1)
        {
            return resource;
        }
    }
    FAIL("The graph exports no color");
    return 0;
}
void ReadFrame(CaptureFixture& inoutFixture,
               renderer::RenderGraph& inoutGraph,
               uint32 exportResource,
               StaticArray<uint8, PIXEL_BYTES>& outPixels)
{
    rhi::SubmissionToken completion;
    REQUIRE(inoutGraph.Render(inoutFixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    const uint64 beginning = time::NowTicks();
    rhi::RasterStatus result = rhi::GetStatus(inoutFixture.Device, completion);
    while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        result = rhi::GetStatus(inoutFixture.Device, completion);
    }
    REQUIRE(result == rhi::RasterStatus::Ready);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(inoutGraph.GetExport(exportResource, texture, aspect) == rhi::RasterStatus::Ready);
    rhi::ReadbackTicket ticket;
    result = rhi::RequestTextureReadback(inoutFixture.Device,
                                         texture,
                                         {0, 0, 0, WIDTH, HEIGHT},
                                         static_cast<usize>(WIDTH) * 4,
                                         ticket);
    while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        result = rhi::GetStatus(inoutFixture.Device, ticket);
    }
    REQUIRE(result == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CopyReadback(inoutFixture.Device, ticket, outPixels.GetData(), outPixels.GetSize()) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(inoutFixture.Device, ticket) == rhi::RasterStatus::Ready);
}
/// Decoded result of one capture run.
struct CaptureResult final
{
    renderer::RayPathCaptureReport Report{};
    Array<renderer::RayPathEvent> Events;
    Array<renderer::RayPathSummary> Summaries;
};
/// Render the plain and capture graphs side by side, require identical exported bytes every
/// frame, then read back and decode the capture without blocking the owner thread.
void RunCapture(CaptureFixture& inoutFixture,
                const renderer::RenderGraphPlan& plan,
                const renderer::RenderGraphPrograms& plain,
                const renderer::RenderGraphPrograms& capture,
                const renderer::RayPathCaptureDescription& description,
                uint32 frames,
                CaptureResult& outResult)
{
    REQUIRE(plain.HasRayPathCapture == false);
    REQUIRE(capture.HasRayPathCapture == true);
    Finish(inoutFixture.Plain, inoutFixture.Plain.Initialize(inoutFixture.Device, plan, plain));
    Finish(inoutFixture.Capture, inoutFixture.Capture.Initialize(inoutFixture.Device, plan, capture));
    CHECK(inoutFixture.Plain.RequestRayPathCapture(description) == rhi::RasterStatus::Unsupported);
    renderer::RenderGraphReport report;
    CHECK(inoutFixture.Capture.Update({inoutFixture.Meshes.GetData(), inoutFixture.Meshes.GetSize()},
                                      {inoutFixture.Items.GetData(), inoutFixture.ItemCount},
                                      {inoutFixture.Cameras.GetData(), inoutFixture.Cameras.GetSize()},
                                      1,
                                      renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                      report,
                                      &inoutFixture.Inputs) == rhi::RasterStatus::InvalidState);
    const rhi::RasterStatus requested = inoutFixture.Capture.RequestRayPathCapture(description);
    REQUIRE((requested == rhi::RasterStatus::Ready || requested == rhi::RasterStatus::Pending));
    CHECK(inoutFixture.Capture.RequestRayPathCapture(description) == rhi::RasterStatus::InvalidState);
    UpdateScene(inoutFixture, inoutFixture.Plain, plain.HasSceneShaderInterface);
    UpdateScene(inoutFixture, inoutFixture.Capture, capture.HasSceneShaderInterface);
    // One extraction per capture graph: a replacement would trace samples [0, N) again.
    CHECK(inoutFixture.Capture.Update({inoutFixture.Meshes.GetData(), inoutFixture.Meshes.GetSize()},
                                      {inoutFixture.Items.GetData(), inoutFixture.ItemCount},
                                      {inoutFixture.Cameras.GetData(), inoutFixture.Cameras.GetSize()},
                                      2,
                                      renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                      report,
                                      capture.HasSceneShaderInterface == true ? &inoutFixture.Inputs : nullptr) ==
          rhi::RasterStatus::InvalidState);
    const uint32 exportResource = FindExport(plan);
    for (uint32 frame = 0; frame < frames; ++frame)
    {
        ReadFrame(inoutFixture, inoutFixture.Plain, exportResource, inoutFixture.PlainPixels);
        ReadFrame(inoutFixture, inoutFixture.Capture, exportResource, inoutFixture.CapturePixels);
        usize differences = 0;
        for (usize byte = 0; byte < PIXEL_BYTES; ++byte)
        {
            differences += inoutFixture.PlainPixels[byte] != inoutFixture.CapturePixels[byte] ? 1 : 0;
        }
        CHECK(differences == 0);
    }
    const uint64 beginning = time::NowTicks();
    rhi::RasterStatus polled = inoutFixture.Capture.PollRayPathCapture(outResult.Report);
    CHECK(polled == rhi::RasterStatus::Pending);
    while (polled == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        static_cast<void>(rhi::PollLifetime(inoutFixture.Device));
        polled = inoutFixture.Capture.PollRayPathCapture(outResult.Report);
    }
    REQUIRE(polled == rhi::RasterStatus::Ready);
    const BorrowedView<const renderer::RayPathEvent> events = inoutFixture.Capture.GetRayPathEvents();
    const BorrowedView<const renderer::RayPathSummary> summaries = inoutFixture.Capture.GetRayPathSummaries();
    for (usize index = 0; index < events.GetSize(); ++index)
    {
        outResult.Events.Add(events[index]);
    }
    for (usize index = 0; index < summaries.GetSize(); ++index)
    {
        outResult.Summaries.Add(summaries[index]);
    }
}
/// World position of a pixel center on the orthographic identity camera's near plane.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a pixel column and row
math::Vector3 GetNearPositionWorld(uint32 x, uint32 y) noexcept
{
    const float32 u = (static_cast<float32>(x) + .5F) / static_cast<float32>(WIDTH);
    const float32 v = (static_cast<float32>(y) + .5F) / static_cast<float32>(HEIGHT);
    return {u * 2 - 1, 1 - v * 2, 0};
}
bool IsNear(float32 actual, float32 expected) noexcept
{
    return math::Abs(actual - expected) <= DISTANCE_TOLERANCE_WORLD;
}
/// The camera event of an orthographic pixel center: origin on z = 0, direction +Z, interval [0, 1].
void CheckCameraRay(const renderer::RayPathEvent& event, uint32 x, uint32 y)
{
    const math::Vector3 originWorld = GetNearPositionWorld(x, y);
    CHECK(IsNear(event.OriginWorld.X, originWorld.X) == true);
    CHECK(IsNear(event.OriginWorld.Y, originWorld.Y) == true);
    CHECK(IsNear(event.OriginWorld.Z, 0) == true);
    CHECK(IsNear(event.DirectionWorld.X, 0) == true);
    CHECK(IsNear(event.DirectionWorld.Y, 0) == true);
    CHECK(IsNear(event.DirectionWorld.Z, 1) == true);
    CHECK(IsNear(event.MinDistanceWorld, 0) == true);
    CHECK(IsNear(event.MaxDistanceWorld, 1) == true);
}
void CheckCompleteSummaries(const CaptureResult& result, uint32 expectedSamples)
{
    REQUIRE(result.Summaries.GetSize() == expectedSamples);
    for (uint32 sample = 0; sample < expectedSamples; ++sample)
    {
        const renderer::RayPathSummary& summary = result.Summaries[sample];
        CHECK(summary.Sample == sample);
        CHECK(summary.HasFailed == false);
        CHECK(summary.IsTruncated == false);
    }
    CHECK(result.Report.DroppedRecords == 0);
    CHECK(result.Report.TruncatedSummaries == 0);
}
} // namespace

TEST_CASE("Triangle-profile capture records a camera hit and a miss without changing the image",
          "[render-graph][gpu][ray-path]")
{
    CaptureFixture fixture;
    const renderer::RenderGraphPlan plan = packages::rg2_raytracing_plain::MakePlan();
    const renderer::RenderGraphPrograms plain =
        packages::rg2_raytracing_plain::MakePrograms(shaders::renderer_composite::Vertex(),
                                                     shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms capture =
        packages::rg2_raytracing_capture::MakePrograms(shaders::renderer_composite::Vertex(),
                                                       shaders::renderer_composite::Fragment());
    uint32 x = CENTER_X;
    uint32 y = CENTER_Y;
    bool shouldHit = true;
    SECTION("hit") {}
    SECTION("miss")
    {
        x = 0;
        y = 0;
        shouldHit = false;
    }
    constexpr uint32 SAMPLES = 2;
    CaptureResult result;
    RunCapture(fixture, plan, plain, capture, { .PixelX = x, .PixelY = y, .SampleCount = SAMPLES }, SAMPLES, result);
    CheckCompleteSummaries(result, SAMPLES);
    REQUIRE(result.Events.GetSize() == SAMPLES);
    for (uint32 sample = 0; sample < SAMPLES; ++sample)
    {
        const renderer::RayPathEvent& event = result.Events[sample];
        CHECK(event.PixelX == x);
        CHECK(event.PixelY == y);
        CHECK(event.Sample == sample);
        CHECK(event.Pass == 0);
        CHECK(event.Sequence == 0);
        CHECK(event.Kind == renderer::RayPathEventKind::Closest);
        CheckCameraRay(event, x, y);
        CHECK(event.RejectedCandidates == 0);
        if (shouldHit == true)
        {
            CHECK(event.Outcome == renderer::RayPathOutcome::Hit);
            CHECK(IsNear(event.HitDistanceWorld, TRIANGLE_DEPTH_WORLD) == true);
            CHECK(event.Triangle == 0);
            CHECK(event.Instance == 0);
            CHECK(event.Primitive == 0);
            CHECK(event.SourceId == 1);
        }
        else
        {
            CHECK(event.Outcome == renderer::RayPathOutcome::Miss);
            CHECK(event.Triangle == renderer::RAY_PATH_INVALID_INDEX);
            CHECK(event.SourceId == 0);
        }
    }
}

TEST_CASE("Scene compute capture records occluded and unoccluded visibility rays",
          "[render-graph][gpu][ray-path][compute-ray-tracing]")
{
    CaptureFixture fixture;
    fixture.ItemCount = 2;
    const renderer::RenderGraphPlan plan = packages::rg2_ray_path_plain::MakePlan();
    const renderer::RenderGraphPrograms plain =
        packages::rg2_ray_path_plain::MakePrograms(shaders::renderer_composite::Vertex(),
                                                   shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms capture =
        packages::rg2_ray_path_capture::MakePrograms(shaders::renderer_composite::Vertex(),
                                                     shaders::renderer_composite::Fragment());
    constexpr uint32 SAMPLES = 3;
    CaptureResult result;
    RunCapture(fixture,
               plan,
               plain,
               capture,
               { .PixelX = CENTER_X, .PixelY = CENTER_Y, .SampleCount = SAMPLES },
               SAMPLES + 1,
               result);
    CheckCompleteSummaries(result, SAMPLES);
    REQUIRE(result.Events.GetSize() == usize{SAMPLES} * FIXTURE_EVENTS_PER_HIT);
    for (uint32 sample = 0; sample < SAMPLES; ++sample)
    {
        const renderer::RayPathSummary& summary = result.Summaries[sample];
        CHECK(summary.EventCount == FIXTURE_EVENTS_PER_HIT);
        CHECK(summary.Radiance.X == static_cast<float32>(sample + 1) * SAMPLE_COLOR_SCALE);
        CHECK(summary.Radiance.Y == 1);
        CHECK(summary.Radiance.Z == 1);
        const renderer::RayPathEvent& camera = result.Events[usize{sample} * FIXTURE_EVENTS_PER_HIT];
        const renderer::RayPathEvent& occluded = result.Events[usize{sample} * FIXTURE_EVENTS_PER_HIT + 1];
        const renderer::RayPathEvent& unoccluded = result.Events[usize{sample} * FIXTURE_EVENTS_PER_HIT + 2];
        constexpr usize PREVIEW_VISIBILITY_EVENT = 3;
        const renderer::RayPathEvent& preview =
            result.Events[usize{sample} * FIXTURE_EVENTS_PER_HIT + PREVIEW_VISIBILITY_EVENT];
        CHECK(camera.Sample == sample);
        CHECK(camera.Sequence == 0);
        CHECK(camera.Kind == renderer::RayPathEventKind::Closest);
        CHECK(camera.Outcome == renderer::RayPathOutcome::Hit);
        CheckCameraRay(camera, CENTER_X, CENTER_Y);
        CHECK(IsNear(camera.HitDistanceWorld, TRIANGLE_DEPTH_WORLD) == true);
        CHECK(camera.SourceId == 1);
        CHECK(occluded.Sequence == 1);
        CHECK(occluded.Kind == renderer::RayPathEventKind::Visibility);
        CHECK(occluded.Outcome == renderer::RayPathOutcome::Hit);
        CHECK(occluded.SourceId == 2);
        CHECK(occluded.Instance == 1);
        CHECK(IsNear(occluded.DirectionWorld.Z, 1) == true);
        CHECK(IsNear(occluded.OriginWorld.Z, TRIANGLE_DEPTH_WORLD) == true);
        CHECK(IsNear(occluded.HitDistanceWorld, OCCLUDER_DEPTH_WORLD - TRIANGLE_DEPTH_WORLD) == true);
        CHECK(unoccluded.Sequence == 2);
        CHECK(unoccluded.Kind == renderer::RayPathEventKind::Visibility);
        CHECK(unoccluded.Outcome == renderer::RayPathOutcome::Miss);
        CHECK(unoccluded.SourceId == 0);
        CHECK(IsNear(unoccluded.DirectionWorld.Z, -1) == true);
        CHECK(IsNear(unoccluded.HitDistanceWorld, unoccluded.MaxDistanceWorld) == true);
        CHECK(preview.Sample == sample);
        CHECK(preview.Sequence == PREVIEW_VISIBILITY_EVENT);
        CHECK(preview.Kind == renderer::RayPathEventKind::Visibility);
        CHECK(preview.Outcome == renderer::RayPathOutcome::Hit);
        CHECK(preview.SourceId == 2);
        CHECK(preview.Instance == 1);
        CHECK(IsNear(preview.DirectionWorld.Z, 1) == true);
        CHECK(IsNear(preview.HitDistanceWorld, OCCLUDER_DEPTH_WORLD - preview.OriginWorld.Z) == true);
    }
}

TEST_CASE("Ray path capture keeps earlier records and marks truncated paths when full", "[render-graph][gpu][ray-path]")
{
    CaptureFixture fixture;
    fixture.ItemCount = 2;
    const renderer::RenderGraphPlan plan = packages::rg2_ray_path_plain::MakePlan();
    const renderer::RenderGraphPrograms plain =
        packages::rg2_ray_path_plain::MakePrograms(shaders::renderer_composite::Vertex(),
                                                   shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms capture =
        packages::rg2_ray_path_capture::MakePrograms(shaders::renderer_composite::Vertex(),
                                                     shaders::renderer_composite::Fragment());
    constexpr uint32 SAMPLES = 3;
    constexpr uint32 CAPACITY = 4;
    CaptureResult result;
    RunCapture(fixture,
               plan,
               plain,
               capture,
               { .PixelX = CENTER_X, .PixelY = CENTER_Y, .SampleCount = SAMPLES, .RecordCapacity = CAPACITY },
               SAMPLES,
               result);
    CHECK(result.Report.RecordCapacity == CAPACITY);
    CHECK(result.Report.StoredRecords == CAPACITY);
    CHECK(result.Report.DroppedRecords == usize{SAMPLES} * FIXTURE_EVENTS_PER_HIT - CAPACITY);
    // Summaries have their own region, so every invocation still reports its path length.
    REQUIRE(result.Summaries.GetSize() == SAMPLES);
    uint32 truncated = 0;
    for (const renderer::RayPathSummary& summary : result.Summaries)
    {
        CHECK(summary.EventCount == FIXTURE_EVENTS_PER_HIT);
        truncated += summary.IsTruncated == true ? 1 : 0;
    }
    CHECK(truncated == result.Report.TruncatedSummaries);
    CHECK(truncated >= 2);
    CHECK(result.Events.GetSize() == CAPACITY);
}

TEST_CASE("Visibility-buffer capture starts each path with a raster first hit",
          "[render-graph][gpu][ray-path][visibility]")
{
    CaptureFixture fixture;
    const renderer::RenderGraphPlan plan = packages::rg2_visibility_plain::MakePlan();
    const renderer::RenderGraphPrograms plain =
        packages::rg2_visibility_plain::MakePrograms(shaders::renderer_composite::Vertex(),
                                                     shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms capture =
        packages::rg2_visibility_capture::MakePrograms(shaders::renderer_composite::Vertex(),
                                                       shaders::renderer_composite::Fragment());
    REQUIRE(capture.HasVisibilityBuffer == true);
    constexpr uint32 SAMPLES = 2;
    CaptureResult result;
    RunCapture(fixture,
               plan,
               plain,
               capture,
               { .PixelX = CENTER_X, .PixelY = CENTER_Y, .SampleCount = SAMPLES },
               SAMPLES,
               result);
    CheckCompleteSummaries(result, SAMPLES);
    // visibility/generate.slang: a raster first hit, then one secondary ray back toward the camera.
    constexpr uint32 EVENTS_PER_SAMPLE = 2;
    REQUIRE(result.Events.GetSize() == usize{SAMPLES} * EVENTS_PER_SAMPLE);
    for (uint32 sample = 0; sample < SAMPLES; ++sample)
    {
        const renderer::RayPathEvent& first = result.Events[usize{sample} * EVENTS_PER_SAMPLE];
        const renderer::RayPathEvent& secondary = result.Events[usize{sample} * EVENTS_PER_SAMPLE + 1];
        CHECK(first.Pass == 1);
        CHECK(first.Kind == renderer::RayPathEventKind::RasterFirstHit);
        CHECK(first.Outcome == renderer::RayPathOutcome::Hit);
        CheckCameraRay(first, CENTER_X, CENTER_Y);
        CHECK(IsNear(first.HitDistanceWorld, TRIANGLE_DEPTH_WORLD) == true);
        CHECK(first.SourceId == 1);
        CHECK(secondary.Kind == renderer::RayPathEventKind::Closest);
        CHECK(secondary.Outcome == renderer::RayPathOutcome::Miss);
        CHECK(IsNear(secondary.DirectionWorld.Z, -1) == true);
    }
}

TEST_CASE("Scene-callback capture records contiguous paths for every sample", "[render-graph][gpu][ray-path]")
{
    CaptureFixture fixture;
    const renderer::RenderGraphPlan plan = packages::rg2_scene_ray_plain::MakePlan();
    const renderer::RenderGraphPrograms plain =
        packages::rg2_scene_ray_plain::MakePrograms(shaders::renderer_composite::Vertex(),
                                                    shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms capture =
        packages::rg2_scene_ray_capture::MakePrograms(shaders::renderer_composite::Vertex(),
                                                      shaders::renderer_composite::Fragment());
    REQUIRE(capture.HasSceneShaderInterface == true);
    constexpr uint32 SAMPLES = 2;
    CaptureResult result;
    RunCapture(fixture,
               plan,
               plain,
               capture,
               { .PixelX = CENTER_X, .PixelY = CENTER_Y, .SampleCount = SAMPLES },
               SAMPLES,
               result);
    CheckCompleteSummaries(result, SAMPLES);
    usize event = 0;
    for (const renderer::RayPathSummary& summary : result.Summaries)
    {
        REQUIRE(summary.EventCount != 0);
        REQUIRE(event + summary.EventCount <= result.Events.GetSize());
        const renderer::RayPathEvent& camera = result.Events[event];
        CHECK(camera.Sample == summary.Sample);
        CHECK(camera.Kind == renderer::RayPathEventKind::Closest);
        CHECK(camera.Outcome == renderer::RayPathOutcome::Hit);
        CheckCameraRay(camera, CENTER_X, CENTER_Y);
        CHECK(camera.SourceId == 1);
        for (uint32 sequence = 0; sequence < summary.EventCount; ++sequence)
        {
            CHECK(result.Events[event + sequence].Sequence == sequence);
        }
        event += summary.EventCount;
    }
    CHECK(event == result.Events.GetSize());
}

TEST_CASE("An ordered ray sequence reports each record under its own plan pass", "[render-graph][gpu][ray-path]")
{
    CaptureFixture fixture;
    const renderer::RenderGraphPlan plan = packages::rg2_ordered_ray_plain::MakePlan();
    const renderer::RenderGraphPrograms plain =
        packages::rg2_ordered_ray_plain::MakePrograms(shaders::renderer_composite::Vertex(),
                                                      shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms capture =
        packages::rg2_ordered_ray_capture::MakePrograms(shaders::renderer_composite::Vertex(),
                                                        shaders::renderer_composite::Fragment());
    REQUIRE(capture.HasPersistentRayBuffers == true);
    // ordered_ray: initial, temporal and shade RayTracingPipeline passes, in that plan order.
    constexpr uint32 RAY_PASSES = 3;
    REQUIRE(plan.Passes.GetSize() == RAY_PASSES);
    constexpr uint32 SAMPLES = 2;
    CaptureResult result;
    RunCapture(fixture,
               plan,
               plain,
               capture,
               { .PixelX = CENTER_X, .PixelY = CENTER_Y, .SampleCount = SAMPLES },
               SAMPLES,
               result);
    REQUIRE(result.Summaries.GetSize() == usize{SAMPLES} * RAY_PASSES);
    for (usize index = 0; index < result.Summaries.GetSize(); ++index)
    {
        const renderer::RayPathSummary& summary = result.Summaries[index];
        CHECK(summary.Sample == index / RAY_PASSES);
        CHECK(summary.Pass == index % RAY_PASSES);
        CHECK(summary.IsTruncated == false);
    }
    // The initial pass's TraceAndShade is its first event; every event names a plan ray pass.
    REQUIRE(result.Events.IsEmpty() == false);
    CHECK(result.Events[0].Pass == 0);
    CHECK(result.Events[0].Kind == renderer::RayPathEventKind::Closest);
    CHECK(result.Events[0].Outcome == renderer::RayPathOutcome::Hit);
    CHECK(result.Events[0].SourceId == 1);
    for (const renderer::RayPathEvent& event : result.Events)
    {
        CHECK(event.Pass < RAY_PASSES);
    }
}
