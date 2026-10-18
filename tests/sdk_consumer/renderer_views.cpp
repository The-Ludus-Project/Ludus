#include "l1_scene.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
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
rr::Mesh gMesh;
rr::Snapshot gSnapshot;
rr::PreparedView gViews[2];
rr::ViewReport gReports[2];
rhi::TextureHandle gTargets[2], gNextTargets[2];
rr::Presentation gPresentations[2], gNextPresentations[2];
rhi::SubmissionToken gCompletion;
uint32 gStep = 0, gFrames = 0, gWidth = 96, gHeight = 64;
bool gDone = false, gFailed = false, gWaiting = false, gZeroSkipped = false, gResized = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(int32, Selection, (), { const s = new URL(location.href).searchParams.get('backend'); return s === 'webgpu' ? 1 : s === 'webgl2' ? 2 : 0; });
EM_JS(int32, Reference, (), { return new URL(location.href).searchParams.has('reference') ? 1 : 0; });
EM_JS(int32, Fill, (), { return new URL(location.href).searchParams.get('fit') === 'fill' ? 1 : 0; });
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(void, ResizeCanvas, (uint32 width, uint32 height), { const canvas = document.querySelector('#canvas'); canvas.width = width; canvas.height = height; });
EM_JS(void, Report, (int32 state, uint32 frames, uint32 width, uint32 height, uint32 draws, uint32 culled, int32 backend, int32 zero, int32 resized), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : state === 3 ? 'rendering' : 'loading';
    if (state === 3) globalThis.__qaPause = true;
    Object.assign(node.dataset, {frames, width, height, draws, culled, zero, resized, backend: backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'native'});
    node.textContent = node.dataset.state + ' frames=' + frames;
});
// clang-format on
#else
int32 Selection() noexcept
{
    return 0;
}
int32 Reference() noexcept
{
    return 0;
}
int32 Fill() noexcept
{
    return 0;
}
int32 Paused() noexcept
{
    return 0;
}
void ResizeCanvas(uint32, uint32) noexcept {}
void Report(int32, uint32, uint32, uint32, uint32, uint32, int32, int32, int32) noexcept {}
#endif
void Publish(int32 state) noexcept
{
    rhi::DeviceInfo info;
    static_cast<void>(rhi::GetDeviceInfo(gDevice, info));
    Report(state,
           gFrames,
           gWidth,
           gHeight,
           gReports[0].Draws + gReports[1].Draws,
           gReports[0].Culled + gReports[1].Culled,
           static_cast<int32>(info.Startup.SelectedBackend),
           gZeroSkipped ? 1 : 0,
           gResized ? 1 : 0);
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
bool CreateTargets(uint32 size, rhi::TextureHandle (&targets)[2]) noexcept
{
    for (auto& target : targets)
    {
        if (Accepted(rhi::CreateTexture(gDevice, {size, size, rhi::RasterFormat::Rgba8Unorm, true}, {}, target)) ==
            false)
        {
            Stop(true);
            return false;
        }
    }
    return true;
}
void Tick() noexcept
{
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
        if (Accepted(gRenderer.CreateMesh({ludus::qa::L1_QUAD, 4, ludus::qa::L1_INDICES, 6}, gMesh)) == false ||
            CreateTargets(48, gTargets) == false)
        {
            Stop(true);
            return;
        }
        gStep = 3;
    }
    if (gStep == 3)
    {
        if (Wait(gRenderer.GetStatus(gMesh)))
        {
            return;
        }
        for (const auto target : gTargets)
        {
            if (Wait(rhi::GetStatus(gDevice, target)))
            {
                return;
            }
        }
        if (ludus::qa::BuildL1Snapshot(gRenderer, gMesh, gSnapshot) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        for (usize i = 0; i < 2; ++i)
        {
            if (Accepted(ludus::qa::BuildL1View(gRenderer,
                                                gSnapshot,
                                                i == 1,
                                                i == 1,
                                                Reference() != 0,
                                                gViews[i],
                                                gReports[i])) == false)
            {
                Stop(true);
                return;
            }
        }
        gStep = 4;
    }
    if (gStep == 4)
    {
        for (const auto view : gViews)
        {
            if (Wait(gRenderer.GetStatus(view)))
            {
                return;
            }
        }
        if (ludus::qa::PrepareL1Presentations(gRenderer,
                                              gTargets,
                                              gWidth,
                                              gHeight,
                                              gPresentations,
                                              Fill() ? rr::ScreenFit::Fill : rr::ScreenFit::Fit) !=
            rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gStep = 5;
    }
    if (gStep == 5)
    {
        for (const auto presentation : gPresentations)
        {
            if (Wait(gRenderer.GetStatus(presentation)))
            {
                return;
            }
        }
        if (gRenderer.Release(gSnapshot) != rhi::RasterStatus::Ready ||
            gRenderer.Release(gMesh) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gStep = 6;
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
            Stop(gZeroSkipped == false || gResized == false);
            return;
        }
    }
    if (gFrames == 20 && gZeroSkipped == false)
    {
        if (rhi::SetFrameTarget(gDevice, gSurface, {0, 0, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready ||
            rhi::BeginFrame(gDevice, gSurface) != rhi::DeviceStatus::Skipped)
        {
            Stop(true);
            return;
        }
        gZeroSkipped = true;
    }
    if (gFrames == 40 && gResized == false)
    {
        if (gStep == 6)
        {
            // Allocate complete replacement targets before dropping the old generation.
            if (CreateTargets(96, gNextTargets) == false)
            {
                return;
            }
            gStep = 7;
        }
        if (gStep == 7)
        {
            for (const auto target : gNextTargets)
            {
                if (Wait(rhi::GetStatus(gDevice, target)))
                {
                    return;
                }
            }
            if (ludus::qa::PrepareL1Presentations(gRenderer,
                                                  gNextTargets,
                                                  120,
                                                  80,
                                                  gNextPresentations,
                                                  Fill() ? rr::ScreenFit::Fill : rr::ScreenFit::Fit) !=
                rhi::RasterStatus::Ready)
            {
                Stop(true);
                return;
            }
            gStep = 8;
        }
        if (gStep == 8)
        {
            for (const auto presentation : gNextPresentations)
            {
                if (Wait(gRenderer.GetStatus(presentation)))
                {
                    return;
                }
            }
            for (usize i = 0; i < 2; ++i)
            {
                if (gRenderer.Release(gPresentations[i]) != rhi::RasterStatus::Ready ||
                    rhi::Destroy(gDevice, gTargets[i]) != rhi::RasterStatus::Ready)
                {
                    Stop(true);
                    return;
                }
                gPresentations[i] = gNextPresentations[i];
                gNextPresentations[i] = {};
                gTargets[i] = gNextTargets[i];
                gNextTargets[i] = {};
            }
            gWidth = 120;
            gHeight = 80;
            ResizeCanvas(gWidth, gHeight);
            gResized = true;
            gStep = 6;
        }
    }
    if (rhi::SetFrameTarget(gDevice, gSurface, {gWidth, gHeight, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready)
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
    rhi::FrameInfo acquired;
    if (rhi::GetFrameInfo(gDevice, gSurface, acquired) != rhi::DeviceStatus::Ready || acquired.Width != gWidth ||
        acquired.Height != gHeight ||
        ludus::qa::DrawL1Frame(gRenderer, gViews, gTargets, gPresentations) != rhi::RasterStatus::Ready ||
        rhi::EndFrame(gDevice, gCompletion) != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return;
    }
    ++gFrames;
    gWaiting = true;
    if (gFrames == 5 || gFrames == 45)
    {
        Publish(3);
    }
}
} // namespace
int main()
{
    rhi::WindowInfo window;
    window.Width = gWidth;
    window.Height = gHeight;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo settings;
    settings.Width = gWidth;
    settings.Height = gHeight;
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
    const auto started = rhi::CreateDevice({}, window, description, gDevice, gSurface, &failure);
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
