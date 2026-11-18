#include "internal/backend.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/profile.h"
#include "internal/raster.h"
#include "internal/resources.h"
#include "internal/surface.h"
#include "internal/webgpu_probe.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include <cstring>
#include <string_view>

#include <emscripten.h>
#include <webgpu/webgpu.h>

// On the browser Auto build this backend is compiled into backend::webgpu and a
// dispatcher (rhi_web.cpp) routes to it. When built standalone it keeps defining
// backend:: directly. LUDUS_RHI_WEBGPU_NAMESPACE selects the leaf namespace.
#if !defined(LUDUS_RHI_WEBGPU_NAMESPACE)
#    define LUDUS_RHI_WEBGPU_NAMESPACE backend
#endif
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
{
void RasterFrameExtent() noexcept;
void TimingBegin() noexcept;
void TimingEnd() noexcept;
void TimingSubmitted() noexcept;
namespace
{
using namespace ludus::foundation;
constexpr logging::LogCategory LOG_RHI{"RHI-WebGPU"};
WGPUInstance gInstance = nullptr;
WGPUAdapter gAdapter = nullptr;
WGPUDevice gDevice = nullptr;
WGPUQueue gQueue = nullptr;
bool gCompatibility = false;
uint32 gMaxDimension = 0;
uint64 gMaxUniformSize = 0;
uint32 gSession = 0;

/// @brief R14b presentation record of one canvas surface of the device.
///
/// Slot 0 is the primary canvas Start connects; slots 1.. are canvases CreateSurface
/// connected afterwards. Every slot shares the primary's device and color format.
/// See docs/development/fullscreen-rendering.md, "Multiple surfaces per device (R14b)".
struct WebSurface final
{
    /// Port surface (a GPUCanvasContext) of this slot's canvas.
    WGPUSurface SurfaceOrNull = nullptr;
    /// Borrowed selector; WindowInfo keeps it naming the same canvas until disconnection.
    const char* SelectorOrNull = nullptr;
    /// Last target SetTarget accepted for this slot.
    FrameTarget Target;
    /// Last configuration applied to the canvas context; width/height are canvas pixels.
    WGPUSurfaceConfiguration Configuration = WGPU_SURFACE_CONFIGURATION_INIT;
    bool IsConfigured = false;
    bool IsConnected = false;
    /// A configuration failure arrived while this slot's frame was open; retire it at End.
    bool IsLossPending = false;
    /// Callback identity of this slot's current occupant; zero for a free slot.
    uint32 Ticket = 0;
    /// Frames of this slot submitted to the queue.
    uint64 SubmittedFrames = 0;
    /// Frames whose submitted work the queue reported done.
    uint64 RenderedFrames = 0;
    /// SubmittedFrames and Ticket when the in-flight completion query was issued.
    uint64 QueriedFrames = 0;
    uint32 QueriedTicket = 0;
    /// Frames that submitted the canvas texture, which the browser presents after the task.
    uint64 PresentedFrames = 0;
};
StaticArray<WebSurface, MAX_DEVICE_SURFACES> gSurfaces;
/// Slot routed to SetTarget/Begin/End; see backend::SelectSurface.
usize gActiveSurface = 0;
/// Last callback ticket issued. Tickets keep growing across sessions, so a slot reused by a
/// new surface never matches a late callback of its previous occupant.
uint32 gLastTicket = 0;
/// Queue completion queries in flight, zero or one; see RequestSurfaceCompletion.
usize gPendingCompletionQueries = 0;

WebSurface& GetActiveSurface() noexcept
{
    return gSurfaces[gActiveSurface];
}
/// Color format every surface and surface pipeline uses: the primary canvas's format.
WGPUTextureFormat GetSurfaceFormat() noexcept
{
    return gSurfaces[0].Configuration.format;
}
uint32 TakeTicket() noexcept
{
    ++gLastTicket;
    if (gLastTicket == 0)
    {
        ++gLastTicket;
    }
    return gLastTicket;
}
/// Slot whose current occupant owns `ticket`, or MAX_DEVICE_SURFACES for a retired surface.
usize FindSurfaceTicket(uint32 ticket) noexcept
{
    for (usize slot = 0; slot < gSurfaces.GetSize(); ++slot)
    {
        if (ticket != 0 && gSurfaces[slot].Ticket == ticket)
        {
            return slot;
        }
    }
    return MAX_DEVICE_SURFACES;
}
WGPUTexture gTexture = nullptr;
WGPUTextureView gView = nullptr;
WGPUCommandEncoder gEncoder = nullptr;
WGPUTexture gDepthTexture = nullptr;
WGPUTextureView gDepthView = nullptr;
WGPURenderPassEncoder gPass = nullptr;

void ReleaseFrame() noexcept
{
    if (gPass != nullptr)
    {
        wgpuRenderPassEncoderRelease(gPass);
    }
    if (gEncoder != nullptr)
    {
        wgpuCommandEncoderRelease(gEncoder);
    }
    if (gView != nullptr)
    {
        wgpuTextureViewRelease(gView);
    }
    if (gTexture != nullptr)
    {
        wgpuTextureRelease(gTexture);
    }
    if (gDepthView != nullptr)
    {
        wgpuTextureViewRelease(gDepthView);
    }
    if (gDepthTexture != nullptr)
    {
        wgpuTextureRelease(gDepthTexture);
    }
    gDepthView = nullptr;
    gDepthTexture = nullptr;
    gPass = nullptr;
    gEncoder = nullptr;
    gView = nullptr;
    gTexture = nullptr;
}
FrameStatus FrameFailure() noexcept
{
    internal::Fail(gSession, StartupError::RenderingUnavailable);
    return FrameStatus::Failed;
}

// Validate the foreign target before the port's assertion-based surface API.
// No callback borrows this selector or application storage. Platform prepares a fresh canvas before
// this check. Acquiring WebGPU commits its context mode; a capability failure
// asks Platform to replace it before attempting WebGL 2. Check getContext here
// so an incompatible unmanaged canvas fails before the port assertion.
// clang-format off
EM_JS(int32, CanCreateSurface, (const char* selector), {
    try {
        if (typeof navigator.gpu == 'undefined') return -1;
        const canvas = document.querySelector(UTF8ToString(selector));
        if (!(canvas instanceof HTMLCanvasElement) || !canvas.isConnected) return 0;
        // Validate the actual surface too before the port assertion boundary.
        return canvas.getContext('webgpu') ? 1 : -1;
    } catch (_) { return 0; }
});
// R14b: a created canvas stops accepting frames when it leaves the document.
EM_JS(int32, IsCanvasConnected, (const char* selector), {
    try {
        const canvas = document.querySelector(UTF8ToString(selector));
        return canvas instanceof HTMLCanvasElement && canvas.isConnected ? 1 : 0;
    } catch (_) { return 0; }
});
// R14b: distinct selector strings may name one canvas, which has one WebGPU context.
EM_JS(int32, IsSameCanvas, (const char* first, const char* second), {
    try {
        const canvas = document.querySelector(UTF8ToString(first));
        return canvas !== null && canvas === document.querySelector(UTF8ToString(second)) ? 1 : 0;
    } catch (_) { return 0; }
});
// clang-format on

void* Userdata(uint32 token) noexcept
{
    // C API userdata carries an opaque numeric generation, never a memory address.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void*>(static_cast<usize>(token));
}
uint32 Token(void* userdata) noexcept
{
    return static_cast<uint32>(reinterpret_cast<usize>(userdata));
}
void Diagnose(WGPUStringView message) noexcept
{
    if (message.data != nullptr)
    {
        usize length = message.length < 2048 ? message.length : 2048;
        if (message.length == WGPU_STRLEN)
        {
            length = 0;
            while (length < 2048 && message.data[length] != '\0')
            {
                ++length;
            }
        }
        LUDUS_LOG_WARN(LOG_RHI, "WebGPU: {}", std::string_view(message.data, length));
    }
}
/// Release one created slot's canvas configuration and port surface. WebGPU keeps the canvas
/// textures of submitted work alive until that work completes, so retirement needs no wait.
/// The slot keeps its counters for GetSurfaceInfo; its ticket is cleared, so completions of
/// frames still in flight are not counted.
void ReleaseSurface(usize slot) noexcept
{
    WebSurface& surface = gSurfaces[slot];
    if (surface.IsConfigured == true && surface.SurfaceOrNull != nullptr)
    {
        wgpuSurfaceUnconfigure(surface.SurfaceOrNull);
    }
    if (surface.SurfaceOrNull != nullptr)
    {
        wgpuSurfaceRelease(surface.SurfaceOrNull);
    }
    const uint64 submittedFrames = surface.SubmittedFrames;
    const uint64 renderedFrames = surface.RenderedFrames;
    const uint64 presentedFrames = surface.PresentedFrames;
    surface = {};
    surface.SubmittedFrames = submittedFrames;
    surface.RenderedFrames = renderedFrames;
    surface.PresentedFrames = presentedFrames;
}
/// A created canvas stopped accepting frames: retire only its presentation and report it lost.
/// The device, its resources and its other surfaces keep working.
void LoseCreatedSurface(usize slot) noexcept
{
    ReleaseSurface(slot);
    internal::LoseSurface(slot);
}
/// Validation result of a created canvas's configuration. A destroyed surface, or a new
/// surface that reuses its slot, carries another ticket, so a late result is ignored.
void SurfaceConfigured(WGPUPopErrorScopeStatus status,
                       WGPUErrorType error,
                       WGPUStringView message,
                       void* session,
                       void* ticket) noexcept
{
    if (internal::Current(Token(session)) == false)
    {
        return;
    }
    const usize slot = FindSurfaceTicket(Token(ticket));
    if (slot == 0 || slot == MAX_DEVICE_SURFACES ||
        (status == WGPUPopErrorScopeStatus_Success && error == WGPUErrorType_NoError))
    {
        return;
    }
    Diagnose(message);
    if (gEncoder != nullptr && gActiveSurface == slot)
    {
        // The open frame still owns this canvas texture; End submits it, then retires the slot.
        gSurfaces[slot].IsLossPending = true;
        return;
    }
    LoseCreatedSurface(slot);
}
/// Configure a created canvas inside a validation scope, so a configuration error loses only
/// that surface instead of reaching the device's uncaptured-error handler.
void ConfigureCreatedSurface(usize slot) noexcept
{
    WebSurface& surface = gSurfaces[slot];
    wgpuDevicePushErrorScope(gDevice, WGPUErrorFilter_Validation);
    wgpuSurfaceConfigure(surface.SurfaceOrNull, &surface.Configuration);
    surface.IsConfigured = true;
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = SurfaceConfigured;
    callback.userdata1 = Userdata(gSession);
    callback.userdata2 = Userdata(surface.Ticket);
    (void)wgpuDevicePopErrorScope(gDevice, callback);
}
/// Queue completion of every frame submitted before the query. A surface destroyed or replaced
/// since then carries another ticket, so its late completion is ignored.
void SurfacesRendered(WGPUQueueWorkDoneStatus status, WGPUStringView, void* session, void*) noexcept
{
    if (internal::Current(Token(session)) == false)
    {
        return;
    }
    gPendingCompletionQueries = 0;
    if (status != WGPUQueueWorkDoneStatus_Success)
    {
        return;
    }
    for (WebSurface& surface : gSurfaces)
    {
        if (surface.Ticket != 0 && surface.QueriedTicket == surface.Ticket &&
            surface.QueriedFrames > surface.RenderedFrames)
        {
            surface.RenderedFrames = surface.QueriedFrames;
        }
    }
}
/// Ask the queue once whether the frames submitted so far completed. Queries are issued only when
/// a caller reads surface counters, so frames add no completion callbacks of their own.
void RequestSurfaceCompletion() noexcept
{
    if (gPendingCompletionQueries != 0 || gQueue == nullptr)
    {
        return;
    }
    bool hasPendingFrames = false;
    for (WebSurface& surface : gSurfaces)
    {
        surface.QueriedFrames = surface.SubmittedFrames;
        surface.QueriedTicket = surface.Ticket;
        hasPendingFrames =
            hasPendingFrames || (surface.Ticket != 0 && surface.SubmittedFrames > surface.RenderedFrames);
    }
    if (hasPendingFrames == false)
    {
        return;
    }
    gPendingCompletionQueries = 1;
    WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = SurfacesRendered;
    callback.userdata1 = Userdata(gSession);
    (void)wgpuQueueOnSubmittedWorkDone(gQueue, callback);
}
void Configured(WGPUPopErrorScopeStatus status,
                WGPUErrorType error,
                WGPUStringView message,
                void* userdata,
                void*) noexcept
{
    const uint32 token = Token(userdata);
    if (internal::Current(token) == false)
    {
        return;
    }
    if (status != WGPUPopErrorScopeStatus_Success || error != WGPUErrorType_NoError)
    {
        Diagnose(message);
        internal::Fail(token, StartupError::SurfaceUnavailable);
        return;
    }
    internal::Complete(token,
                       StartupError::None,
                       { .MaxFrameDimension2D = gMaxDimension, .MaxUniformBufferSize = gMaxUniformSize });
}
void DeviceReady(WGPURequestDeviceStatus status,
                 WGPUDevice device,
                 WGPUStringView message,
                 void* userdata,
                 void*) noexcept
{
    const uint32 token = Token(userdata);
    if (internal::Current(token) == false)
    {
        if (device != nullptr)
        {
            wgpuDeviceDestroy(device);
            wgpuDeviceRelease(device);
        }
        return;
    }
    if (status != WGPURequestDeviceStatus_Success || device == nullptr)
    {
        if (device != nullptr)
        {
            wgpuDeviceDestroy(device);
            wgpuDeviceRelease(device);
        }
        Diagnose(message);
        internal::Fail(token, StartupError::DeviceUnavailable);
        return;
    }
    gDevice = device;
    gQueue = wgpuDeviceGetQueue(device);
    WGPULimits limits = WGPU_LIMITS_INIT;
    WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuDeviceGetLimits(device, &limits) != WGPUStatus_Success ||
        wgpuSurfaceGetCapabilities(gSurfaces[0].SurfaceOrNull, gAdapter, &capabilities) != WGPUStatus_Success ||
        capabilities.formatCount == 0 || gQueue == nullptr)
    {
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        internal::Fail(token, StartupError::SurfaceUnavailable);
        return;
    }
    if (limits.maxTextureDimension2D == 0)
    {
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        internal::Fail(token, StartupError::DeviceUnavailable);
        return;
    }
    gMaxDimension = limits.maxTextureDimension2D;
    gMaxUniformSize = limits.maxUniformBufferBindingSize < limits.maxBufferSize ? limits.maxUniformBufferBindingSize
                                                                                : limits.maxBufferSize;
    WGPUSurfaceConfiguration configuration = WGPU_SURFACE_CONFIGURATION_INIT;
    configuration.device = device;
    configuration.format = capabilities.formats[0];
    // The port orders its preferred browser format first (ADR 0009).
    // Validate startup with a minimal surface; frames reconfigure to actual pixels.
    configuration.width = 1;
    configuration.height = 1;
    configuration.presentMode = WGPUPresentMode_Fifo;
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    wgpuSurfaceConfigure(gSurfaces[0].SurfaceOrNull, &configuration);
    gSurfaces[0].IsConfigured = true;
    gSurfaces[0].Configuration = configuration;
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = Configured;
    callback.userdata1 = Userdata(token);
    (void)wgpuDevicePopErrorScope(device, callback);
}
void AdapterReady(WGPURequestAdapterStatus status,
                  WGPUAdapter adapter,
                  WGPUStringView message,
                  void* userdata,
                  void*) noexcept;
void RequestAdapter(uint32 token) noexcept
{
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.compatibleSurface = gSurfaces[0].SurfaceOrNull;
    options.featureLevel = gCompatibility ? WGPUFeatureLevel_Compatibility : WGPUFeatureLevel_Core;
    WGPURequestAdapterCallbackInfo callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = AdapterReady;
    callback.userdata1 = Userdata(token);
    (void)wgpuInstanceRequestAdapter(gInstance, &options, callback);
}
void AdapterReady(WGPURequestAdapterStatus status,
                  WGPUAdapter adapter,
                  WGPUStringView message,
                  void* userdata,
                  void*) noexcept
{
    const uint32 token = Token(userdata);
    if (internal::Current(token) == false)
    {
        if (adapter != nullptr)
        {
            wgpuAdapterRelease(adapter);
        }
        return;
    }
    if (status != WGPURequestAdapterStatus_Success || adapter == nullptr)
    {
        if (adapter != nullptr)
        {
            wgpuAdapterRelease(adapter);
        }
        if (gCompatibility == false)
        {
            gCompatibility = true;
            RequestAdapter(token);
            return;
        }
        Diagnose(message);
        internal::Fail(token, StartupError::AdapterUnavailable);
        return;
    }
    gAdapter = adapter;
    WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    // Thanks to W3C GPU for the Web Working Group, WebGPU, GPUDeviceDescriptor.requiredLimits:
    // https://www.w3.org/TR/webgpu/#dom-gpudevicedescriptor-requiredlimits
    // Adapter maxima must be requested before device creation; later queries use enabled device limits.
    WGPULimits adapterLimits = WGPU_LIMITS_INIT;
    if (wgpuAdapterGetLimits(adapter, &adapterLimits) != WGPUStatus_Success)
    {
        internal::Fail(token, StartupError::DeviceUnavailable);
        return;
    }
    WGPULimits requiredLimits = WGPU_LIMITS_INIT;
    requiredLimits.maxTextureDimension2D = adapterLimits.maxTextureDimension2D;
    requiredLimits.maxTextureDimension3D = adapterLimits.maxTextureDimension3D;
    requiredLimits.maxTextureArrayLayers = adapterLimits.maxTextureArrayLayers;
    requiredLimits.maxBufferSize = internal::GetResourceSizeLimit(adapterLimits.maxBufferSize);
    requiredLimits.maxStorageBufferBindingSize =
        adapterLimits.maxStorageBufferBindingSize < requiredLimits.maxBufferSize
            ? adapterLimits.maxStorageBufferBindingSize
            : requiredLimits.maxBufferSize;
    // The pinned port does not forward maxBindingsPerBindGroup in requests.
    // Sixteen slots fit the default envelope; BindingLimits queries the enabled device.
    requiredLimits.maxUniformBuffersPerShaderStage =
        adapterLimits.maxUniformBuffersPerShaderStage < RASTER_BINDING_COUNT
            ? adapterLimits.maxUniformBuffersPerShaderStage
            : static_cast<uint32>(RASTER_BINDING_COUNT);
    requiredLimits.maxStorageBuffersPerShaderStage =
        adapterLimits.maxStorageBuffersPerShaderStage < RASTER_BINDING_COUNT
            ? adapterLimits.maxStorageBuffersPerShaderStage
            : static_cast<uint32>(RASTER_BINDING_COUNT);
    requiredLimits.maxSampledTexturesPerShaderStage =
        adapterLimits.maxSampledTexturesPerShaderStage < RASTER_BINDING_COUNT
            ? adapterLimits.maxSampledTexturesPerShaderStage
            : static_cast<uint32>(RASTER_BINDING_COUNT);
    requiredLimits.maxSamplersPerShaderStage = adapterLimits.maxSamplersPerShaderStage < RASTER_BINDING_COUNT
                                                   ? adapterLimits.maxSamplersPerShaderStage
                                                   : static_cast<uint32>(RASTER_BINDING_COUNT);
    descriptor.requiredLimits = &requiredLimits;
    // Optional timestamps are requested only for an opted-in measurement session.
    constexpr usize OPTIONAL_FEATURE_COUNT = 3;
    StaticArray<WGPUFeatureName, OPTIONAL_FEATURE_COUNT> features{
        {WGPUFeatureName_TextureCompressionBC, WGPUFeatureName_TimestampQuery, WGPUFeatureName_IndirectFirstInstance},
    };
    if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_TextureCompressionBC))
    {
        features[descriptor.requiredFeatureCount++] = WGPUFeatureName_TextureCompressionBC;
    }
    // R12a: nonzero GPU-written firstInstance needs indirect-first-instance; without it the
    // profile reports HasFirstInstance false and kernels must write zero.
    if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_IndirectFirstInstance))
    {
        features[descriptor.requiredFeatureCount++] = WGPUFeatureName_IndirectFirstInstance;
    }
    const WGPUFeatureName timestamp = WGPUFeatureName_TimestampQuery;
    if (internal::GpuTimingRequested() && wgpuAdapterHasFeature(adapter, timestamp))
    {
        features[descriptor.requiredFeatureCount++] = timestamp;
    }
    descriptor.requiredFeatures = features.GetData();
    descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    descriptor.deviceLostCallbackInfo.userdata1 = Userdata(token);
    descriptor.deviceLostCallbackInfo.callback =
        [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView lostMessage, void* lostData, void*) noexcept {
            if (internal::Current(Token(lostData)))
            {
                Diagnose(lostMessage);
                internal::Fail(Token(lostData),
                               reason == WGPUDeviceLostReason_FailedCreation ? StartupError::DeviceUnavailable
                                                                             : StartupError::DeviceLost);
            }
        };
    descriptor.uncapturedErrorCallbackInfo.userdata1 = Userdata(token);
    descriptor.uncapturedErrorCallbackInfo.callback =
        [](WGPUDevice const*, WGPUErrorType, WGPUStringView errorMessage, void* errorData, void*) noexcept {
            if (internal::Current(Token(errorData)))
            {
                Diagnose(errorMessage);
                internal::Fail(Token(errorData), StartupError::Validation);
            }
        };
    WGPURequestDeviceCallbackInfo callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = DeviceReady;
    callback.userdata1 = Userdata(token);
    (void)wgpuAdapterRequestDevice(adapter, &descriptor, callback);
}
} // namespace
Backend Kind() noexcept
{
    return Backend::WebGPU;
}
bool Supports(BackendSelection selection) noexcept
{
    return selection == BackendSelection::Auto || selection == BackendSelection::WebGPU;
}
StartupError Start(const ApplicationInfo&, const WindowInfo& window, uint32 token, BackendSelection) noexcept
{
    if (window.System != ludus::platform::WindowSystem::WebCanvas || window.CanvasSelector == nullptr)
    {
        return StartupError::InvalidWindow;
    }
    const auto available = CanCreateSurface(window.CanvasSelector);
    if (available < 0)
    {
        return StartupError::AdapterUnavailable;
    }
    if (available == 0)
    {
        return StartupError::InvalidWindow;
    }
    gInstance = wgpuCreateInstance(nullptr);
    if (gInstance == nullptr)
    {
        return StartupError::InstanceUnavailable;
    }
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    source.selector = { .data = window.CanvasSelector, .length = WGPU_STRLEN };
    WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &source.chain;
    gSurfaces[0].SurfaceOrNull = wgpuInstanceCreateSurface(gInstance, &descriptor);
    if (gSurfaces[0].SurfaceOrNull == nullptr)
    {
        return StartupError::SurfaceUnavailable;
    }
    gSurfaces[0].SelectorOrNull = window.CanvasSelector;
    gSurfaces[0].IsConnected = true;
    gSurfaces[0].Ticket = TakeTicket();
    gActiveSurface = 0;
    gCompatibility = false;
    gSession = token;
    RequestAdapter(token);
    return StartupError::None;
}
void Shutdown() noexcept
{
    // The facade invalidates callback tokens before this can trigger device loss.
    ReleaseFrame();
    for (usize slot = 0; slot < gSurfaces.GetSize(); ++slot)
    {
        ReleaseSurface(slot);
        gSurfaces[slot] = {};
    }
    gActiveSurface = 0;
    gPendingCompletionQueries = 0;
    if (gQueue != nullptr)
    {
        wgpuQueueRelease(gQueue);
    }
    if (gDevice != nullptr)
    {
        wgpuDeviceDestroy(gDevice);
        wgpuDeviceRelease(gDevice);
    }
    if (gAdapter != nullptr)
    {
        wgpuAdapterRelease(gAdapter);
    }
    if (gInstance != nullptr)
    {
        wgpuInstanceRelease(gInstance);
    }
    gQueue = nullptr;
    gDevice = nullptr;
    gAdapter = nullptr;
    gInstance = nullptr;
    gMaxDimension = 0;
    gMaxUniformSize = 0;
    gSession = 0;
}
bool Initialize(const ApplicationInfo&) noexcept
{
    return false;
}
bool ConnectWindow(const WindowInfo&) noexcept
{
    return false;
}
bool InitializeRendering() noexcept
{
    return false;
}
void ShutdownRendering() noexcept {}
bool BeginFrame() noexcept
{
    return false;
}
bool EndFrame() noexcept
{
    return false;
}
usize AdditionalSurfaceLimit() noexcept
{
    return gDevice != nullptr && gQueue != nullptr ? MAX_DEVICE_SURFACES - 1 : 0;
}
StartupError CreateSurface(usize slot, const WindowInfo& window) noexcept
{
    if (gDevice == nullptr || slot == 0 || slot >= gSurfaces.GetSize() || gSurfaces[slot].IsConnected == true ||
        window.System != ludus::platform::WindowSystem::WebCanvas || window.CanvasSelector == nullptr ||
        window.Width > gMaxDimension || window.Height > gMaxDimension)
    {
        return StartupError::InvalidWindow;
    }
    for (const WebSurface& surface : gSurfaces)
    {
        if (surface.IsConnected == true && IsSameCanvas(surface.SelectorOrNull, window.CanvasSelector) == 1)
        {
            return StartupError::InvalidWindow;
        }
    }
    const int32 available = CanCreateSurface(window.CanvasSelector);
    if (available == 0)
    {
        return StartupError::InvalidWindow;
    }
    if (available < 0)
    {
        // The canvas already holds another context mode, such as WebGL 2.
        return StartupError::SurfaceUnavailable;
    }
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    source.selector = { .data = window.CanvasSelector, .length = WGPU_STRLEN };
    WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &source.chain;
    WGPUSurface createdOrNull = wgpuInstanceCreateSurface(gInstance, &descriptor);
    if (createdOrNull == nullptr)
    {
        return StartupError::SurfaceUnavailable;
    }
    // Pipelines target the primary format, so every canvas must accept it.
    WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
    const bool hasCapabilities =
        wgpuSurfaceGetCapabilities(createdOrNull, gAdapter, &capabilities) == WGPUStatus_Success;
    bool hasFormat = false;
    for (usize i = 0; hasCapabilities == true && i < capabilities.formatCount; ++i)
    {
        hasFormat = hasFormat || capabilities.formats[i] == GetSurfaceFormat();
    }
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    if (hasFormat == false)
    {
        wgpuSurfaceRelease(createdOrNull);
        return hasCapabilities == true ? StartupError::BackendUnavailable : StartupError::SurfaceUnavailable;
    }
    WebSurface& surface = gSurfaces[slot];
    surface = {};
    surface.SurfaceOrNull = createdOrNull;
    surface.SelectorOrNull = window.CanvasSelector;
    surface.Target = { .Width = window.Width, .Height = window.Height };
    surface.Configuration = gSurfaces[0].Configuration;
    // A zero extent leaves the canvas size alone; the first nonzero target reconfigures.
    surface.Configuration.width = window.Width;
    surface.Configuration.height = window.Height;
    surface.IsConnected = true;
    surface.Ticket = TakeTicket();
    ConfigureCreatedSurface(slot);
    // A synchronous validation result may already have retired the slot.
    return surface.IsConnected == true ? StartupError::None : StartupError::SurfaceUnavailable;
}
void DestroySurface(usize slot) noexcept
{
    if (slot == 0 || slot >= gSurfaces.GetSize() || gSurfaces[slot].IsConnected == false)
    {
        return;
    }
    ReleaseSurface(slot);
    if (gActiveSurface == slot)
    {
        gActiveSurface = 0;
    }
}
void SelectSurface(usize slot) noexcept
{
    gActiveSurface = slot < gSurfaces.GetSize() ? slot : 0;
}
backend::SurfaceCompletion GetSurfaceCompletion(usize slot) noexcept
{
    if (slot >= gSurfaces.GetSize())
    {
        return {};
    }
    RequestSurfaceCompletion();
    return { .RenderedFrames = gSurfaces[slot].RenderedFrames, .PresentedFrames = gSurfaces[slot].PresentedFrames };
}
namespace
{
bool ValidColor(float64 value) noexcept
{
    return value >= 0 && value <= 1;
}
/// Retire the selected created surface after its canvas stopped accepting frames; no frame opens.
FrameStatus LoseActiveSurface() noexcept
{
    LoseCreatedSurface(gActiveSurface);
    return FrameStatus::Skipped;
}
} // namespace
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    if (target.Width > gMaxDimension || target.Height > gMaxDimension || ValidColor(target.Red) == false ||
        ValidColor(target.Green) == false || ValidColor(target.Blue) == false || ValidColor(target.Alpha) == false)
    {
        return FrameStatus::InvalidState;
    }
    GetActiveSurface().Target = target;
    return FrameStatus::Ready;
}
FrameStatus Begin() noexcept
{
    WebSurface& surface = GetActiveSurface();
    const FrameTarget& target = surface.Target;
    const bool isCreated = gActiveSurface != 0;
    if (target.Width == 0 || target.Height == 0 || (isCreated == true && surface.IsConnected == false))
    {
        return FrameStatus::Skipped;
    }
    if (target.ShouldPresentToSurface == false)
    {
        // Thanks to W3C, WebGPU, "Canvas Contexts" and "GPUQueue.submit":
        // https://www.w3.org/TR/webgpu/#canvas-context
        // An offscreen submission needs an encoder, not a canvas texture. Avoid
        // getCurrentTexture/configure so candidate preparation cannot publish a
        // cleared canvas in place of the last presented image.
        gEncoder = wgpuDeviceCreateCommandEncoder(gDevice, nullptr);
        if (gEncoder == nullptr)
        {
            return FrameFailure();
        }
        RasterFrameExtent();
        TimingBegin();
        return FrameStatus::Ready;
    }
    if (isCreated == true && IsCanvasConnected(surface.SelectorOrNull) == 0)
    {
        return LoseActiveSurface();
    }
    if (surface.IsConfigured == false || surface.Configuration.width != target.Width ||
        surface.Configuration.height != target.Height)
    {
        surface.Configuration.width = target.Width;
        surface.Configuration.height = target.Height;
        if (isCreated == true)
        {
            ConfigureCreatedSurface(gActiveSurface);
            if (surface.IsConnected == false)
            {
                // A synchronous validation result retired this slot.
                return FrameStatus::Skipped;
            }
        }
        else
        {
            wgpuSurfaceConfigure(surface.SurfaceOrNull, &surface.Configuration);
            surface.IsConfigured = true;
        }
    }
    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface.SurfaceOrNull, &current);
    gTexture = current.texture;
    if (current.status == WGPUSurfaceGetCurrentTextureStatus_Timeout ||
        current.status == WGPUSurfaceGetCurrentTextureStatus_Outdated)
    {
        ReleaseFrame();
        // Reconfigure at the next nonzero frame; do not fabricate presentation.
        if (current.status != WGPUSurfaceGetCurrentTextureStatus_Timeout)
        {
            wgpuSurfaceUnconfigure(surface.SurfaceOrNull);
            surface.IsConfigured = false;
        }
        return FrameStatus::Skipped;
    }
    if ((current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
         current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) ||
        gTexture == nullptr)
    {
        if (isCreated == true)
        {
            // A created canvas whose context cannot supply a texture is lost alone.
            ReleaseFrame();
            return LoseActiveSurface();
        }
        return FrameFailure();
    }
    gView = wgpuTextureCreateView(gTexture, nullptr);
    gEncoder = wgpuDeviceCreateCommandEncoder(gDevice, nullptr);
    if (gView == nullptr || gEncoder == nullptr)
    {
        return FrameFailure();
    }
    RasterFrameExtent();
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = gView;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = { .r = target.Red, .g = target.Green, .b = target.Blue, .a = target.Alpha };
    WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &color;
    WGPUTextureDescriptor depthTexture = WGPU_TEXTURE_DESCRIPTOR_INIT;
    depthTexture.size = {target.Width, target.Height, 1};
    depthTexture.format = WGPUTextureFormat_Depth24Plus;
    depthTexture.usage = WGPUTextureUsage_RenderAttachment;
    gDepthTexture = wgpuDeviceCreateTexture(gDevice, &depthTexture);
    if (gDepthTexture == nullptr)
    {
        return FrameFailure();
    }
    gDepthView = wgpuTextureCreateView(gDepthTexture, nullptr);
    if (gDepthView == nullptr)
    {
        return FrameFailure();
    }
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = gDepthView;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Discard;
    depth.depthClearValue = 1;
    descriptor.depthStencilAttachment = &depth;
    TimingBegin();
    gPass = wgpuCommandEncoderBeginRenderPass(gEncoder, &descriptor);
    return gPass != nullptr ? FrameStatus::Ready : FrameFailure();
}
FrameStatus End() noexcept
{
    const usize slot = gActiveSurface;
    const bool isPresenting = gTexture != nullptr;
    if (gPass != nullptr)
    {
        wgpuRenderPassEncoderEnd(gPass);
    }
    TimingEnd();
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(gEncoder, nullptr);
    if (commands == nullptr)
    {
        return FrameFailure();
    }
    wgpuQueueSubmit(gQueue, 1, &commands);
    TimingSubmitted();
    wgpuCommandBufferRelease(commands);
    ReleaseFrame();
    WebSurface& surface = gSurfaces[slot];
    ++surface.SubmittedFrames;
    // WebGPU has no explicit present: the browser presents a canvas texture submitted in
    // this task when the task returns, so a submitted canvas frame counts as presented.
    if (isPresenting == true)
    {
        ++surface.PresentedFrames;
    }
    if (surface.IsLossPending == true)
    {
        LoseCreatedSurface(slot);
    }
    return FrameStatus::Ready;
}
WGPUDevice ProbeDevice() noexcept
{
    return gDevice;
}
WGPUTextureFormat ProbeFormat() noexcept
{
    return GetSurfaceFormat();
}
WGPURenderPassEncoder ProbePass() noexcept
{
    return gPass;
}

