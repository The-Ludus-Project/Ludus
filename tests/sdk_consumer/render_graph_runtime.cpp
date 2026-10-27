// RG3 runtime generations through the installed SDK, natively and in a browser.
// The installed ludus_compile_render_graph helper cooks two offscreen graphs into
// compiled headers and `.lurgrt` runtime generations: the opaque static graph and
// the masked graph whose authored coverage reads a per-draw uniform value. For each,
// this consumer renders the header's graph, then decodes the generation and
// activates it twice through RenderGraphActivation. Every exported RGBA8 byte of
// each activated generation must equal the compiled header's bytes; reading back
// the export avoids comparing browser canvas presentation instead of the graph's
// own output.
#include "renderer_composite.h"
#include "rg3_masked.h"
#include "rg3_masked_package.h"
#include "rg3_runtime.h"
#include "rg3_runtime_package.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
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
namespace opaque = ludus::render_graph_packages::rg3_runtime;
namespace masked = ludus::render_graph_packages::rg3_masked;
namespace shaders = ludus::shaders;
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr usize BYTES_PER_PIXEL = 4;
constexpr usize PIXEL_BYTES = usize{WIDTH} * HEIGHT * BYTES_PER_PIXEL;
/// Both graphs export their color target as logical resource zero.
constexpr uint32 EXPORTED_COLOR = 0;
/// Activate the same generation twice: a first activation and a replacement at a frame boundary.
constexpr uint64 GENERATION_COUNT = 2;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr usize TABLE_COUNT = 1;
/// Byte offset of pixel (column, row) in the RGBA8 readback.
constexpr usize GetPixel(usize column, usize row) noexcept
{
    return (row * WIDTH + column) * BYTES_PER_PIXEL;
}
/// Center of the triangle; the comparison is over every byte, this only proves it is not empty.
constexpr usize CENTER_PIXEL = GetPixel(WIDTH / 2, HEIGHT / 2);
/// Masked graph probes at mid height: mesh x = +0.125 is covered, x = -0.125 is below the threshold.
constexpr usize KEPT_PIXEL = GetPixel(54, HEIGHT / 2);
constexpr usize DISCARDED_PIXEL = GetPixel(42, HEIGHT / 2);
/// One cooked graph: compiled header entry points, host interface and embedded runtime generation.
struct GraphFixture final
{
    renderer::RenderGraphPlan (*MakePlan)() noexcept = nullptr;
    renderer::RenderGraphPrograms (*MakePrograms)(const rhi::RasterShaderDescription&,
                                                  const rhi::RasterShaderDescription&) noexcept = nullptr;
    BorrowedView<const uint8> Identity;
    BorrowedView<const uint8> Package;
    bool IsMasked = false;
};
constexpr usize FIXTURE_COUNT = 2;
const StaticArray<GraphFixture, FIXTURE_COUNT> FIXTURES{
    {{opaque::MakePlan,
      opaque::MakePrograms,
      {opaque::INTERFACE_IDENTITY.GetData(), opaque::INTERFACE_IDENTITY.GetSize()},
      {RG3_RUNTIME_PACKAGE.GetData(), RG3_RUNTIME_PACKAGE.GetSize()},
      false},
     {masked::MakePlan,
      masked::MakePrograms,
      {masked::INTERFACE_IDENTITY.GetData(), masked::INTERFACE_IDENTITY.GetSize()},
      {RG3_MASKED_PACKAGE.GetData(), RG3_MASKED_PACKAGE.GetSize()},
      true}}};
