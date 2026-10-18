#include "l0_scene.h"
#include "renderer_flat.h"
#include "resource_mesh.h"
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/platform/base/window.h>
#endif
namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rr::Renderer gRenderer;
rr::Mesh gQuad, gTriangle;
rr::Snapshot gSnapshot;
rr::PreparedView gView;
rr::ViewReport gReport;
rhi::SubmissionToken gCompletion;
uint32 gStep = 0, gFrames = 0;
bool gWaiting = false, gDone = false, gFailed = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(void, Progress, (uint32 step, uint32 frames), { const n = document.querySelector("#status"); n.dataset.step = step; n.dataset.frames = frames; });
EM_JS(int32, Selection, (), { const s = new URL(location.href).searchParams.get('backend'); return s === 'webgpu' ? 1 : s === 'webgl2' ? 2 : 0; });
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(int32, Reference, (), { return new URL(location.href).searchParams.has('reference') ? 1 : 0; });
EM_JS(void, Report, (int32 state, uint32 frames, uint32 draws, uint32 culled, int32 backend), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : state === 3 ? 'rendering' : 'loading';
    if (state === 3) globalThis.__qaPause = true;
    node.dataset.frames = frames; node.dataset.draws = draws; node.dataset.culled = culled;
    node.dataset.backend = backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native';
    node.textContent = node.dataset.state + ' frames=' + frames;
});
// clang-format on
#else
void Progress(uint32, uint32) noexcept {}
int32 Selection() noexcept
{
    return 0;
}
int32 Paused() noexcept
{
    return 0;
}
int32 Reference() noexcept
{
    return 0;
}
void Report(int32, uint32, uint32, uint32, int32) noexcept {}
#endif
bool Accept(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
void Stop(bool failed) noexcept
{
    gFailed = failed;
    gDone = true;
    rhi::DeviceInfo info;
    static_cast<void>(rhi::GetDeviceInfo(gDevice, info));
    Report(failed ? 2 : 1, gFrames, gReport.Draws, gReport.Culled, static_cast<int32>(info.Startup.SelectedBackend));
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
    gRenderer.Reset();
    rhi::Shutdown();
}
bool Wait(rhi::RasterStatus status) noexcept
{
    if (status == rhi::RasterStatus::Pending || status == rhi::RasterStatus::NotReady)
    {
        return true;
    }
    if (status != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return true;
    }
    return false;
}
void Tick() noexcept
{
    if (gDone || Paused() != 0)
    {
        return;
    }
    Progress(gStep, gFrames);
    rhi::DeviceInfo info;
    const auto device = rhi::GetDeviceInfo(gDevice, info);
    if (device == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (device != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    if (gStep == 0)
    {
        if (Accept(gRenderer.Initialize(gDevice,
                                        ludus::shaders::renderer_flat::Vertex(),
                                        ludus::shaders::renderer_flat::Fragment())) == false)
        {
            Stop(true);
            return;
        }
        gStep = 1;
    }
    if (gStep == 1)
    {
        if (Wait(gRenderer.Poll()))
        {
            return;
        }
        if (ludus::qa::CreateResourceMeshes(gRenderer, gQuad, gTriangle) == false)
        {
            Stop(true);
            return;
        }
        gStep = 2;
    }
    if (gStep == 2)
    {
        if (Wait(gRenderer.GetStatus(gQuad)) || Wait(gRenderer.GetStatus(gTriangle)))
        {
            return;
        }
        if (Accept(ludus::qa::BuildL0Scene(gRenderer, gQuad, gTriangle, Reference() != 0, gSnapshot, gView, gReport)) ==
            false)
        {
            Stop(true);
            return;
        }
        // Static view retains copied values/mesh versions after all source owners detach.
        if (gRenderer.Release(gSnapshot) != rhi::RasterStatus::Ready ||
            gRenderer.Release(gQuad) != rhi::RasterStatus::Ready ||
            gRenderer.Release(gTriangle) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gStep = 3;
    }
    if (Wait(gRenderer.GetStatus(gView)))
    {
        return;
    }
    if (Wait(rhi::PollLifetime(gDevice)))
    {
        return;
    }
    if (gWaiting)
    {
        if (Wait(rhi::GetStatus(gDevice, gCompletion)))
        {
            return;
        }
        gCompletion = {};
        gWaiting = false;
        if (gFrames == 120)
        {
            Stop(false);
            return;
        }
    }
    if (rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    const auto status = gRenderer.Submit(gView, gSurface, gCompletion);
    if (status == rhi::RasterStatus::NotReady || status == rhi::RasterStatus::CapacityExceeded)
    {
        return;
    }
    if (status != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return;
    }
    ++gFrames;
    gWaiting = true;
    // Pause at a live presented frame for the browser image oracle, then resume to 120.
    if (gFrames == 5)
    {
        Report(3, gFrames, gReport.Draws, gReport.Culled, static_cast<int32>(info.Startup.SelectedBackend));
    }
}
} // namespace
int main()
{
    rhi::WindowInfo window;
    window.Width = 96;
    window.Height = 64;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo settings;
    settings.Width = 96;
    settings.Height = 64;
    settings.CanvasSelector = "#canvas";
    settings.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(settings, gWindow) == false)
        return 1;
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Selection = Selection() == 1   ? rhi::BackendSelection::WebGPU
                            : Selection() == 2 ? rhi::BackendSelection::WebGL2
                                               : rhi::BackendSelection::Auto;
    rhi::StartupInfo failure;
    const auto startup = rhi::CreateDevice({}, window, description, gDevice, gSurface, &failure);
    if (startup != rhi::DeviceStatus::Ready && startup != rhi::DeviceStatus::Pending)
    {
        Stop(true);
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const auto started = time::NowTicks();
    while (gDone == false && time::NowTicks() - started < 15 * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (gDone == false)
    {
        Stop(true);
    }
#endif
    return gFailed ? 1 : 0;
}
