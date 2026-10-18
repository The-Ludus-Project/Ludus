#include "l5_scene.h"
#include "renderer_clustered.h"
#include "renderer_clustered_shadow.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_output.h"
#include "renderer_shadow.h"
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
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
ludus::qa::L5Scene gScene;
uint32 gStep = 0, gFrames = 0;
bool gDone = false, gFailed = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(int32, Selection, (), { const s = new URL(location.href).searchParams.get('backend'); return s === 'webgpu' ? 1 : s === 'webgl2' ? 2 : 0; });
EM_JS(int32, Reference, (), { return new URL(location.href).searchParams.has('reference') ? 1 : 0; });
EM_JS(void, Progress, (uint32 step, uint32 frames, uint32 scene, uint32 operation), { Object.assign(document.querySelector('#status').dataset,{step,current:frames,scene,operation}); });
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(void, Report, (int32 state, uint32 frames, int32 backend), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : state === 3 ? 'rendering' : 'loading';
    if (state === 3) globalThis.__qaPause = true;
    Object.assign(node.dataset, {frames, hdr: globalThis.__qaHdr || 0, backend: backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native'});
    node.textContent = node.dataset.state + ' frames=' + frames;
});
EM_JS(int32, Scenario, (), { return Number(new URL(location.href).searchParams.get('scenario') || 0); });
EM_JS(int32, Ldr, (), { return new URL(location.href).searchParams.has('ldr') ? 1 : 0; });
EM_JS(void, Profile, (int32 hdr), { globalThis.__qaHdr = hdr; });
// clang-format on
#else
int32 Scenario() noexcept
{
    return 0;
}
int32 Ldr() noexcept
{
    return 0;
}
void Profile(int32) noexcept {}
int32 Selection() noexcept
{
    return 0;
}
int32 Reference() noexcept
{
    return 0;
}
int32 Paused() noexcept
{
    return 0;
}
void Report(int32, uint32, int32) noexcept {}
void Progress(uint32, uint32, uint32, uint32) noexcept {}
#endif
void Publish(int32 state) noexcept
{
    rhi::DeviceInfo info;
    static_cast<void>(rhi::GetDeviceInfo(gDevice, info));
    Report(state, gFrames, static_cast<int32>(info.Startup.SelectedBackend));
}
void Stop(bool failed) noexcept
{
    gDone = true;
    gFailed = failed;
    Progress(gStep, gFrames, static_cast<uint32>(gScene.Stage), gScene.Operation);
    Publish(failed ? 2 : 1);
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
    rhi::FrameInfo frame;
    if (rhi::GetFrameInfo(gDevice, gSurface, frame) == rhi::DeviceStatus::Ready)
    {
        rhi::SubmissionToken completion;
        static_cast<void>(rhi::EndFrame(gDevice, completion));
    }
    gRenderer.Reset();
    rhi::Shutdown();
}
bool Accepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
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
    Progress(gStep, gFrames, static_cast<uint32>(gScene.Stage), gScene.Operation);
    if (gDone || Paused() != 0)
    {
        return;
    }
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
        if (rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready)
        {
            Stop(true);
            return;
        }
        if (Accepted(gRenderer.Initialize(gDevice,
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
        if (Accepted(gRenderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                               ludus::shaders::renderer_composite::Fragment())) == false)
        {
            Stop(true);
            return;
        }
        gStep = 2;
    }
    if (gStep == 2)
    {
        if (Wait(gRenderer.PollViews()))
        {
            return;
        }
        if (Accepted(gRenderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                                  ludus::shaders::renderer_output::Fragment(),
                                                  ludus::shaders::renderer_shadow::Fragment())) == false)
        {
            Stop(true);
            return;
        }
        gStep = 3;
    }
    if (gStep == 3)
    {
        if (Wait(gRenderer.PollLighting()))
        {
            return;
        }
        if (Accepted(gRenderer.PrewarmLitProgram(ludus::shaders::renderer_clustered::Vertex(),
                                                 ludus::shaders::renderer_clustered::Fragment(),
                                                 ludus::shaders::renderer_clustered_shadow::Fragment(),
                                                 gScene.Program)) == false)
        {
            Stop(true);
            return;
        }
        rhi::RasterCapabilities caps;
        if (rhi::GetRasterCapabilities(gDevice, caps) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gScene.Hdr = caps.HdrColor && Ldr() == 0;
        gScene.Reference = Reference() != 0;
        gScene.Scenario = static_cast<uint32>(Scenario());
        Profile(gScene.Hdr ? 1 : 0);
        gStep = 4;
    }
    if (gStep == 4)
    {
        if (Wait(gScene.Advance(gRenderer, gDevice, gSurface)))
        {
            return;
        }
        gStep = 5;
    }

    const auto begun = rhi::BeginFrame(gDevice, gSurface);
    if (begun == rhi::DeviceStatus::Skipped)
    {
        return;
    }
    if (begun != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    rhi::SubmissionToken completion;
    if (gScene.Draw(gRenderer) != rhi::RasterStatus::Ready ||
        rhi::EndFrame(gDevice, completion) != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return;
    }
    ++gFrames;
    if (gFrames == 5 || gFrames == 80)
    {
        Publish(3);
    }
    if (gFrames == 120)
    {
        Stop(false);
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
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Selection = Selection() == 1   ? rhi::BackendSelection::WebGPU
                            : Selection() == 2 ? rhi::BackendSelection::WebGL2
                                               : rhi::BackendSelection::Auto;
    const auto started = rhi::CreateDevice({}, window, description, gDevice, gSurface);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        Stop(true);
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const auto start = time::NowTicks();
    while (gDone == false && time::NowTicks() - start < 15 * time::NANOSECONDS_PER_SECOND)
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
