#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include "diagnostic.h"

#include <initializer_list>
#include <string_view>
#include <type_traits>

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#endif
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
struct alignas(16) Uniforms final
{
    float32 Resolution[2];
    float32 Elapsed;
    float32 Padding0;
    float32 Direction[3];
    float32 Padding1;
    float32 Tint[4];
};
static_assert(std::is_standard_layout_v<Uniforms>);
static_assert(offsetof(Uniforms, Resolution) == 0);
static_assert(offsetof(Uniforms, Elapsed) == 8);
static_assert(offsetof(Uniforms, Direction) == 16);
static_assert(offsetof(Uniforms, Tint) == 32);
static_assert(sizeof(Uniforms) == 48 && alignof(Uniforms) == 16);
// Initial canvas size in pixels; frames then alternate landscape and portrait.
constexpr uint32 INITIAL_WIDTH = 96;
constexpr uint32 INITIAL_HEIGHT = 64;
rhi::WindowInfo gWindow;
#if defined(LUDUS_PLATFORM_WEB)
// Platform owns the canvas, so an Auto fallback from WebGPU can swap in a fresh node for WebGL 2.
ludus::foundation::core::UniquePtr<ludus::platform::Window> gPlatformWindow;
#endif
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::StartupInfo gCreationFailure;
rhi::ShaderHandle gVertex, gFragment;
rhi::UniformHandle gUniform;
rhi::PipelineHandle gPipeline;
uint32 gFrames = 0;
uint32 gSession = 0;
bool gCreated = false;
bool gCancelPending = false;
bool gPipelineCreated = false;
bool gDone = false;
bool gFailed = false;

#if defined(LUDUS_PLATFORM_WEB)
// Reporting belongs to the test application; no GPU handle or private header.
// clang-format off
EM_JS(void, Report, (int32 state, uint32 frames, int32 error, int32 backend), {
    const status = document.getElementById('status');
    status.dataset.state = state === 1 ? 'passed' : state === 2 ? 'failed' : 'rendering';
    status.dataset.frames = frames;
    status.dataset.error = error;
    status.dataset.backend = backend === 1 ? 'webgpu' : backend === 2 ? 'webgl2' : 'none';
    status.textContent = status.dataset.state + ' frames=' + frames;
});
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
// ?backend=webgpu or webgl2 forces that API; anything else is Auto.
EM_JS(int32, Selection, (), {
    const value = new URL(location.href).searchParams.get('backend');
    return value === 'webgpu' ? 1 : value === 'webgl2' ? 2 : 0;
});
EM_JS(void, ResizeCanvas, (uint32 width, uint32 height), {
    const canvas = document.getElementById('canvas'); canvas.width = width; canvas.height = height;
});
// clang-format on
#else
void Report(int32, uint32, int32, int32) noexcept {}
void ResizeCanvas(uint32, uint32) noexcept {}
int32 Paused() noexcept
{
    return 0;
}
int32 Selection() noexcept
{
    return 0;
}
#endif
/// Backend reported to the page: 1 WebGPU, 2 WebGL 2, 0 none or native.
int32 GetReportedBackend(rhi::DeviceStatus state, const rhi::DeviceInfo& info) noexcept
{
    if (state != rhi::DeviceStatus::Ready)
    {
        return 0;
    }
    return info.Startup.SelectedBackend == rhi::Backend::WebGPU   ? 1
           : info.Startup.SelectedBackend == rhi::Backend::WebGL2 ? 2
                                                                  : 0;
}
int32 gBackend = 0;
bool Accepted(rhi::ResourceStatus status) noexcept
{
    return status == rhi::ResourceStatus::Ready || status == rhi::ResourceStatus::Pending;
}
/// Creates the window every device session draws to; on the web, a Platform-owned canvas.
bool CreateDrawingWindow() noexcept
{
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo windowDescription;
    windowDescription.Width = INITIAL_WIDTH;
    windowDescription.Height = INITIAL_HEIGHT;
    windowDescription.CanvasSelector = "#canvas";
    windowDescription.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(windowDescription, gPlatformWindow) == false)
    {
        return false;
    }
    gWindow = gPlatformWindow->GetNativeWindowInfo();
#else
    gWindow.Width = INITIAL_WIDTH;
    gWindow.Height = INITIAL_HEIGHT;
