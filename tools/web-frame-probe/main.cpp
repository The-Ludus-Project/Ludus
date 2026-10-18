#include "internal/webgpu_probe.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/browser/window.h>

#include <emscripten.h>

namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics;
UniquePtr<ludus::platform::Window> gWindow;
WGPURenderPipeline gPipeline = nullptr;
uint32 gGeneration = 0;
uint32 gFrames = 0;
bool gBuilding = false;
bool gReady = false;
bool gDemoFailed = false;

// WGSL and graphics resources belong to this sample, not the engine.
constexpr const char* SHADER = R"(
@vertex fn vertex(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let points = array(vec2f(0.0, 0.7), vec2f(-0.7, -0.7), vec2f(0.7, -0.7));
    return vec4f(points[i], 0.0, 1.0);
}
@fragment fn fragment() -> @location(0) vec4f { return vec4f(1.0, 0.65, 0.15, 1.0); }
)";
// clang-format off
EM_JS(void, Status, (uint32 state, uint32 error, uint32 frames, uint32 width, uint32 height, int32 ready, int32 failed), {
    const names = ['idle', 'pending', 'ready', 'failed', 'device lost'];
    const status = document.getElementById('status');
    const errors = ['none', 'invalid canvas', 'instance unavailable', 'adapter unavailable', 'device unavailable', 'surface unavailable', 'rendering unavailable', 'validation error', 'device lost', 'generation exhausted'];
    status.dataset.state = failed ? 'failed' : names[state];
    status.dataset.frames = frames;
    status.textContent = status.dataset.state + ' | ' + errors[error] + ' | pipeline ' + (failed ? 'validation failed' : (ready ? 'ready' : (state == 2 ? 'pending' : 'waiting for device'))) + ' | frames ' + frames + ' | framebuffer ' + width + ' × ' + height;
});
// clang-format on
void ReleaseDemo() noexcept
{
    // Never reuse a callback generation within a running page.
    if (gGeneration != 0xFFFFFFFFU)
    {
        ++gGeneration;
    }
    if (gPipeline != nullptr)
    {
        wgpuRenderPipelineRelease(gPipeline);
    }
    gPipeline = nullptr;
    gReady = false;
    gBuilding = false;
}
void BuildDemo() noexcept
{
    gBuilding = true;
    const auto device = rhi::backend::ProbeDevice();
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = { .data = SHADER, .length = WGPU_STRLEN };
    WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDescriptor.nextInChain = &source.chain;
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(device, &shaderDescriptor);
    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = rhi::backend::ProbeFormat();
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader;
    fragment.entryPoint = { .data = "fragment", .length = WGPU_STRLEN };
    fragment.targetCount = 1;
    fragment.targets = &color;
    WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = WGPUTextureFormat_Depth24Plus;
    depth.depthWriteEnabled = WGPUOptionalBool_False;
    depth.depthCompare = WGPUCompareFunction_Always;
    descriptor.depthStencil = &depth;
    descriptor.vertex.module = shader;
    descriptor.vertex.entryPoint = { .data = "vertex", .length = WGPU_STRLEN };
    descriptor.fragment = &fragment;
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    if (shader != nullptr)
    {
        gPipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    }
    if (shader != nullptr)
    {
        wgpuShaderModuleRelease(shader);
    }
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    // Opaque generation, never dereferenced by the foreign callback.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    callback.userdata1 = reinterpret_cast<void*>(static_cast<usize>(gGeneration));
    callback.callback =
        [](WGPUPopErrorScopeStatus status, WGPUErrorType error, WGPUStringView, void* data, void*) noexcept {
            if (static_cast<uint32>(reinterpret_cast<usize>(data)) != gGeneration ||
                rhi::GetStartup().State != rhi::StartupState::Ready)
            {
                return;
            }
            if (status != WGPUPopErrorScopeStatus_Success || error != WGPUErrorType_NoError || gPipeline == nullptr)
            {
                LUDUS_LOG_WARN(logging::LOG_TEMP, "W5 WGSL/pipeline validation failed");
                gDemoFailed = true;
                ReleaseDemo();
                rhi::Shutdown();
                return;
            }
            gReady = true;
        };
    (void)wgpuDevicePopErrorScope(device, callback);
}
void Frame() noexcept
{
    auto startup = rhi::GetStartup();
    ludus::platform::browser::WindowState window;
    if (gWindow)
    {
        (void)gWindow->HandleEvent({});
        (void)gWindow->GetBrowserState(window);
    }
    if (startup.State == rhi::StartupState::Ready)
    {
        (void)gWindow->SetBrowserFramebufferLimit(startup.MaxTextureDimension2D);
        (void)gWindow->GetBrowserState(window);
        if (gBuilding == false)
        {
            BuildDemo();
        }
        if (gReady)
        {
            const float64 pulse = static_cast<float64>(gFrames % 120) / 120.0;
            (void)rhi::SetFrameTarget(
            {
                .Width = window.FramebufferWidth,
                .Height = window.FramebufferHeight,
                .Red = 0.05 + pulse * 0.1,
                .Green = 0.05,
                .Blue = 0.15 + pulse * 0.2,
            });
            if (rhi::BeginFrameStatus() == rhi::FrameStatus::Ready)
            {
                const auto pass = rhi::backend::ProbePass();
                // A centered square viewport preserves geometry on any canvas aspect.
                const uint32 size = window.FramebufferWidth < window.FramebufferHeight ? window.FramebufferWidth
                                                                                       : window.FramebufferHeight;
                wgpuRenderPassEncoderSetViewport(pass,
                                                 static_cast<float32>(window.FramebufferWidth - size) * 0.5F,
                                                 static_cast<float32>(window.FramebufferHeight - size) * 0.5F,
                                                 static_cast<float32>(size),
                                                 static_cast<float32>(size),
                                                 0,
                                                 1);
                wgpuRenderPassEncoderSetPipeline(pass, gPipeline);
                wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
                if (rhi::EndFrameStatus() == rhi::FrameStatus::Ready)
                {
                    ++gFrames;
                }
            }
        }
    }
    startup = rhi::GetStartup();
    if (startup.State != rhi::StartupState::Ready && gBuilding)
    {
        ReleaseDemo();
    }
    Status(static_cast<uint32>(startup.State),
           static_cast<uint32>(startup.Error),
           gFrames,
           window.FramebufferWidth,
           window.FramebufferHeight,
           gReady ? 1 : 0,
           gDemoFailed ? 1 : 0);
}
} // namespace
extern "C" EMSCRIPTEN_KEEPALIVE void Stop() noexcept
{
    ReleaseDemo();
    rhi::Shutdown();
}
extern "C" EMSCRIPTEN_KEEPALIVE void Restart() noexcept
{
    Stop();
    if (gGeneration == 0xFFFFFFFFU)
    {
        gDemoFailed = true;
        return;
    }
    gDemoFailed = false;
    gFrames = 0;
    gWindow.Reset();
    ludus::platform::WindowManager manager;
    if (manager.Initialize({}) && manager.CreateWindow({}, gWindow))
    {
        (void)rhi::Start({ .Name = "W5 frames" }, gWindow->GetNativeWindowInfo(), rhi::BackendSelection::WebGPU);
    }
}
int main()
{
    Restart();
    emscripten_set_main_loop(Frame, 0, true);
}
