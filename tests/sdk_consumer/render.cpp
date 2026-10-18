#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include "diagnostic.h"

#include <initializer_list>
#include <span>
#include <string_view>
#include <type_traits>

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
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
EM_JS(void, Report, (int32 state, uint32 frames, int32 error), {
    const status = document.getElementById('status');
    status.dataset.state = state === 1 ? 'passed' : state === 2 ? 'failed' : 'rendering';
    status.dataset.frames = frames;
    status.dataset.error = error;
    status.textContent = status.dataset.state + ' frames=' + frames;
});
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(void, ResizeCanvas, (uint32 width, uint32 height), {
    const canvas = document.getElementById('canvas'); canvas.width = width; canvas.height = height;
});
// clang-format on
#else
void Report(int32, uint32, int32) noexcept {}
void ResizeCanvas(uint32, uint32) noexcept {}
int32 Paused() noexcept
{
    return 0;
}
#endif
bool Accepted(rhi::ResourceStatus status) noexcept
{
    return status == rhi::ResourceStatus::Ready || status == rhi::ResourceStatus::Pending;
}
bool Start() noexcept
{
    rhi::WindowInfo window;
    window.Width = 96;
    window.Height = 64;
#if defined(LUDUS_PLATFORM_WEB)
    window.System = ludus::platform::WindowSystem::WebCanvas;
    window.CanvasSelector = "#canvas";
#endif
    gSurface = {};
    const auto result = rhi::CreateDevice({}, window, {}, gDevice, gSurface, &gCreationFailure);
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
    Report(failed ? 2 : 1, gFrames, error);
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
    const auto bytes = std::span<const uint8>(reinterpret_cast<const uint8*>(&uniforms), sizeof(uniforms));
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
    Report(0, gFrames, 0);
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
    if (Start() == false)
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