#endif
    return true;
}
bool Start() noexcept
{
    gSurface = {};
    rhi::DeviceDescription description;
    description.Selection = Selection() == 1   ? rhi::BackendSelection::WebGPU
                            : Selection() == 2 ? rhi::BackendSelection::WebGL2
                                               : rhi::BackendSelection::Auto;
    const rhi::DeviceStatus result = rhi::CreateDevice({}, gWindow, description, gDevice, gSurface, &gCreationFailure);
    return result == rhi::DeviceStatus::Ready || result == rhi::DeviceStatus::Pending;
}
void Stop(bool failed) noexcept
{
    gFailed = failed;
    gDone = true;
    rhi::DeviceInfo info;
    const auto state = rhi::GetDeviceInfo(gDevice, info);
    const auto error =
        static_cast<int32>(state == rhi::DeviceStatus::InvalidHandle ? gCreationFailure.Error : info.Startup.Error);
    (void)rhi::DestroyDevice(gDevice);
    Report(failed ? 2 : 1, gFrames, error, gBackend);
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
}
void Tick() noexcept
{
    if (Paused() != 0)
    {
        return;
    }
    if (gDone)
    {
        return;
    }
    rhi::DeviceInfo info;
    const auto startup = rhi::GetDeviceInfo(gDevice, info);
    if (startup == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (startup != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    gBackend = GetReportedBackend(startup, info);
    if (gCreated == false)
    {
        if (Accepted(rhi::CreateShader(gDevice, ludus::shaders::diagnostic::Vertex(), gVertex)) == false ||
            Accepted(rhi::CreateShader(gDevice, ludus::shaders::diagnostic::Fragment(), gFragment)) == false ||
            Accepted(rhi::CreateUniform(gDevice, sizeof(Uniforms), gUniform)) == false)
        {
            Stop(true);
            return;
        }
        gCreated = true;
        if (gCancelPending)
        {
            const auto stale = gVertex;
            (void)rhi::DestroyDevice(gDevice);
            gVertex = {};
            gFragment = {};
            gUniform = {};
            gPipeline = {};
            gCreated = false;
            gPipelineCreated = false;
            gCancelPending = false;
            if (rhi::GetStatus(gDevice, stale) != rhi::ResourceStatus::InvalidHandle || Start() == false)
            {
                Stop(true);
            }
            return;
        }
    }
    for (auto status :
         {rhi::GetStatus(gDevice, gVertex), rhi::GetStatus(gDevice, gFragment), rhi::GetStatus(gDevice, gUniform)})
    {
        if (status == rhi::ResourceStatus::Pending)
        {
            return;
        }
        if (status != rhi::ResourceStatus::Ready)
        {
            Stop(true);
            return;
        }
    }
    if (gPipelineCreated == false)
    {
        if (Accepted(rhi::CreatePipeline(gDevice, {gVertex, gFragment, gUniform}, gPipeline)) == false)
        {
            Stop(true);
            return;
        }
        gPipelineCreated = true;
    }
    if (rhi::GetStatus(gDevice, gPipeline) == rhi::ResourceStatus::Pending)
    {
        return;
    }
    if (rhi::GetStatus(gDevice, gPipeline) != rhi::ResourceStatus::Ready)
    {
        Stop(true);
        return;
    }
    const bool portrait = (gFrames % 16) >= 8;
    const uint32 width = portrait ? 64 : 96, height = portrait ? 96 : 64;
    Uniforms uniforms =
    {
        .Resolution = {static_cast<float32>(width), static_cast<float32>(height)},
        .Elapsed = (gFrames % 8) < 4 ? 0.0F : 2.0F,
        .Padding0 = 0,
        .Direction = {0.2F, 0.4F, 0.6F},
        .Padding1 = 0,
        .Tint = {0.6F, 0.4F, 0.2F, 1.0F},
    };
    const ludus::foundation::BorrowedView<const uint8> bytes =
        ludus::foundation::BorrowedView<const uint8>(reinterpret_cast<const uint8*>(&uniforms), sizeof(uniforms));
    if (rhi::UpdateUniform(gDevice, gUniform, bytes) != rhi::ResourceStatus::Ready)
    {
        Stop(true);
        return;
    }
    // Exercise minimized/skipped acquisition without opening a frame.
    if (rhi::SetFrameTarget(gDevice, gSurface, {}) != rhi::DeviceStatus::Ready ||
        rhi::BeginFrame(gDevice, gSurface) != rhi::DeviceStatus::Skipped ||
        rhi::EndFrame(gDevice) != rhi::DeviceStatus::InvalidState)
    {
        Stop(true);
        return;
    }
    ResizeCanvas(width, height);
    if (rhi::SetFrameTarget(gDevice, gSurface, { .Width = width, .Height = height }) != rhi::DeviceStatus::Ready)
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
    rhi::FrameInfo frameInfo;
    if (rhi::GetFrameInfo(gDevice, gSurface, frameInfo) != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    uniforms.Resolution[0] = static_cast<float32>(frameInfo.Width);
    uniforms.Resolution[1] = static_cast<float32>(frameInfo.Height);
    if (rhi::UpdateUniform(gDevice, gUniform, bytes) != rhi::ResourceStatus::Ready ||
        rhi::DrawFullscreen(gDevice, gPipeline) != rhi::ResourceStatus::Ready ||
        rhi::EndFrame(gDevice) != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    ++gFrames;
    Report(0, gFrames, 0, gBackend);
    if (gFrames == 16 && gSession == 0)
    {
        const auto stale = gPipeline;
        if (rhi::Destroy(gDevice, gUniform) != rhi::ResourceStatus::InUse ||
            rhi::Destroy(gDevice, gPipeline) != rhi::ResourceStatus::Ready ||
            rhi::Destroy(gDevice, gVertex) != rhi::ResourceStatus::Ready ||
            rhi::Destroy(gDevice, gFragment) != rhi::ResourceStatus::Ready ||
            rhi::Destroy(gDevice, gUniform) != rhi::ResourceStatus::Ready)
        {
            Stop(true);
            return;
        }
        (void)rhi::DestroyDevice(gDevice);
        if (rhi::GetStatus(gDevice, stale) != rhi::ResourceStatus::InvalidHandle)
        {
            Stop(true);
            return;
        }
        gVertex = {};
        gFragment = {};
        gUniform = {};
        gPipeline = {};
        gCreated = false;
        gPipelineCreated = false;
        ++gSession;
        if (Start() == false)
        {
            Stop(true);
        }
    }
    if (gFrames == 32)
    {
        Stop(false);
    }
}
} // namespace
int main(int argc, char** argv)
{
    gCancelPending = argc > 1 && std::string_view(argv[1]) == "cancel-pending";
    if (CreateDrawingWindow() == false || Start() == false)
    {
        Stop(true);
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, false);
#else
    while (gDone == false)
    {
        Tick();
    }
#endif
    return gFailed ? 1 : 0;
}
