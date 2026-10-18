#include <ludus/foundation/base/types.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>

using namespace ludus::foundation;

namespace
{
// This probe deliberately links no engine runtime. Handles outlive asynchronous
// requests and animation callbacks; a reload destroys the browser's module.
struct Probe final
{
    WGPUInstance Instance = nullptr;
    WGPUAdapter Adapter = nullptr;
    WGPUDevice Device = nullptr;
    WGPUQueue Queue = nullptr;
    WGPUSurface Surface = nullptr;
    bool Failed = false;
    bool CompatibilityRequested = false;
    uint32 Frames = 0;
};

Probe gProbe;

// clang-format off
EM_JS(void, showStatus, (const char* state, const char* message), {
    const status = document.getElementById('status');
    status.dataset.state = UTF8ToString(state);
    status.textContent = UTF8ToString(message);
});

EM_JS(void, reportFrame, (uint32 frames), {
    document.getElementById('status').dataset.frames = String(frames);
});
EM_JS(void, appendDiagnostic, (const char* label, const char* message, usize length), {
    document.getElementById('diagnostics').textContent +=
        UTF8ToString(label) + ': ' + (message && length ? UTF8ToString(message, length) : '(no message)') + '\n';
});
// clang-format on

void diagnose(const char* label, WGPUStringView message) noexcept
{
    // The API permits both explicit lengths and null-terminated views.
    // Bound display output without requiring allocation or trusting termination.
    constexpr usize maxLength = 4096;
    const usize length = message.length < maxLength ? message.length : maxLength;
    appendDiagnostic(label, message.data, length);
}

void fail(const char* message) noexcept
{
    gProbe.Failed = true;
    showStatus("failed", message);
}

// Bounded probe diagnostic conversion; no locale, allocation or format library.
void appendUnsigned(char*& cursor, uint32 value) noexcept
{
    char digits[10];
    usize count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
    {
        *cursor++ = digits[--count];
    }
    *cursor = '\0';
}

EM_BOOL renderFrame(float64, void*) noexcept
{
    if (gProbe.Failed)
    {
        return EM_FALSE;
    }

    WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(gProbe.Surface, &surfaceTexture);
    if (surfaceTexture.texture == nullptr ||
        (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
         surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal))
    {
        if (surfaceTexture.texture)
        {
            wgpuTextureRelease(surfaceTexture.texture);
        }
        fail("WebGPU could not acquire a canvas frame. Reload to retry.");
        return EM_FALSE;
    }

    WGPUTextureView view = wgpuTextureCreateView(surfaceTexture.texture, nullptr);
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(gProbe.Device, nullptr);
    if (view == nullptr || encoder == nullptr)
    {
        if (view)
        {
            wgpuTextureViewRelease(view);
        }
        if (encoder)
        {
            wgpuCommandEncoderRelease(encoder);
        }
        wgpuTextureRelease(surfaceTexture.texture);
        fail("WebGPU could not create frame resources.");
        return EM_FALSE;
    }

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    // A bounded triangle pulse shows frame progress without a math-library dependency.
    const uint32 phase = gProbe.Frames % 240;
    const uint32 intensity = phase < 120 ? phase : 240 - phase;
    color.clearValue.r = 0.05 + static_cast<float64>(intensity) / 300.0;
    color.clearValue.g = 0.12;
    color.clearValue.b = 0.45;
    color.clearValue.a = 1.0;

    WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
    if (pass)
    {
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, nullptr);
        if (command)
        {
            wgpuQueueSubmit(gProbe.Queue, 1, &command);
            wgpuCommandBufferRelease(command);
            reportFrame(++gProbe.Frames);
        }
        else
        {
            fail("WebGPU could not finish frame commands.");
        }
    }
    else
    {
        fail("WebGPU could not begin the clear pass.");
    }
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(surfaceTexture.texture);
    // Browser WebGPU presents after the animation callback returns. Calling
    // wgpuSurfacePresent here aborts in the pinned Emdawnwebgpu implementation.
    return gProbe.Failed ? EM_FALSE : EM_TRUE;
}

