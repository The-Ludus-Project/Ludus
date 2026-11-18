// Managed bilinear upscale pipelines through the installed SDK, natively (Metal, Vulkan) and in a
// browser (WebGPU). The cooked offscreen graph's scene pass is rewritten to draw at half extent and
// a BilinearUpscale pass resamples it into the exported RGBA8 target, as in the Renderer GPU test.
// Three graphs draw the same extraction in turn:
//   unshared:  initialized without a program cache, so the pass prepares its own dependencies;
//   active:    initialized with a shared RenderGraphProgramCache;
//   candidate: initialized with the same cache while the active graph is still alive.
// The candidate must join the active graph's native upscale pipeline, and every exported byte of
// both shared graphs must equal the unshared graph's bytes. One `bilinear` line reports pipeline
// requests, shared requests, preparation and Render CPU time; see
// docs/architecture/renderer-systems.md#first-migration-and-acceptance.
#include "renderer_composite.h"
#include "rg3_bilinear.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <new>
#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/platform/base/window.h>
#endif
namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace offscreen = ludus::render_graph_packages::rg3_bilinear;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
constexpr logging::LogCategory LOG_BILINEAR{"Bilinear"};
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr usize BYTES_PER_PIXEL = 4;
constexpr usize PIXEL_BYTES = usize{WIDTH} * HEIGHT * BYTES_PER_PIXEL;
/// Scene color, its private depth, and the exported upscale target.
constexpr usize RESOURCE_COUNT = 3;
constexpr usize PASS_COUNT = 2;
constexpr usize CAMERA_COUNT = 1;
constexpr uint32 SCENE_COLOR = 0;
constexpr uint32 SCENE_DEPTH = 1;
constexpr uint32 UPSCALED_COLOR = 2;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr usize TABLE_COUNT = 1;
/// Byte offset of the center pixel; the comparison covers every byte, this proves the image is not empty.
constexpr usize CENTER_PIXEL = (usize{HEIGHT / 2} * WIDTH + WIDTH / 2) * BYTES_PER_PIXEL;
constexpr uint64 NANOSECONDS_PER_MICROSECOND = 1000;
constexpr float64 MICROSECONDS_PER_SECOND = 1e6;
enum class Case : uint8
{
    Unshared,
    Active,
    Candidate,
};
constexpr usize CASE_COUNT = 3;
const StaticArray<const char*, CASE_COUNT> CASE_NAMES{{"unshared", "active", "candidate"}};
/// Sub-steps of preparing one graph and reading its export back.
enum class Step : uint8
{
    Initialize,
    Prepare,
    Render,
    AwaitExport,
    AwaitReadback,
};
/// Measured outcome of one graph.
struct CaseMeasurement final
{
    renderer::RenderGraphManagedPipelines Upscale{};
    uint64 PrepareNanoseconds = 0;
    uint64 RenderNanoseconds = 0;
    usize Mismatches = 0;
};
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
renderer::RenderGraphProgramCache gCache;
StaticArray<renderer::RenderGraph, CASE_COUNT> gGraphs{};
StaticArray<format::Resource, RESOURCE_COUNT> gResources{};
StaticArray<format::Pass, PASS_COUNT> gPasses{};
StaticArray<format::Camera, CAMERA_COUNT> gTargets{};
StaticArray<math::Vector3, VERTEX_COUNT> gPositions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
StaticArray<uint32, INDEX_COUNT> gIndices{{0, 1, 2}};
StaticArray<renderer::RenderGraphMesh, TABLE_COUNT> gMeshes{};
StaticArray<renderer::RenderGraphItem, TABLE_COUNT> gItems{};
StaticArray<renderer::ViewDescription, TABLE_COUNT> gCameras{};
StaticArray<StaticArray<uint8, PIXEL_BYTES>, CASE_COUNT> gPixels{};
StaticArray<CaseMeasurement, CASE_COUNT> gMeasurements{};
rhi::ReadbackTicket gReadback;
usize gCase = 0;
Step gStep = Step::Initialize;
uint64 gRevision = 0;
uint64 gPrepareStart = 0;
rhi::RasterStatus gLastStatus = rhi::RasterStatus::Ready;
/// Lifecycle flags of the whole qualification.
struct ProgressFlags final
{
    bool IsDone = false;
    bool HasFailed = false;
    bool IsCacheBound = false;
};
ProgressFlags gFlags{};
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(void, Report, (int32 state, uint32 graph, uint32 step, uint32 mismatches, uint32 shared, uint32 center, int32 backend, uint32 status), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : 'loading';
    Object.assign(node.dataset, {graph, step, mismatches, shared, center, status, backend: backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native'});
    node.textContent = node.dataset.state + ' graph=' + graph + ' mismatches=' + mismatches + ' shared=' + shared;
});
// clang-format on
#else
void Report(int32, uint32, uint32, uint32, uint32, uint32, int32, uint32) noexcept {}
#endif
[[nodiscard]] bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
[[nodiscard]] bool IsWaiting(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Pending || status == rhi::RasterStatus::NotReady;
}
[[nodiscard]] usize CountMismatches() noexcept
{
    usize mismatches = 0;
    for (usize shared = static_cast<usize>(Case::Active); shared < CASE_COUNT; ++shared)
    {
        for (usize i = 0; i < PIXEL_BYTES; ++i)
        {
            mismatches += gPixels[shared][i] != gPixels[static_cast<usize>(Case::Unshared)][i] ? 1 : 0;
        }
    }
    return mismatches;
}
void Publish(int32 state) noexcept
{
    rhi::DeviceInfo information;
    static_cast<void>(rhi::GetDeviceInfo(gDevice, information));
    Report(state,
           static_cast<uint32>(gCase),
           static_cast<uint32>(gStep),
           static_cast<uint32>(gMeasurements[static_cast<usize>(Case::Candidate)].Mismatches),
           gMeasurements[static_cast<usize>(Case::Candidate)].Upscale.SharedRequests,
           gPixels[static_cast<usize>(Case::Unshared)][CENTER_PIXEL],
           static_cast<int32>(information.Startup.SelectedBackend),
           static_cast<uint32>(gLastStatus));
}
void Stop(bool hasPassed) noexcept
{
    gFlags.IsDone = true;
    gFlags.HasFailed = hasPassed == false;
    rhi::DeviceInfo information;
    static_cast<void>(rhi::GetDeviceInfo(gDevice, information));
    for (usize graph = 0; graph < CASE_COUNT; ++graph)
    {
        const CaseMeasurement& measurement = gMeasurements[graph];
        LUDUS_LOG_INFO(LOG_BILINEAR,
                       "bilinear graph={} backend={} requests={} shared_requests={} ready_requests={} "
                       "prewarm_microseconds={} queue_microseconds={} prepare_microseconds={} "
                       "render_microseconds={}",
                       CASE_NAMES[graph],
                       static_cast<uint32>(information.Startup.SelectedBackend),
                       measurement.Upscale.Requests,
                       measurement.Upscale.SharedRequests,
                       measurement.Upscale.ReadyRequests,
                       static_cast<uint64>(measurement.Upscale.PrewarmSeconds * MICROSECONDS_PER_SECOND),
                       static_cast<uint64>(measurement.Upscale.QueueSeconds * MICROSECONDS_PER_SECOND),
                       measurement.PrepareNanoseconds / NANOSECONDS_PER_MICROSECOND,
                       measurement.RenderNanoseconds / NANOSECONDS_PER_MICROSECOND);
    }
    const renderer::RenderGraphProgramCacheReport cache = gCache.GetReport();
    LUDUS_LOG_INFO(LOG_BILINEAR,
                   "bilinear {} mismatched_bytes={} created_entries={} shared_leases={} last_status={}",
                   hasPassed == true ? "passed" : "failed",
                   gMeasurements[static_cast<usize>(Case::Candidate)].Mismatches,
                   cache.CreatedEntries,
                   cache.SharedLeases,
                   static_cast<uint32>(gLastStatus));
    Publish(hasPassed == true ? 1 : 2);
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
    static_cast<void>(rhi::Release(gDevice, gReadback));
    for (renderer::RenderGraph& graph : gGraphs)
    {
        graph.Reset();
    }
    gCache.Reset();
    rhi::Shutdown();
}
void Fail(rhi::RasterStatus status) noexcept
{
    gLastStatus = status;
    Stop(false);
}
/// Rewrite the cooked offscreen plan: the scene pass draws at half extent and a bilinear pass
/// resamples it into the exported RGBA8 target, which the camera views.
[[nodiscard]] renderer::RenderGraphPlan MakeBilinearPlan() noexcept
{
    renderer::RenderGraphPlan plan = offscreen::MakePlan();
    gResources = {
        {offscreen::RESOURCES[SCENE_COLOR], offscreen::RESOURCES[SCENE_DEPTH], offscreen::RESOURCES[SCENE_COLOR]}};
    for (usize i = 0; i < UPSCALED_COLOR; ++i)
    {
        gResources[i].Width = WIDTH / 2;
        gResources[i].Height = HEIGHT / 2;
        gResources[i].IsExported = 0;
    }
    gResources[UPSCALED_COLOR].Backing = UPSCALED_COLOR;
    gResources[UPSCALED_COLOR].IsExported = 1;
    gPasses = {{offscreen::PASSES[0],
                {format::Technique::BilinearUpscale,
                 0,
                 UPSCALED_COLOR,
                 format::ABSENT,
                 format::Load::Clear,
                 format::Store::Store,
                 format::Load::Discard,
                 format::Store::Discard,
                 0}}};
    gPasses[1].SceneData = 0;
    gPasses[1].SourceColor = SCENE_COLOR;
    gTargets = {{{UPSCALED_COLOR}}};
    plan.Description.ResourceCount = RESOURCE_COUNT;
    plan.Description.PassCount = PASS_COUNT;
    plan.Resources = {gResources.GetData(), gResources.GetSize()};
    plan.Passes = {gPasses.GetData(), gPasses.GetSize()};
    plan.Cameras = {gTargets.GetData(), gTargets.GetSize()};
    plan.PassNames = {};
    return plan;
}
[[nodiscard]] rhi::RasterStatus InitializeGraph(renderer::RenderGraph& inoutGraph) noexcept
{
    // RenderGraphPrograms is tens of KiB; a temporary in this frame would take over half of the
    // browser's default 64 KiB stack, so the new-expression builds it in place.
    renderer::RenderGraphPrograms* programsOrNull = new (std::nothrow) renderer::RenderGraphPrograms(
        offscreen::MakePrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()));
    const core::UniquePtr<renderer::RenderGraphPrograms> programs{programsOrNull};
    if (programs.Get() == nullptr)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    renderer::RenderGraphProgramCache* cacheOrNull = gCase == static_cast<usize>(Case::Unshared) ? nullptr : &gCache;
    return inoutGraph.Initialize(gDevice, MakeBilinearPlan(), *programs, nullptr, 0, 0, nullptr, cacheOrNull);
}
[[nodiscard]] rhi::RasterStatus UpdateScene(renderer::RenderGraph& inoutGraph) noexcept
{
    ++gRevision;
    renderer::RenderGraphReport report;
    return inoutGraph.Update({gMeshes.GetData(), gMeshes.GetSize()},
                             {gItems.GetData(), gItems.GetSize()},
                             {gCameras.GetData(), gCameras.GetSize()},
                             gRevision,
                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                             report);
}
/// Advance the current graph by one step; true once its export has been read back.
[[nodiscard]] bool AdvanceGraph() noexcept
{
    renderer::RenderGraph& graph = gGraphs[gCase];
    CaseMeasurement& measurement = gMeasurements[gCase];
    if (gStep == Step::Initialize)
    {
        gPrepareStart = time::NowTicks();
        const rhi::RasterStatus initialized = InitializeGraph(graph);
        if (IsAccepted(initialized) == false)
        {
            Fail(initialized);
            return false;
        }
        gStep = Step::Prepare;
    }
    if (gStep == Step::Prepare)
    {
        const rhi::RasterStatus polled = graph.Poll();
        if (IsWaiting(polled) == true)
        {
            return false;
        }
        if (polled != rhi::RasterStatus::Ready)
        {
            Fail(polled);
            return false;
        }
        if (graph.GetDrawableSceneRevision() == 0)
        {
            const rhi::RasterStatus updated = UpdateScene(graph);
            if (updated != rhi::RasterStatus::NotReady && IsAccepted(updated) == false)
            {
                Fail(updated);
            }
            return false;
        }
        measurement.PrepareNanoseconds = time::NowTicks() - gPrepareStart;
        renderer::RenderGraphPipelineReport report;
        const rhi::RasterStatus reported = graph.GetPipelineReport(report);
        if (reported != rhi::RasterStatus::Ready)
        {
            Fail(reported);
            return false;
        }
        measurement.Upscale = report.Upscale;
        gStep = Step::Render;
    }
    if (gStep == Step::Render)
    {
        // The browser surface starts with a zero frame target, which skips acquisition; size it every frame.
        if (rhi::SetFrameTarget(gDevice, gSurface, { .Width = WIDTH, .Height = HEIGHT }) != rhi::DeviceStatus::Ready)
        {
            Fail(rhi::RasterStatus::InvalidState);
            return false;
        }
        rhi::SubmissionToken completion;
        const uint64 start = time::NowTicks();
        const rhi::RasterStatus rendered = graph.Render(gSurface, WIDTH, HEIGHT, completion);
        measurement.RenderNanoseconds = time::NowTicks() - start;
        if (rendered == rhi::RasterStatus::NotReady)
        {
            gLastStatus = rendered;
            return false;
        }
        if (IsAccepted(rendered) == false)
        {
            Fail(rendered);
            return false;
        }
        gStep = Step::AwaitExport;
    }
    if (gStep == Step::AwaitExport)
    {
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect = rhi::TextureAspect::Color;
        const rhi::RasterStatus exported = graph.GetExport(UPSCALED_COLOR, texture, aspect);
        if (IsWaiting(exported) == true)
        {
            return false;
        }
        if (exported != rhi::RasterStatus::Ready || aspect != rhi::TextureAspect::Color)
        {
            Fail(exported);
            return false;
        }
        const rhi::RasterStatus requested = rhi::RequestTextureReadback(gDevice,
                                                                        texture,
                                                                        { .Width = WIDTH, .Height = HEIGHT },
                                                                        usize{WIDTH} * BYTES_PER_PIXEL,
                                                                        gReadback);
        if (IsAccepted(requested) == false)
        {
            Fail(requested);
            return false;
        }
        gStep = Step::AwaitReadback;
    }
    const rhi::RasterStatus copied =
        rhi::CopyReadback(gDevice, gReadback, gPixels[gCase].GetData(), gPixels[gCase].GetSize());
    if (IsWaiting(copied) == true)
    {
        return false;
    }
    if (copied != rhi::RasterStatus::Ready || rhi::Release(gDevice, gReadback) != rhi::RasterStatus::Ready)
    {
        Fail(copied);
        return false;
    }
    gStep = Step::Initialize;
    return true;
}
/// The unshared graph owns its pipeline; the active graph creates the shared one and the candidate joins it.
[[nodiscard]] bool HasExpectedSharing() noexcept
{
    const renderer::RenderGraphManagedPipelines& unshared = gMeasurements[static_cast<usize>(Case::Unshared)].Upscale;
    const renderer::RenderGraphManagedPipelines& active = gMeasurements[static_cast<usize>(Case::Active)].Upscale;
    const renderer::RenderGraphManagedPipelines& candidate = gMeasurements[static_cast<usize>(Case::Candidate)].Upscale;
    const renderer::RenderGraphProgramCacheReport cache = gCache.GetReport();
    return unshared.Requests == 1 && unshared.SharedRequests == 0 && unshared.ReadyRequests == 1 &&
           active.Requests == 1 && active.SharedRequests == 0 && active.ReadyRequests == 1 && candidate.Requests == 1 &&
           candidate.SharedRequests == 1 && candidate.ReadyRequests == 1 && cache.CreatedEntries == 1 &&
           cache.SharedLeases == 1 && cache.Leases == 2;
}
void Tick() noexcept
{
    if (gFlags.IsDone == true)
    {
        return;
    }
    rhi::DeviceInfo information;
    const rhi::DeviceStatus device = rhi::GetDeviceInfo(gDevice, information);
    if (device == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (device != rhi::DeviceStatus::Ready)
    {
        Fail(rhi::RasterStatus::DeviceLost);
        return;
    }
    if (gFlags.IsCacheBound == false)
    {
        const rhi::RasterStatus bound = gCache.Initialize(gDevice);
        if (bound != rhi::RasterStatus::Ready)
        {
            Fail(bound);
            return;
        }
        gFlags.IsCacheBound = true;
    }
    // Drains readback/creation completion; Pending only means other requests remain queued.
    const rhi::RasterStatus lifetime = rhi::PollLifetime(gDevice);
    if (IsAccepted(lifetime) == false && lifetime != rhi::RasterStatus::NotReady)
    {
        Fail(lifetime);
        return;
    }
    // Publish progress first, so a page that never finishes still reports where it waits.
    Publish(0);
    if (AdvanceGraph() == false)
    {
        return;
    }
    if (gCase == static_cast<usize>(Case::Unshared))
    {
        // Retire the unshared graph so its pipeline cannot be mistaken for the shared one.
        gGraphs[gCase].Reset();
    }
    if (gCase + 1 < CASE_COUNT)
    {
        ++gCase;
        return;
    }
    // The active graph stays alive while the candidate prepares, as in a RenderGraphActivation replacement.
    gMeasurements[static_cast<usize>(Case::Candidate)].Mismatches = CountMismatches();
    const bool isDrawn = gPixels[static_cast<usize>(Case::Unshared)][CENTER_PIXEL] != 0;
    Stop(gMeasurements[static_cast<usize>(Case::Candidate)].Mismatches == 0 && isDrawn == true &&
         HasExpectedSharing() == true);
}
} // namespace
int main()
{
    static_cast<void>(logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} }));
    gMeshes[0].Geometry = {gPositions.GetData(), gPositions.GetSize(), gIndices.GetData(), gIndices.GetSize()};
    gItems[0].SourceId = 1;
    gItems[0].Material.Color = {.25F, .5F, .75F, 1};
    rhi::WindowInfo window;
    window.Width = WIDTH;
    window.Height = HEIGHT;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo settings;
    settings.Width = WIDTH;
    settings.Height = HEIGHT;
    settings.CanvasSelector = "#canvas";
    settings.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(settings, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
#if defined(LUDUS_PLATFORM_WEB)
    description.Selection = rhi::BackendSelection::WebGPU;
#endif
    rhi::StartupInfo failure;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, window, description, gDevice, gSurface, &failure);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    // Native hosts have no browser event loop; bound the polling loop instead.
    constexpr uint64 NATIVE_TIMEOUT_SECONDS = 60;
    const uint64 start = time::NowTicks();
    while (gFlags.IsDone == false && time::NowTicks() - start < NATIVE_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (gFlags.IsDone == false)
    {
        Fail(rhi::RasterStatus::Pending);
    }
#endif
    static_cast<void>(logging::LogSystem::Shutdown());
    return gFlags.HasFailed == true ? 1 : 0;
}