/// Per-draw Mask values for the masked graph: cover mesh x >= 0.
StaticArray<float32, masked::UNIFORM_VALUE_COUNT> gMask{};
/// Steps of the qualification, in order.
enum class Step : uint8
{
    StartCompiled,
    PrepareCompiled,
    CaptureCompiled,
    StartActivation,
    Activate,
    CaptureActivated,
};
/// Sub-steps of rendering one frame and reading its export back.
enum class Capture : uint8
{
    Render,
    AwaitExport,
    AwaitReadback,
};
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
renderer::RenderGraph gCompiled;
renderer::RenderGraphActivation gActivation;
StaticArray<math::Vector3, VERTEX_COUNT> gPositions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
StaticArray<uint32, INDEX_COUNT> gIndices{{0, 1, 2}};
StaticArray<renderer::RenderGraphMesh, TABLE_COUNT> gMeshes{};
StaticArray<renderer::RenderGraphItem, TABLE_COUNT> gItems{};
StaticArray<renderer::ViewDescription, TABLE_COUNT> gCameras{};
StaticArray<uint8, PIXEL_BYTES> gExpected{};
StaticArray<uint8, PIXEL_BYTES> gPixels{};
rhi::ReadbackTicket gReadback;
Step gStep = Step::StartCompiled;
Capture gCapture = Capture::Render;
uint64 gRevision = 0;
usize gFixture = 0;
uint64 gGeneration = 0;
/// Generation whose first extraction was accepted; its preparation may complete over later frames.
uint64 gUpdatedGeneration = 0;
usize gMismatches = 0;
rhi::RasterStatus gLastStatus = rhi::RasterStatus::Ready;
bool gIsDone = false;
bool gHasFailed = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(int32, Selection, (), { const s = new URL(location.href).searchParams.get('backend'); return s === 'webgpu' ? 1 : s === 'webgl2' ? 2 : 0; });
EM_JS(void, Report, (int32 state, uint32 step, uint32 capture, uint32 generation, uint32 mismatches, uint32 center, int32 backend, uint32 status, uint32 fixture), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : 'loading';
    Object.assign(node.dataset, {step, capture, generation, mismatches, center, status, fixture, backend: backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native'});
    node.textContent = node.dataset.state + ' fixture=' + fixture + ' generation=' + generation + ' mismatches=' + mismatches;
});
// clang-format on
#else
int32 Selection() noexcept
{
    return 0;
}
void Report(int32, uint32, uint32, uint32, uint32, uint32, int32, uint32, uint32) noexcept {}
#endif
void Publish(int32 state) noexcept
{
    rhi::DeviceInfo information;
    static_cast<void>(rhi::GetDeviceInfo(gDevice, information));
    Report(state,
           static_cast<uint32>(gStep),
           static_cast<uint32>(gCapture),
           static_cast<uint32>(gGeneration),
           static_cast<uint32>(gMismatches),
           gExpected[FIXTURES[gFixture].IsMasked == true ? KEPT_PIXEL : CENTER_PIXEL],
           static_cast<int32>(information.Startup.SelectedBackend),
           static_cast<uint32>(gLastStatus),
           static_cast<uint32>(gFixture));
}
void Stop(bool hasPassed) noexcept
{
    gIsDone = true;
    gHasFailed = hasPassed == false;
    Publish(hasPassed == true ? 1 : 2);
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
    static_cast<void>(rhi::Release(gDevice, gReadback));
    gActivation.Reset();
    gCompiled.Reset();
    rhi::Shutdown();
}
/// Record the rejected status for the page report and stop.
void Fail(rhi::RasterStatus status) noexcept
{
    gLastStatus = status;
    Stop(false);
}
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
renderer::RenderGraphPrograms MakeCompiledPrograms() noexcept
{
    return FIXTURES[gFixture].MakePrograms(shaders::renderer_composite::Vertex(),
                                           shaders::renderer_composite::Fragment());
}
rhi::RasterStatus UpdateScene(bool isActivation) noexcept
{
    ++gRevision;
    // Runtime values select coverage; they never select a shader configuration.
    const bool isMasked = FIXTURES[gFixture].IsMasked;
    gItems[0].Coverage =
        isMasked == true ? renderer::RenderGraphMaterial::Masked : renderer::RenderGraphMaterial::Opaque;
    gItems[0].Uniforms = isMasked == true ? BorrowedView<const float32>{gMask.GetData(), gMask.GetSize()}
                                          : BorrowedView<const float32>{};
    renderer::RenderGraphReport report;
    if (isActivation == true)
    {
        return gActivation.Update({gMeshes.GetData(), gMeshes.GetSize()},
                                  {gItems.GetData(), gItems.GetSize()},
                                  {gCameras.GetData(), gCameras.GetSize()},
                                  gRevision,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report);
    }
    return gCompiled.Update({gMeshes.GetData(), gMeshes.GetSize()},
                            {gItems.GetData(), gItems.GetSize()},
                            {gCameras.GetData(), gCameras.GetSize()},
                            gRevision,
                            renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                            report);
}
/// Decode a fresh copy of the embedded generation and stage it as the next candidate.
bool StageGeneration() noexcept
{
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    if (package.Get() == nullptr ||
        package->Decode(FIXTURES[gFixture].Package) != renderer::RenderGraphPackageStatus::Ready)
    {
        Fail(rhi::RasterStatus::InvalidDescription);
        return false;
    }
    const rhi::RasterStatus staged = gActivation.Stage(Move(package));
    if (IsAccepted(staged) == false)
    {
        Fail(staged);
        return false;
    }
    ++gGeneration;
    return true;
}
/// Render one frame of graph and copy its exported color into gPixels; true once complete.
bool CaptureFrame(renderer::RenderGraph& inoutGraph) noexcept
{
    if (gCapture == Capture::Render)
    {
        // The browser surface starts with a zero frame target, which skips acquisition; size it every frame.
        if (rhi::SetFrameTarget(gDevice, gSurface, { .Width = WIDTH, .Height = HEIGHT }) != rhi::DeviceStatus::Ready)
        {
            Fail(rhi::RasterStatus::InvalidState);
            return false;
        }
        rhi::SubmissionToken completion;
        const rhi::RasterStatus rendered = inoutGraph.Render(gSurface, WIDTH, HEIGHT, completion);
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
        gCapture = Capture::AwaitExport;
    }
    if (gCapture == Capture::AwaitExport)
    {
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect = rhi::TextureAspect::Color;
        const rhi::RasterStatus exported = inoutGraph.GetExport(EXPORTED_COLOR, texture, aspect);
        if (exported == rhi::RasterStatus::NotReady || exported == rhi::RasterStatus::Pending)
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
        gCapture = Capture::AwaitReadback;
    }
    const rhi::RasterStatus copied = rhi::CopyReadback(gDevice, gReadback, gPixels.GetData(), gPixels.GetSize());
    if (copied == rhi::RasterStatus::Pending || copied == rhi::RasterStatus::NotReady)
    {
        return false;
    }
    if (copied != rhi::RasterStatus::Ready || rhi::Release(gDevice, gReadback) != rhi::RasterStatus::Ready)
    {
        Fail(copied);
        return false;
    }
    gCapture = Capture::Render;
    return true;
}
/// Advance the activation until the target generation is active and drawable.
bool AdvanceActivation() noexcept
{
    const rhi::RasterStatus polled = gActivation.Poll();
    const renderer::RenderGraphActivationReport report = gActivation.GetReport();
    if (report.Candidate == renderer::RenderGraphCandidateState::Failed ||
        report.Candidate == renderer::RenderGraphCandidateState::RestartRequired)
    {
        Fail(report.LastFailure);
        return false;
    }
    if (polled != rhi::RasterStatus::Pending && polled != rhi::RasterStatus::NotReady && IsAccepted(polled) == false)
    {
        Fail(polled);
        return false;
    }
    renderer::RenderGraph* activeOrNull = gActivation.GetActiveOrNull();
    const bool isTargetActive = report.ActiveGeneration == gGeneration && activeOrNull != nullptr;
    // Update a newly active generation once, then wait: on WebGPU its scene uploads complete over later
    // frames, and another Update would replace that pending preparation before it ever becomes drawable.
    const bool needsFirstScene =
        isTargetActive == true && activeOrNull->GetDrawableSceneRevision() == 0 && gUpdatedGeneration != gGeneration;
    if (report.Candidate == renderer::RenderGraphCandidateState::NeedsUpdate || needsFirstScene == true)
    {
        const rhi::RasterStatus updated = UpdateScene(true);
        if (updated != rhi::RasterStatus::NotReady && IsAccepted(updated) == false)
        {
            Fail(updated);
            return false;
        }
        if (IsAccepted(updated) == true && isTargetActive == true)
        {
            gUpdatedGeneration = gGeneration;
        }
    }
    return isTargetActive == true && activeOrNull->GetDrawableSceneRevision() != 0;
}
void Tick() noexcept
{
    if (gIsDone == true)
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
    // Drains readback/creation completion; Pending only means other requests remain queued.
    const rhi::RasterStatus lifetime = rhi::PollLifetime(gDevice);
    if (IsAccepted(lifetime) == false && lifetime != rhi::RasterStatus::NotReady)
    {
        Fail(lifetime);
        return;
    }
    // Publish progress first, so a page that never finishes still reports where it waits.
    Publish(0);
    if (gStep == Step::StartCompiled)
    {
        // RenderGraphPrograms is tens of KiB; a temporary in this frame would take over half
        // of the browser's default 64 KiB stack, so the new-expression builds it in place.
        renderer::RenderGraphPrograms* programsOrNull =
            new (std::nothrow) renderer::RenderGraphPrograms(MakeCompiledPrograms());
        const core::UniquePtr<renderer::RenderGraphPrograms> programs{programsOrNull};
        if (programs.Get() == nullptr)
        {
            Fail(rhi::RasterStatus::OutOfMemory);
            return;
        }
        const rhi::RasterStatus initialized = gCompiled.Initialize(gDevice, FIXTURES[gFixture].MakePlan(), *programs);
        if (IsAccepted(initialized) == false)
        {
            Fail(initialized);
            return;
        }
        gStep = Step::PrepareCompiled;
    }
    if (gStep == Step::PrepareCompiled)
    {
        const rhi::RasterStatus polled = gCompiled.Poll();
        if (polled == rhi::RasterStatus::Pending || polled == rhi::RasterStatus::NotReady)
        {
            return;
        }
        if (polled != rhi::RasterStatus::Ready)
        {
            Fail(polled);
            return;
        }
        if (gCompiled.GetDrawableSceneRevision() == 0)
        {
            const rhi::RasterStatus updated = UpdateScene(false);
            if (updated != rhi::RasterStatus::NotReady && IsAccepted(updated) == false)
            {
                Fail(updated);
            }
            return;
        }
        gStep = Step::CaptureCompiled;
    }
    if (gStep == Step::CaptureCompiled)
    {
        if (CaptureFrame(gCompiled) == false)
        {
            return;
        }
        gExpected = gPixels;
        gCompiled.Reset();
        gStep = Step::StartActivation;
    }
    if (gStep == Step::StartActivation)
    {
        const rhi::RasterStatus initialized = gActivation.Initialize(gDevice,
                                                                     FIXTURES[gFixture].Identity,
                                                                     shaders::renderer_composite::Vertex(),
                                                                     shaders::renderer_composite::Fragment());
        if (initialized != rhi::RasterStatus::Ready || StageGeneration() == false)
        {
            Fail(initialized);
            return;
        }
        gStep = Step::Activate;
    }
    if (gStep == Step::Activate)
    {
        if (AdvanceActivation() == false)
        {
            return;
        }
        gStep = Step::CaptureActivated;
    }
    if (gStep == Step::CaptureActivated)
    {
        if (CaptureFrame(*gActivation.GetActiveOrNull()) == false)
        {
            return;
        }
        for (usize i = 0; i < PIXEL_BYTES; ++i)
        {
            gMismatches += gPixels[i] != gExpected[i] ? 1 : 0;
        }
        // The masked graph keeps the covered half and leaves the discarded half at the cleared background.
        const bool isDrawn = FIXTURES[gFixture].IsMasked == true
                                 ? gExpected[KEPT_PIXEL] != 0 && gExpected[DISCARDED_PIXEL] == gExpected[0]
                                 : gExpected[CENTER_PIXEL] != 0;
        if (gMismatches != 0 || isDrawn == false)
        {
            Stop(false);
            return;
        }
        if (gGeneration == GENERATION_COUNT && gFixture + 1 == FIXTURE_COUNT)
        {
            Stop(true);
            return;
        }
        if (gGeneration == GENERATION_COUNT)
        {
            // Qualify the next graph from its compiled header, with a fresh activation.
            gActivation.Reset();
            ++gFixture;
            gGeneration = 0;
            gUpdatedGeneration = 0;
            gStep = Step::StartCompiled;
            return;
        }
        if (StageGeneration() == false)
        {
            return;
        }
        gStep = Step::Activate;
    }
}
} // namespace
int main()
{
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
    const int32 selection = Selection();
    description.Selection = selection == 1   ? rhi::BackendSelection::WebGPU
                            : selection == 2 ? rhi::BackendSelection::WebGL2
                                             : rhi::BackendSelection::Auto;
    rhi::StartupInfo failure;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, window, description, gDevice, gSurface, &failure);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        Fail(rhi::RasterStatus::Unsupported);
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    // Native hosts have no browser event loop; bound the polling loop instead.
    constexpr uint64 NATIVE_TIMEOUT_SECONDS = 30;
    const uint64 start = time::NowTicks();
    while (gIsDone == false && time::NowTicks() - start < NATIVE_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (gIsDone == false)
    {
        Fail(rhi::RasterStatus::Pending);
    }
#endif
    return gHasFailed == true ? 1 : 0;
}