void deviceReady(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void*, void*) noexcept
{
    diagnose("Device request", message);
    if (status != WGPURequestDeviceStatus_Success || device == nullptr)
    {
        fail("WebGPU device request failed. Check browser/GPU support and reload.");
        return;
    }
    gProbe.Device = device;
    gProbe.Queue = wgpuDeviceGetQueue(device);

    WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(gProbe.Surface, gProbe.Adapter, &capabilities) != WGPUStatus_Success ||
        capabilities.formatCount == 0 || gProbe.Queue == nullptr)
    {
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        fail("WebGPU canvas capabilities are unavailable.");
        return;
    }
    WGPUSurfaceConfiguration configuration = WGPU_SURFACE_CONFIGURATION_INIT;
    configuration.device = device;
    configuration.format = capabilities.formats[0];
    configuration.width = 640;
    configuration.height = 360;
    configuration.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(gProbe.Surface, &configuration);
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    if (gProbe.Failed == false)
    {
        showStatus("ready",
                   gProbe.CompatibilityRequested
                       ? "WebGPU is ready (compatibility request). The canvas color should animate."
                       : "WebGPU is ready (core request). The canvas color should animate.");
        emscripten_request_animation_frame_loop(renderFrame, nullptr);
    }
}

void adapterReady(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void*, void*) noexcept;

void requestAdapter(WGPUFeatureLevel featureLevel) noexcept
{
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.featureLevel = featureLevel;
    options.compatibleSurface = gProbe.Surface;
    WGPURequestAdapterCallbackInfo callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = adapterReady;
    (void)wgpuInstanceRequestAdapter(gProbe.Instance, &options, callback);
}

void adapterReady(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void*, void*) noexcept
{
    char label[64];
    char* cursor = label;
    const char* prefix =
        gProbe.CompatibilityRequested ? "Compatibility adapter request (status " : "Core adapter request (status ";
    while (*prefix != '\0')
    {
        *cursor++ = *prefix++;
    }
    appendUnsigned(cursor, static_cast<uint32>(status));
    *cursor++ = ')';
    *cursor = '\0';
    diagnose(label, message);
    if (status != WGPURequestAdapterStatus_Success || adapter == nullptr)
    {
        if (adapter)
        {
            wgpuAdapterRelease(adapter);
        }
        if (gProbe.CompatibilityRequested == false)
        {
            gProbe.CompatibilityRequested = true;
            showStatus("initializing", "Core adapter unavailable; trying WebGPU compatibility mode…");
            requestAdapter(WGPUFeatureLevel_Compatibility);
            return;
        }
        fail("No WebGPU adapter is available in core or compatibility mode. See diagnostics below.");
        return;
    }
    gProbe.Adapter = adapter;
    showStatus("initializing", "WebGPU adapter acquired; requesting a device…");
    WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    descriptor.deviceLostCallbackInfo.callback =
        [](WGPUDevice const*, WGPUDeviceLostReason, WGPUStringView message, void*, void*) noexcept {
            diagnose("Device lost", message);
            fail("WebGPU device lost. Reload to retry.");
        };
    descriptor.uncapturedErrorCallbackInfo.callback =
        [](WGPUDevice const*, WGPUErrorType, WGPUStringView message, void*, void*) noexcept {
            diagnose("Uncaptured error", message);
            fail("WebGPU reported a validation error. Reload to retry.");
        };
    WGPURequestDeviceCallbackInfo callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = deviceReady;
    (void)wgpuAdapterRequestDevice(adapter, &descriptor, callback);
}
} // namespace

int main()
{
    showStatus("initializing", "Requesting a WebGPU adapter…");
    gProbe.Instance = wgpuCreateInstance(nullptr);
    if (gProbe.Instance == nullptr)
    {
        fail("WebGPU instance creation failed.");
        return 1;
    }
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = { .data = "#canvas", .length = WGPU_STRLEN };
    WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &canvas.chain;
    gProbe.Surface = wgpuInstanceCreateSurface(gProbe.Instance, &descriptor);
    if (gProbe.Surface == nullptr)
    {
        fail("WebGPU canvas surface creation failed.");
        return 1;
    }
    requestAdapter(WGPUFeatureLevel_Core);
    // Emdawnwebgpu keeps pending callbacks alive after main returns.
    return 0;
}