namespace
{
struct Shader final
{
    WGPUShaderModule Module = nullptr;
    char Entry[64]{};
};
struct Uniform final
{
    WGPUBuffer Buffer = nullptr;
    uint8 Bytes[internal::UNIFORM_CAPACITY]{};
    usize Size = 0;
};
struct Pipeline final
{
    WGPURenderPipeline Object = nullptr;
    WGPUBindGroupLayout Bindings = nullptr;
    WGPUPipelineLayout Layout = nullptr;
    WGPUBindGroup Group = nullptr;
    usize UniformSlot = 0;
};
Shader gShaders[internal::RESOURCE_CAPACITY];
Uniform gUniforms[internal::RESOURCE_CAPACITY];
Pipeline gPipelines[internal::RESOURCE_CAPACITY];
void ResourceChecked(WGPUPopErrorScopeStatus status,
                     WGPUErrorType error,
                     WGPUStringView message,
                     void* data,
                     void*) noexcept
{
    // The facade resolves a never-reused resource ID, not an application pointer.
    if (status != WGPUPopErrorScopeStatus_Success || error != WGPUErrorType_NoError)
    {
        Diagnose(message);
    }
    internal::ResourceComplete(Token(data),
                               status == WGPUPopErrorScopeStatus_Success && error == WGPUErrorType_NoError
                                   ? ResourceStatus::Ready
                                   : ResourceStatus::Failed);
}
void CheckResource(uint32 id) noexcept
{
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = ResourceChecked;
    callback.userdata1 = Userdata(id);
    (void)wgpuDevicePopErrorScope(gDevice, callback);
}
} // namespace
ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32 id) noexcept
{
    wgpuDevicePushErrorScope(gDevice, WGPUErrorFilter_Validation);
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = { .data = description.Wgsl.data(), .length = description.Wgsl.size() };
    WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &source.chain;
    auto& shader = gShaders[slot];
    std::memcpy(shader.Entry, description.WgslEntry.data(), description.WgslEntry.size());
    shader.Entry[description.WgslEntry.size()] = '\0';
    shader.Module = wgpuDeviceCreateShaderModule(gDevice, &descriptor);
    CheckResource(id);
    return shader.Module != nullptr ? ResourceStatus::Pending : ResourceStatus::Failed;
}
ResourceStatus CreateUniform(usize slot, const backend::UniformDescription& description, uint32 id) noexcept
{
    const usize size = description.Size;
    wgpuDevicePushErrorScope(gDevice, WGPUErrorFilter_Validation);
    auto& uniform = gUniforms[slot];
    uniform.Size = size;
    WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.size = size;
    descriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    uniform.Buffer = wgpuDeviceCreateBuffer(gDevice, &descriptor);
    CheckResource(id);
    return uniform.Buffer != nullptr ? ResourceStatus::Pending : ResourceStatus::Failed;
}
ResourceStatus CreatePipeline(usize slot, const backend::PipelineResources& resources, uint32 id) noexcept
{
    const usize vertex = resources.Vertex, fragment = resources.Fragment, uniform = resources.Uniform;
    wgpuDevicePushErrorScope(gDevice, WGPUErrorFilter_Validation);
    auto& pipeline = gPipelines[slot];
    pipeline.UniformSlot = uniform;
    WGPUBindGroupLayoutEntry binding = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    binding.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    binding.buffer.type = WGPUBufferBindingType_Uniform;
    binding.buffer.minBindingSize = gUniforms[uniform].Size;
    WGPUBindGroupLayoutDescriptor bindings = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bindings.entryCount = 1;
    bindings.entries = &binding;
    pipeline.Bindings = wgpuDeviceCreateBindGroupLayout(gDevice, &bindings);
    WGPUPipelineLayoutDescriptor layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout.bindGroupLayoutCount = 1;
    layout.bindGroupLayouts = &pipeline.Bindings;
    pipeline.Layout = wgpuDeviceCreatePipelineLayout(gDevice, &layout);
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.buffer = gUniforms[uniform].Buffer;
    entry.size = gUniforms[uniform].Size;
    WGPUBindGroupDescriptor group = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    group.layout = pipeline.Bindings;
    group.entryCount = 1;
    group.entries = &entry;
    pipeline.Group = wgpuDeviceCreateBindGroup(gDevice, &group);
    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = GetSurfaceFormat();
    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = gShaders[fragment].Module;
    fragmentState.entryPoint = { .data = gShaders[fragment].Entry, .length = WGPU_STRLEN };
    fragmentState.targetCount = 1;
    fragmentState.targets = &color;
    WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    descriptor.layout = pipeline.Layout;
    descriptor.vertex.module = gShaders[vertex].Module;
    descriptor.vertex.entryPoint = { .data = gShaders[vertex].Entry, .length = WGPU_STRLEN };
    descriptor.fragment = &fragmentState;
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = WGPUTextureFormat_Depth24Plus;
    depth.depthWriteEnabled = WGPUOptionalBool_False;
    depth.depthCompare = WGPUCompareFunction_Always;
    descriptor.depthStencil = &depth;
    pipeline.Object = wgpuDeviceCreateRenderPipeline(gDevice, &descriptor);
    CheckResource(id);
    return pipeline.Object != nullptr && pipeline.Group != nullptr && pipeline.Layout != nullptr &&
                   pipeline.Bindings != nullptr
               ? ResourceStatus::Pending
               : ResourceStatus::Failed;
}
void DestroyShader(usize slot) noexcept
{
    if (gShaders[slot].Module != nullptr)
    {
        wgpuShaderModuleRelease(gShaders[slot].Module);
    }
    gShaders[slot] = {};
}
void DestroyUniform(usize slot) noexcept
{
    if (gUniforms[slot].Buffer != nullptr)
    {
        // Release the host reference. Submitted command buffers retain GPU use;
        // explicit buffer.destroy would invalidate pending submissions.
        wgpuBufferRelease(gUniforms[slot].Buffer);
    }
    gUniforms[slot] = {};
}
void DestroyPipeline(usize slot) noexcept
{
    auto& pipeline = gPipelines[slot];
    if (pipeline.Object != nullptr)
    {
        wgpuRenderPipelineRelease(pipeline.Object);
    }
    if (pipeline.Group != nullptr)
    {
        wgpuBindGroupRelease(pipeline.Group);
    }
    if (pipeline.Layout != nullptr)
    {
        wgpuPipelineLayoutRelease(pipeline.Layout);
    }
    if (pipeline.Bindings != nullptr)
    {
        wgpuBindGroupLayoutRelease(pipeline.Bindings);
    }
    pipeline = {};
}
void UpdateUniform(usize slot, ludus::foundation::BorrowedView<const uint8> bytes) noexcept
{
    std::memcpy(gUniforms[slot].Bytes, bytes.GetDataOrNull(), bytes.GetSize());
}
FrameInfo GetFrameInfo() noexcept
{
    const WebSurface& surface = GetActiveSurface();
    const bool isPresenting = surface.Target.ShouldPresentToSurface == true;
    return
    {
        .Width = isPresenting == true ? surface.Configuration.width : surface.Target.Width,
        .Height = isPresenting == true ? surface.Configuration.height : surface.Target.Height,
        .Encoding = GetSurfaceFormat() == WGPUTextureFormat_BGRA8UnormSrgb ||
                            GetSurfaceFormat() == WGPUTextureFormat_RGBA8UnormSrgb
                        ? SurfaceEncoding::Srgb
                        : SurfaceEncoding::Unorm,
    };
}
ResourceStatus Draw(usize slot) noexcept
{
    const FrameTarget& target = GetActiveSurface().Target;
    if (target.ShouldPresentToSurface == false)
    {
        return ResourceStatus::InvalidState;
    }
    const auto& pipeline = gPipelines[slot];
    const auto& uniform = gUniforms[pipeline.UniformSlot];
    // Queue order: write N precedes submission N, which precedes write N+1.
    wgpuQueueWriteBuffer(gQueue, uniform.Buffer, 0, uniform.Bytes, uniform.Size);
    wgpuRenderPassEncoderSetViewport(gPass,
                                     0,
                                     0,
                                     static_cast<float32>(target.Width),
                                     static_cast<float32>(target.Height),
                                     0,
                                     1);
    wgpuRenderPassEncoderSetPipeline(gPass, pipeline.Object);
    wgpuRenderPassEncoderSetBindGroup(gPass, 0, pipeline.Group, 0, nullptr);
    wgpuRenderPassEncoderDraw(gPass, 3, 1, 0, 0);
    return ResourceStatus::Ready;
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE

#include "internal/raster_webgpu.h"

#include "internal/lifetime_webgpu.h"

#include "internal/measurement_webgpu.h"
