#include "l2_scene.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_overlay.h"
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
namespace gt = ludus::graphics::text;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rr::Renderer gRenderer;
rr::OverlayRenderer gOverlays;
rr::OverlayList gList;
gt::CoverageAtlas gAtlas;
rhi::TextureHandle gTexture;
rr::Mesh gMesh;
rr::Snapshot gSnapshot;
rr::PreparedView gView;
rr::PreparedOverlay gOverlay;
uint32 gStep = 0, gFrames = 0;
bool gDone = false, gFailed = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(int32, Selection, (), { const s = new URL(location.href).searchParams.get('backend'); return s === 'webgpu' ? 1 : s === 'webgl2' ? 2 : 0; });
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(void, Report, (int32 state, uint32 frames, int32 backend), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : state === 3 ? 'rendering' : 'loading';
    if (state === 3) globalThis.__qaPause = true;
    Object.assign(node.dataset, {frames, backend: backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native'});
    node.textContent = node.dataset.state + ' frames=' + frames;
});
// clang-format on
#else
int32 Selection() noexcept
{
    return 0;
}
int32 Paused() noexcept
{
    return 0;
}
void Report(int32, uint32, int32) noexcept {}
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
    gOverlays.Reset();
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
    if (gDone || Paused() != 0)
    {
        return;
    }
    rhi::DeviceInfo info;
    auto device = rhi::GetDeviceInfo(gDevice, info);
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
        if (Accepted(gOverlays.Initialize(gDevice,
                                          ludus::shaders::renderer_overlay::Vertex(),
                                          ludus::shaders::renderer_overlay::Fragment())) == false)
        {
            Stop(true);
            return;
        }
        gStep = 3;
    }
    if (gStep == 3)
    {
        if (Wait(gOverlays.Poll()))
        {
            return;
        }
        const math::Vector3 vertices[]{{-.2F, -.2F, .5F}, {.2F, -.2F, .5F}, {.2F, .2F, .5F}, {-.2F, .2F, .5F}};
        const uint32 indices[]{0, 1, 2, 0, 2, 3};
        if (Accepted(gRenderer.CreateMesh({vertices, 4, indices, 6}, gMesh)) == false ||
            ludus::qa::BuildL2List(gList, gAtlas) != rhi::RasterStatus::Ready ||
            Accepted(gAtlas.Publish(gDevice, gTexture)) == false)
        {
            Stop(true);
            return;
        }
        gStep = 4;
    }
    if (gStep == 4)
    {
        if (Wait(gRenderer.GetStatus(gMesh)) || Wait(rhi::GetStatus(gDevice, gTexture)))
        {
            return;
        }
        rr::SceneItem item{gMesh};
        item.SourceId = 1;
        if (gRenderer.CreateSnapshot(&item, 1, gSnapshot) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gStep = 5;
    }
    if (Wait(gOverlays.Poll()))
    {
        return;
    }
    if (gStep == 5)
    {
        rr::SceneItem item{gMesh};
        item.SourceId = 1;
        item.Transform.Translation.X = (gFrames % 2 == 0) ? .4F : -.4F;
        item.Material.Color = {1, 0, 0, 1};
        if (gRenderer.ReplaceSnapshot(&item, 1, gSnapshot) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        rr::ViewDescription view;
        view.ScheduledUpload = true;
        rr::ViewReport report;
        const auto status = gRenderer.PrepareView(gSnapshot, view, gView, report);
        if (status == rhi::RasterStatus::CapacityExceeded)
        {
            return;
        }
        if (Accepted(status) == false)
        {
            Stop(true);
            return;
        }
        gStep = 6;
    }
    if (gStep == 6)
    {
        const auto status = gOverlays.Prepare(gList, gTexture, gOverlay);
        if (status == rhi::RasterStatus::CapacityExceeded)
        {
            return;
        }
        if (Accepted(status) == false)
        {
            Stop(true);
            return;
        }
        gStep = 7;
    }
    if (Wait(gRenderer.GetStatus(gView)) || Wait(gOverlays.GetStatus(gOverlay)))
    {
        return;
    }
    if (rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
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
    rhi::RasterPassDescription pass;
    pass.DepthStore = rhi::RasterStore::Store;
    rhi::SubmissionToken completion;
    if (gRenderer.DrawView(gView, pass) != rhi::RasterStatus::Ready ||
        gOverlays.Draw(gOverlay, pass) != rhi::RasterStatus::Ready ||
        rhi::EndFrame(gDevice, completion) != rhi::RasterStatus::Ready ||
        gRenderer.Release(gView) != rhi::RasterStatus::Ready || gOverlays.Release(gOverlay) != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return;
    }
    ++gFrames;
    gStep = 5;
    if (gFrames == 5)
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
