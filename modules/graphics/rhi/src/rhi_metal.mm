#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/gpu_scope_backend.h"
#include "internal/hosted_metal.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"

#include "internal/native_interop.h"
#include "internal/native_preparation.h"
#include "internal/raster.h"
#include "internal/ray_backend.h"
#include "internal/resources.h"
#include "internal/spatial_upscale_backend.h"
#include "internal/surface.h"
#include <climits>
#if defined(LUDUS_RHI_TEST_READBACK)
#    include "internal/readback.h"
#endif

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/platform/hosted_surface.h>
#include <ludus/platform/native_window.h>

#include <cstring>
#include <span>

// Apple's SDK 26 Metal umbrella (imported by MTLCommandQueue) references
// INFINITY without including its defining header. Keep the compatibility macro
// inside the SDK import boundary; RHI does not acquire a math-header dependency.
#if !defined(INFINITY)
#    define INFINITY (__builtin_inff())
#    define LUDUS_METAL_LOCAL_INFINITY 1
#endif

#import <AppKit/AppKit.h>
#import <Availability.h>
#import <Metal/MTLAccelerationStructure.h>
#import <Metal/MTLAccelerationStructureCommandEncoder.h>
#import <Metal/MTLBlitCommandEncoder.h>
#import <Metal/MTLBuffer.h>
#import <Metal/MTLCommandBuffer.h>
#import <Metal/MTLCommandQueue.h>
#import <Metal/MTLComputeCommandEncoder.h>
#import <Metal/MTLComputePass.h>
#import <Metal/MTLComputePipeline.h>
#import <Metal/MTLCounters.h>
#import <Metal/MTLDepthStencil.h>
#import <Metal/MTLDevice.h>
#import <Metal/MTLLibrary.h>
#import <Metal/MTLRenderPipeline.h>
#import <Metal/MTLSampler.h>
#import <Metal/MTLVertexDescriptor.h>
#import <QuartzCore/CAMetalLayer.h>
#if defined(LUDUS_METAL_LOCAL_INFINITY)
#    undef INFINITY
#    undef LUDUS_METAL_LOCAL_INFINITY
#endif

#if !defined(LUDUS_PLATFORM_MACOS)
#    error "The Metal backend requires macOS"
#endif

// Thanks to Apple, "Graphics Rendering: Render Command Encoder", Metal
// Programming Guide, for the drawable/command-buffer presentation sequence:
// https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Render-Ctx/Render-Ctx.html
// Two bounded frame slots fence uniform reuse; completion is inspected on the
// calling thread rather than mutating the facade from Metal callback threads.
namespace ludus::graphics::rhi::backend
{
void RasterFrameExtent() noexcept;
void TimingBegin() noexcept;
void TimingEnd() noexcept;
void BeginPassTimingFrame() noexcept;
void SubmitPassTimings() noexcept;
void ClosePassLabels() noexcept;
namespace
{
using namespace foundation;
constexpr logging::LogCategory LOG_RHI{"RHI"};
constexpr usize FRAMES = 2;
constexpr uint32 MAX_DIMENSION = 16384;
// Thanks to Apple, "MTLPipelineOption", Metal documentation, for the reflection
// flags: https://developer.apple.com/documentation/metal/mtlpipelineoption
// SDK 15+ provides BindingInfo; retain the legacy name for older SDKs.
// Both names request the same reflection bit.
#if defined(__MAC_15_0)
constexpr MTLPipelineOption BINDING_INFO = MTLPipelineOptionBindingInfo;
#else
constexpr MTLPipelineOption BINDING_INFO = MTLPipelineOptionArgumentInfo;
#endif
struct Shader final
{
    id<MTLFunction> Function = nil;
    usize UniformSize = 0;
};
struct Uniform final
{
    id<MTLBuffer> Buffers[FRAMES]{};
    uint8 Bytes[internal::UNIFORM_CAPACITY]{};
    usize Size = 0;
};
struct Pipeline final
{
    id<MTLRenderPipelineState> Object = nil;
    usize UniformSlot = 0;
};
id<MTLDevice> gDevice = nil;
id<MTLCommandQueue> gQueue = nil;
id<MTLCommandBuffer> gSubmitted[FRAMES]{};
constexpr usize PREFIXES_PER_NATIVE_COMPUTE = 3;
constexpr usize MAX_NATIVE_COMPUTE_PREFIXES = MAX_NATIVE_COMPUTE_PASSES_PER_FRAME * PREFIXES_PER_NATIVE_COMPUTE;
struct MetalCommandPrefixes final
{
    id<MTLCommandBuffer> CommandsOrNull[MAX_NATIVE_COMPUTE_PREFIXES]{};
    usize Count = 0;
};
MetalCommandPrefixes gCommandPrefixes[FRAMES]{};
bool hasNativeComputeSplit = false;
id<MTLCommandBuffer> gCommand = nil;
id<MTLRenderCommandEncoder> gEncoder = nil;
/// @brief Presentation record of one surface (R14b): its borrowed target, drawable layer or
/// hosted producer, offscreen and depth attachments, frame target and completion counters.
///
/// Slot 0 is the primary surface. Every record of a session shares gSurfaceFormat, so the
/// pipelines created for the primary surface draw to all of them.
struct MetalSurface final
{
    CAMetalLayer* LayerOrNull = nil;
    CALayer* PreviousLayerOrNull = nil;
    NSView* __unsafe_unretained ViewOrNull = nil;
    NSWindow* __unsafe_unretained WindowOrNull = nil;
    BOOL PreviousWantsLayer = NO;
    /// Producer of a hosted window's surface; connected only for WindowSystem::Hosted.
    HostedProducerMetal Hosted;
    /// Color attachment of a headless surface, resized with its target.
    id<MTLTexture> HeadlessOrNull = nil;
    id<MTLTexture> DepthOrNull = nil;
    FrameTarget Target{};
    /// This surface's committed frames not yet observed complete.
    StaticArray<id<MTLCommandBuffer>, FRAMES> PendingOrNull{};
    uint64 RenderedFrames = 0;
    uint64 PresentedFrames = 0;
    bool IsConnected = false;
};
StaticArray<MetalSurface, MAX_DEVICE_SURFACES> gSurfaces{};
/// Surface routed SetTarget/Begin/End; see backend::SelectSurface.
usize gActiveSurface = 0;
/// Depth attachment of the open frame's surface.
id<MTLTexture> gDepth = nil;
id<CAMetalDrawable> gDrawable = nil;
/// Color format of every frame target: BGRA8 drawables, or RGBA8 hosted images.
MTLPixelFormat gSurfaceFormat = MTLPixelFormatBGRA8Unorm;
/// Color attachment of the open frame: drawable, hosted image or headless texture.
id<MTLTexture> gSurfaceTexture = nil;
Shader gShaders[internal::RESOURCE_CAPACITY];
Uniform gUniforms[internal::RESOURCE_CAPACITY];
Pipeline gPipelines[internal::RESOURCE_CAPACITY];
FrameInfo gFrame{};
usize gSlot = 0;
uint32 gToken = 0;
bool gConnected = false;
bool gRendering = false;

// A compiler service failure reports only "Compilation failed", unlike a source error, whose description carries
// the program_source log. The domain, code and underlying error tell the two apart in CI logs.
void LogError(NSError* errorOrNull) noexcept
{
    if (errorOrNull == nil)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Metal: operation failed without an error");
        return;
    }
    const char* messageOrNull = errorOrNull.localizedDescription.UTF8String;
    const char* domainOrNull = errorOrNull.domain.UTF8String;
    LUDUS_LOG_ERROR(LOG_RHI,
                    "Metal: {} [{} {}]",
                    messageOrNull != nullptr ? messageOrNull : "operation failed",
                    domainOrNull != nullptr ? domainOrNull : "unknown domain",
                    static_cast<int64>(errorOrNull.code));
    NSError* underlyingOrNull = errorOrNull.userInfo[NSUnderlyingErrorKey];
    const char* underlyingMessageOrNull = underlyingOrNull != nil ? underlyingOrNull.description.UTF8String : nullptr;
    if (underlyingMessageOrNull != nullptr)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Metal: caused by {}", underlyingMessageOrNull);
    }
}
bool CheckCommand(id<MTLCommandBuffer> command) noexcept
{
    if (command.status != MTLCommandBufferStatusError)
    {
        return true;
    }
    LogError(command.error);
    const StartupError error = command.error.code == MTLCommandBufferErrorDeviceRemoved
                                   ? StartupError::DeviceLost
                                   : StartupError::RenderingUnavailable;
    if (internal::Current(gToken))
    {
        internal::Fail(gToken, error);
    }
    else
    {
        ShutdownRendering();
    }
    return false;
}
bool ValidChannel(float64 value) noexcept
{
    // Ordered comparisons reject NaN as well as values outside the clear range.
    return value >= 0 && value <= 1;
}
bool ValidWindow(const WindowInfo& window) noexcept
{
    if (window.System == platform::WindowSystem::Headless)
    {
        return true;
    }
    if (window.System == platform::WindowSystem::Hosted)
    {
        return window.HostedSurfaceOrNull != nullptr;
    }
    if (window.System != platform::WindowSystem::Cocoa || window.CocoaWindow == nullptr || window.CocoaView == nullptr)
    {
        return false;
    }
    NSWindow* nativeWindow = (__bridge NSWindow*)window.CocoaWindow;
    NSView* view = (__bridge NSView*)window.CocoaView;
    return view.window == nativeWindow && nativeWindow.contentView == view;
}
bool BindingsFit(NSArray<id<MTLBinding>>* bindings, usize declaredSize, usize bufferSize) noexcept
{
    for (id<MTLBinding> binding in bindings)
    {
        if (binding.used == false)
        {
            continue;
        }
        if (binding.type != MTLBindingTypeBuffer || binding.index != 0 || binding.access != MTLBindingAccessReadOnly)
        {
            return false;
        }
        id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)binding;
        if (buffer.bufferDataSize > declaredSize || buffer.bufferDataSize > bufferSize)
        {
            return false;
        }
    }
    return true;
}
FrameStatus FrameFailure() noexcept
{
    if (internal::Current(gToken))
    {
        internal::Fail(gToken, StartupError::RenderingUnavailable);
    }
    else
    {
        ShutdownRendering();
    }
    return FrameStatus::Failed;
}
MetalSurface& GetActiveSurface() noexcept
{
    return gSurfaces[gActiveSurface];
}
MTLPixelFormat GetSurfaceFormat(const WindowInfo& window) noexcept
{
    // The editor presents hosted images as RGBA8 (docs/architecture/editor-gui-systems.md).
    return window.System == platform::WindowSystem::Hosted ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatBGRA8Unorm;
}
/// Count this surface's completed frames without waiting.
void RefreshSurface(MetalSurface& inoutSurface) noexcept
{
    for (id<MTLCommandBuffer> __strong& commandOrNull : inoutSurface.PendingOrNull)
    {
        if (commandOrNull == nil)
        {
            continue;
        }
        const MTLCommandBufferStatus status = commandOrNull.status;
        if (status == MTLCommandBufferStatusCompleted)
        {
            ++inoutSurface.RenderedFrames;
            commandOrNull = nil;
        }
        else if (status == MTLCommandBufferStatusError)
        {
            // Session failure is reported by CheckCommand; an errored frame never rendered.
            commandOrNull = nil;
        }
    }
}
/// Record a committed frame of this surface for render-completion reporting.
void TrackSurfaceFrame(MetalSurface& inoutSurface, id<MTLCommandBuffer> command) noexcept
{
    RefreshSurface(inoutSurface);
    for (id<MTLCommandBuffer> __strong& pendingOrNull : inoutSurface.PendingOrNull)
    {
        if (pendingOrNull == nil)
        {
            pendingOrNull = command;
            return;
        }
    }
    // Device frame slots bound the frames in flight; wait for the oldest only if every entry is still pending.
    [inoutSurface.PendingOrNull[0] waitUntilCompleted];
    RefreshSurface(inoutSurface);
    inoutSurface.PendingOrNull[0] = command;
}
/// Borrow a validated target into an empty record; the record keeps the window's extent as its target.
bool ConnectSurface(MetalSurface& outSurface, const WindowInfo& window) noexcept
{
    if (window.System == platform::WindowSystem::Cocoa)
    {
        CAMetalLayer* layerOrNull = [CAMetalLayer layer];
        if (layerOrNull == nil)
        {
            return false;
        }
        outSurface.WindowOrNull = (__bridge NSWindow*)window.CocoaWindow;
        outSurface.ViewOrNull = (__bridge NSView*)window.CocoaView;
        outSurface.LayerOrNull = layerOrNull;
        layerOrNull.device = gDevice;
        layerOrNull.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layerOrNull.framebufferOnly = YES;
        layerOrNull.maximumDrawableCount = FRAMES;
        layerOrNull.allowsNextDrawableTimeout = YES;
        outSurface.PreviousWantsLayer = outSurface.ViewOrNull.wantsLayer;
        outSurface.PreviousLayerOrNull = outSurface.ViewOrNull.layer;
        outSurface.ViewOrNull.wantsLayer = YES;
        outSurface.ViewOrNull.layer = layerOrNull;
    }
    if (window.System == platform::WindowSystem::Hosted)
    {
        outSurface.Hosted.Connect(gDevice, *window.HostedSurfaceOrNull);
    }
    outSurface.Target = { .Width = window.Width, .Height = window.Height };
    outSurface.RenderedFrames = 0;
    outSurface.PresentedFrames = 0;
    outSurface.IsConnected = true;
    return true;
}
/// @brief Retire one surface's presentation resources and restore its borrowed view.
///
/// Waits for this surface's committed frames first. Completion counters are kept, so a
/// lost surface still reports what it rendered; ConnectSurface clears them for a new surface.
void DisconnectSurface(MetalSurface& inoutSurface) noexcept
{
    for (id<MTLCommandBuffer> commandOrNull : inoutSurface.PendingOrNull)
    {
        [commandOrNull waitUntilCompleted];
    }
    RefreshSurface(inoutSurface);
    inoutSurface.Hosted.Disconnect();
    if (inoutSurface.ViewOrNull != nil && inoutSurface.ViewOrNull.layer == inoutSurface.LayerOrNull)
    {
        inoutSurface.ViewOrNull.layer = inoutSurface.PreviousLayerOrNull;
        inoutSurface.ViewOrNull.wantsLayer = inoutSurface.PreviousWantsLayer;
    }
    inoutSurface.ViewOrNull = nil;
    inoutSurface.WindowOrNull = nil;
    inoutSurface.LayerOrNull = nil;
    inoutSurface.PreviousLayerOrNull = nil;
    inoutSurface.PreviousWantsLayer = NO;
    inoutSurface.HeadlessOrNull = nil;
    inoutSurface.DepthOrNull = nil;
    inoutSurface.Target = {};
    inoutSurface.IsConnected = false;
}
/// Disconnect every surface, the primary included, and route frames back to the primary.
void DisconnectSurfaces() noexcept
{
    for (MetalSurface& surface : gSurfaces)
    {
        DisconnectSurface(surface);
    }
    gActiveSurface = 0;
    gSurfaceFormat = MTLPixelFormatBGRA8Unorm;
}
/// @brief A created surface stopped accepting frames (its view left its window or its hosted
/// consumer failed). Retire only its presentation resources and report it lost; the device
/// and other surfaces keep working. Opens no frame.
FrameStatus LoseActiveSurface() noexcept
{
    DisconnectSurface(GetActiveSurface());
    internal::LoseSurface(gActiveSurface);
    return FrameStatus::Skipped;
}
/// Announce completed hosted frames of every surface; a failed created surface is lost.
/// @return false when the primary surface's hosted consumer failed.
bool PollHostedSurfaces() noexcept
{
    for (usize slot = 0; slot < gSurfaces.GetSize(); ++slot)
    {
        MetalSurface& surface = gSurfaces[slot];
        if (surface.Hosted.IsConnected() == false || surface.Hosted.Poll() == true)
        {
            continue;
        }
        if (slot == 0)
        {
            return false;
        }
        DisconnectSurface(surface);
        internal::LoseSurface(slot);
    }
    return true;
}
// R14a adapter catalog. Thanks to Apple, "MTLCopyAllDevices()" and "registryID",
// Metal documentation, for enumerating adapters and their stable identities:
// https://developer.apple.com/documentation/metal/mtlcopyalldevices()
// https://developer.apple.com/documentation/metal/mtldevice/registryid
// The raw edge claims follow Apple's "Metal Feature Set Tables": Mac2 and Apple3
// and later support 16384-texel 2D textures, earlier Apple families 8192.
constexpr uint32 EARLY_APPLE_TEXTURE_DIMENSION = 8192;
bool MeetsMetalBaseline(id<MTLDevice> device) noexcept
{
    return [device supportsFamily:MTLGPUFamilyMac2] == true || [device supportsFamily:MTLGPUFamilyApple4] == true;
}
AdapterIdentity GetMetalIdentity(id<MTLDevice> device) noexcept
{
    AdapterIdentity identity{ .Api = Backend::Metal };
    const bool isWritten = TryWriteLittleEndian<uint64>(device.registryID, {identity.Bytes.GetData(), sizeof(uint64)});
    LUDUS_REQUIRE(isWritten == true);
    return identity;
}
void DescribeMetalAdapter(id<MTLDevice> device, AdapterInfo& outAdapter) noexcept
{
    outAdapter = {};
    outAdapter.Identity = GetMetalIdentity(device);
    const char* nameOrNull = device.name.UTF8String;
    if (nameOrNull != nullptr)
    {
        internal::AssignAdapterName(nameOrNull, outAdapter.Name);
    }
    outAdapter.Kind =
        device.hasUnifiedMemory == YES || device.isLowPower == YES ? AdapterKind::Integrated : AdapterKind::Discrete;
    outAdapter.LocalMemoryBytes = device.recommendedMaxWorkingSetSize;
    outAdapter.MaxTextureDimension2D =
        [device supportsFamily:MTLGPUFamilyMac2] == true || [device supportsFamily:MTLGPUFamilyApple3] == true
            ? MAX_DIMENSION
            : EARLY_APPLE_TEXTURE_DIMENSION;
    outAdapter.HasBackendBaseline = MeetsMetalBaseline(device);
}
/// The requested catalog adapter, or nil when no system device has its identity.
id<MTLDevice> FindMetalDeviceOrNil(const AdapterIdentity& adapter) noexcept
{
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    for (id<MTLDevice> device in devices)
    {
        if (GetMetalIdentity(device) == adapter)
        {
            return device;
        }
    }
    return nil;
}
} // namespace
Backend Kind() noexcept
{
    return Backend::Metal;
}
AdapterStatus EnumerateAdapters(AdapterCatalog& outCatalog) noexcept
{
    @autoreleasepool
    {
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        if (devices == nil)
        {
            return AdapterStatus::Failed;
        }
        outCatalog = { .Api = Backend::Metal };
        for (id<MTLDevice> device in devices)
        {
            AdapterInfo* entryOrNull = internal::ReserveCatalogEntryOrNull(outCatalog);
            if (entryOrNull != nullptr)
            {
                DescribeMetalAdapter(device, *entryOrNull);
            }
        }
        return AdapterStatus::Ready;
    }
}
bool GetSelectedAdapter(AdapterInfo& outAdapter) noexcept
{
    @autoreleasepool
    {
        if (gDevice == nil)
        {
            return false;
        }
        DescribeMetalAdapter(gDevice, outAdapter);
        return true;
    }
}
bool Supports(BackendSelection selection) noexcept
{
    return selection == BackendSelection::Auto;
}
StartupError Start(const ApplicationInfo& app, const WindowInfo& window, uint32 token, BackendSelection) noexcept
{
    @autoreleasepool
    {
        if ([NSThread isMainThread] == false || ValidWindow(window) == false)
        {
            return StartupError::InvalidWindow;
        }
        gToken = token;
        if (backend::Initialize(app) == false)
        {
            return gDevice == nil ? StartupError::AdapterUnavailable : StartupError::DeviceUnavailable;
        }
        if (ConnectWindow(window) == false)
        {
            return StartupError::SurfaceUnavailable;
        }
        if (InitializeRendering() == false)
        {
            return StartupError::RenderingUnavailable;
        }
        internal::Complete(token, StartupError::None, {MAX_DIMENSION, gDevice.maxBufferLength});
        return StartupError::None;
    }
}
bool Initialize(const ApplicationInfo&) noexcept
{
    @autoreleasepool
    {
        if ([NSThread isMainThread] == false)
        {
            return false;
        }
        if (gDevice != nil)
        {
            return gQueue != nil;
        }
        const auto* imported = internal::NativeDeviceRequest();
        if (imported != nullptr)
        {
            gDevice = (__bridge id<MTLDevice>)imported->Identity.Device.GetObject();
            gQueue = (__bridge id<MTLCommandQueue>)imported->Identity.CommandQueue.GetObject();
            if (gQueue.device != gDevice)
            {
                gQueue = nil;
                gDevice = nil;
                return false;
            }
        }
        else
        {
            const AdapterIdentity* requestedOrNull = internal::GetRequestedAdapterOrNull();
            gDevice =
                requestedOrNull != nullptr ? FindMetalDeviceOrNil(*requestedOrNull) : MTLCreateSystemDefaultDevice();
        }
        // Thanks to Apple's "Metal Feature Set Tables" (resource limits),
        // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf:
        // Mac2 / Apple4 and later guarantee the conservative 16384 texture limit.
        if (gDevice == nil || MeetsMetalBaseline(gDevice) == false)
        {
            gDevice = nil;
            return false;
        }
        if (gQueue == nil)
        {
            gQueue = [gDevice newCommandQueue];
        }
        return gQueue != nil;
    }
}
bool ConnectWindow(const WindowInfo& window) noexcept
{
    @autoreleasepool
    {
        if ([NSThread isMainThread] == false || gDevice == nil || gConnected || ValidWindow(window) == false ||
            window.Width > MAX_DIMENSION || window.Height > MAX_DIMENSION)
        {
            return false;
        }
        if (ConnectSurface(gSurfaces[0], window) == false)
        {
            return false;
        }
        gSurfaceFormat = GetSurfaceFormat(window);
        gActiveSurface = 0;
        gConnected = true;
        return true;
    }
}
bool InitializeRendering() noexcept
{
    gRendering = gQueue != nil && gConnected;
    return gRendering;
}
void ShutdownRendering() noexcept
{
    @autoreleasepool
    {
        ClosePassLabels();
        [gEncoder endEncoding];
        gEncoder = nil;
        gCommand = nil;
        gDrawable = nil;
        for (auto& command : gSubmitted)
        {
            [command waitUntilCompleted];
            command = nil;
        }
        for (MetalSurface& surface : gSurfaces)
        {
            surface.Hosted.Cancel();
            RefreshSurface(surface);
            surface.HeadlessOrNull = nil;
            surface.DepthOrNull = nil;
        }
        gSurfaceTexture = nil;
        gDepth = nil;
        gFrame = {};
        gSlot = 0;
        gRendering = false;
    }
}
void Shutdown() noexcept
{
    @autoreleasepool
    {
        ShutdownRendering();
        DisconnectSurfaces();
        gQueue = nil;
        gDevice = nil;
        gConnected = false;
        gToken = 0;
    }
}
usize AdditionalSurfaceLimit() noexcept
{
    return gRendering == true ? MAX_DEVICE_SURFACES - 1 : 0;
}
StartupError CreateSurface(usize slot, const WindowInfo& window) noexcept
{
    @autoreleasepool
    {
        if ([NSThread isMainThread] == false || gRendering == false || slot == 0 || slot >= gSurfaces.GetSize() ||
            gSurfaces[slot].IsConnected == true || ValidWindow(window) == false || window.Width > MAX_DIMENSION ||
            window.Height > MAX_DIMENSION)
        {
            return StartupError::InvalidWindow;
        }
        // Pipelines target gSurfaceFormat, so every surface of a session shares it.
        if (GetSurfaceFormat(window) != gSurfaceFormat)
        {
            return StartupError::BackendUnavailable;
        }
        return ConnectSurface(gSurfaces[slot], window) == true ? StartupError::None : StartupError::SurfaceUnavailable;
    }
}
void DestroySurface(usize slot) noexcept
{
    @autoreleasepool
    {
        if (slot != 0 && slot < gSurfaces.GetSize())
        {
            DisconnectSurface(gSurfaces[slot]);
            gActiveSurface = gActiveSurface == slot ? 0 : gActiveSurface;
        }
    }
}
void SelectSurface(usize slot) noexcept
{
    gActiveSurface = slot < gSurfaces.GetSize() ? slot : 0;
}
SurfaceCompletion GetSurfaceCompletion(usize slot) noexcept
{
    if (slot >= gSurfaces.GetSize())
    {
        return {};
    }
    RefreshSurface(gSurfaces[slot]);
    return { .RenderedFrames = gSurfaces[slot].RenderedFrames, .PresentedFrames = gSurfaces[slot].PresentedFrames };
}
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    if (target.ShouldPresentToSurface == false)
    {
        return FrameStatus::Unsupported;
    }
    if (gEncoder != nil)
    {
        return FrameStatus::InvalidState;
    }
    if (target.Width > MAX_DIMENSION || target.Height > MAX_DIMENSION || ValidChannel(target.Red) == false ||
        ValidChannel(target.Green) == false || ValidChannel(target.Blue) == false ||
        ValidChannel(target.Alpha) == false)
    {
        return FrameStatus::Failed;
    }
    GetActiveSurface().Target = target;
    return FrameStatus::Ready;
}
FrameStatus Begin() noexcept
{
    @autoreleasepool
    {
        if (gRendering == false)
        {
            return FrameStatus::NotReady;
        }
        if (gEncoder != nil)
        {
            return FrameStatus::InvalidState;
        }
        gFrame = {};
        for (auto command : gSubmitted)
        {
            if (command != nil && CheckCommand(command) == false)
            {
                return FrameStatus::Failed;
            }
        }
        if (PollHostedSurfaces() == false)
        {
            return FrameFailure();
        }
        MetalSurface& surface = GetActiveSurface();
        if (gActiveSurface != 0 && surface.IsConnected == false)
        {
            // Lost earlier, for example while another surface polled its hosted consumer.
            return FrameStatus::Skipped;
        }
        // A created Cocoa surface is lost once its view leaves the window it was borrowed from.
        if (gActiveSurface != 0 && surface.ViewOrNull != nil &&
            (surface.ViewOrNull.window != surface.WindowOrNull ||
             surface.WindowOrNull.contentView != surface.ViewOrNull))
        {
            return LoseActiveSurface();
        }
        const FrameTarget& target = surface.Target;
        if (target.Width == 0 || target.Height == 0 ||
            (surface.WindowOrNull != nil &&
             (surface.WindowOrNull.miniaturized || surface.WindowOrNull.visible == false)))
        {
            return FrameStatus::Skipped;
        }
        [gSubmitted[gSlot] waitUntilCompleted];
        (void)RasterCompleted();
        if (gSubmitted[gSlot] != nil && CheckCommand(gSubmitted[gSlot]) == false)
        {
            return FrameStatus::Failed;
        }
        gSubmitted[gSlot] = nil;
        gCommandPrefixes[gSlot] = {};
        hasNativeComputeSplit = false;
        id<MTLTexture> texture = nil;
        if (surface.Hosted.IsConnected() == true)
        {
            const HostedAcquireStatus acquired = surface.Hosted.Acquire(target.Width, target.Height, texture);
            if (acquired == HostedAcquireStatus::Skipped)
            {
                return FrameStatus::Skipped;
            }
            if (acquired == HostedAcquireStatus::Failed)
            {
                return gActiveSurface != 0 ? LoseActiveSurface() : FrameFailure();
            }
        }
        else if (surface.LayerOrNull != nil)
        {
            surface.LayerOrNull.frame = surface.ViewOrNull.bounds;
            surface.LayerOrNull.contentsScale = surface.WindowOrNull.backingScaleFactor;
            surface.LayerOrNull.drawableSize = CGSizeMake(target.Width, target.Height);
            gDrawable = [surface.LayerOrNull nextDrawable];
            if (gDrawable == nil)
            {
                return FrameStatus::Skipped;
            }
            texture = gDrawable.texture;
        }
        else
        {
            if (surface.HeadlessOrNull == nil || surface.HeadlessOrNull.width != target.Width ||
                surface.HeadlessOrNull.height != target.Height)
            {
                MTLTextureDescriptor* description =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                       width:target.Width
                                                                      height:target.Height
                                                                   mipmapped:NO];
                description.usage = MTLTextureUsageRenderTarget;
                description.storageMode = MTLStorageModePrivate;
                surface.HeadlessOrNull = [gDevice newTextureWithDescriptor:description];
            }
            texture = surface.HeadlessOrNull;
        }
        if (texture == nil)
        {
            return FrameFailure();
        }
        if (surface.DepthOrNull == nil || surface.DepthOrNull.width != texture.width ||
            surface.DepthOrNull.height != texture.height)
        {
            MTLTextureDescriptor* depth =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                   width:texture.width
                                                                  height:texture.height
                                                               mipmapped:NO];
            depth.usage = MTLTextureUsageRenderTarget;
            depth.storageMode = MTLStorageModePrivate;
            surface.DepthOrNull = [gDevice newTextureWithDescriptor:depth];
        }
        if (surface.DepthOrNull == nil)
        {
            surface.Hosted.Cancel();
            return FrameFailure();
        }
        gDepth = surface.DepthOrNull;
        gCommand = [gQueue commandBuffer];
        gCommand.label = @"Ludus/Frame";
        BeginPassTimingFrame();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = gDepth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 1;
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(target.Red, target.Green, target.Blue, target.Alpha);
        gEncoder = [gCommand renderCommandEncoderWithDescriptor:pass];
        if (gEncoder == nil)
        {
            surface.Hosted.Cancel();
            return FrameFailure();
        }
        gSurfaceTexture = texture;
        RasterFrameExtent();
        // Extent comes from the acquired texture, bounded by the target above.
        gFrame = {static_cast<uint32>(texture.width), static_cast<uint32>(texture.height), SurfaceEncoding::Unorm};
        return FrameStatus::Ready;
    }
}
FrameStatus End() noexcept
{
    @autoreleasepool
    {
        if (gCommand == nil)
        {
            return FrameStatus::InvalidState;
        }
        if (gEncoder != nil)
        {
            ClosePassLabels();
            [gEncoder endEncoding];
        }
        gEncoder = nil;
        MetalSurface& surface = GetActiveSurface();
        bool isPresented = false;
        if (gDrawable != nil)
        {
            [gCommand presentDrawable:gDrawable];
            isPresented = true;
        }
        TimingEnd();
        SubmitPassTimings();
        [gCommand commit];
        if (surface.Hosted.IsConnected() == true)
        {
            // Announced with Present once the command buffer completes (CPU fence).
            surface.Hosted.Submit(gCommand);
            static_cast<void>(surface.Hosted.Poll());
            isPresented = true;
        }
        TrackSurfaceFrame(surface, gCommand);
        surface.PresentedFrames += isPresented == true ? 1 : 0;
        gSurfaceTexture = nil;
        gDepth = nil;
        gSubmitted[gSlot] = gCommand;
        gCommand = nil;
        gDrawable = nil;
        gFrame = {};
        gSlot = (gSlot + 1) % FRAMES;
        return FrameStatus::Ready;
    }
}
bool BeginFrame() noexcept
{
    return Begin() == FrameStatus::Ready;
}
bool EndFrame() noexcept
{
    return End() == FrameStatus::Ready;
}
ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32) noexcept
{
    @autoreleasepool
    {
        NSString* source = [[NSString alloc] initWithBytes:description.Msl.data()
                                                    length:description.Msl.size()
                                                  encoding:NSUTF8StringEncoding];
        NSString* entry = [[NSString alloc] initWithBytes:description.MslEntry.data()
                                                   length:description.MslEntry.size()
                                                 encoding:NSUTF8StringEncoding];
        if (source == nil || entry == nil)
        {
            return ResourceStatus::Failed;
        }
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_3;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:source options:options error:&error];
        if (library == nil)
        {
            LogError(error);
            return ResourceStatus::Failed;
        }
        id<MTLFunction> function = [library newFunctionWithName:entry];
        const MTLFunctionType type =
            description.Stage == ShaderStage::Vertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
        if (function == nil || function.functionType != type)
        {
            return ResourceStatus::Failed;
        }
        gShaders[slot] = {function, description.UniformSize};
        return ResourceStatus::Ready;
    }
}
ResourceStatus CreateUniform(usize slot, const UniformDescription& description, uint32) noexcept
{
    @autoreleasepool
    {
        auto& uniform = gUniforms[slot];
        uniform.Size = description.Size;
        for (auto& buffer : uniform.Buffers)
        {
            buffer = [gDevice newBufferWithLength:uniform.Size options:MTLResourceStorageModeShared];
            if (buffer == nil || buffer.contents == nullptr)
            {
                return ResourceStatus::Failed;
            }
        }
        return ResourceStatus::Ready;
    }
}
ResourceStatus CreatePipeline(usize slot, const PipelineResources& resources, uint32) noexcept
{
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor* description = [MTLRenderPipelineDescriptor new];
        description.vertexFunction = gShaders[resources.Vertex].Function;
        description.fragmentFunction = gShaders[resources.Fragment].Function;
        description.colorAttachments[0].pixelFormat = gSurfaceFormat;
        description.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError* error = nil;
        MTLRenderPipelineReflection* reflection = nil;
        id<MTLRenderPipelineState> pipeline =
            [gDevice newRenderPipelineStateWithDescriptor:description
                                                  options:BINDING_INFO | MTLPipelineOptionBufferTypeInfo
                                               reflection:&reflection
                                                    error:&error];
        if (pipeline == nil)
        {
            LogError(error);
            return ResourceStatus::Failed;
        }
        const usize size = gUniforms[resources.Uniform].Size;
        if (reflection == nil ||
            BindingsFit(reflection.vertexBindings, gShaders[resources.Vertex].UniformSize, size) == false ||
            BindingsFit(reflection.fragmentBindings, gShaders[resources.Fragment].UniformSize, size) == false)
        {
            return ResourceStatus::Failed;
        }
        gPipelines[slot] = {pipeline, resources.Uniform};
        return ResourceStatus::Ready;
    }
}
void DestroyShader(usize slot) noexcept
{
    gShaders[slot] = {};
}
void DestroyUniform(usize slot) noexcept
{
    gUniforms[slot] = {};
}
void DestroyPipeline(usize slot) noexcept
{
    gPipelines[slot] = {};
}
void UpdateUniform(usize slot, ludus::foundation::BorrowedView<const uint8> bytes) noexcept
{
    std::memcpy(gUniforms[slot].Bytes, bytes.GetDataOrNull(), bytes.GetSize());
}
FrameInfo GetFrameInfo() noexcept
{
    return gFrame;
}
ResourceStatus Draw(usize slot) noexcept
{
    auto& pipeline = gPipelines[slot];
    auto& uniform = gUniforms[pipeline.UniformSlot];
    id<MTLBuffer> buffer = uniform.Buffers[gSlot];
    std::memcpy(buffer.contents, uniform.Bytes, uniform.Size);
    [gEncoder setRenderPipelineState:pipeline.Object];
    [gEncoder setVertexBuffer:buffer offset:0 atIndex:0];
    [gEncoder setFragmentBuffer:buffer offset:0 atIndex:0];
    [gEncoder
        setViewport:MTLViewport{0, 0, static_cast<float64>(gFrame.Width), static_cast<float64>(gFrame.Height), 0, 1}];
    [gEncoder setCullMode:MTLCullModeNone];
    [gEncoder setTriangleFillMode:MTLTriangleFillModeFill];
    [gEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    return ResourceStatus::Ready;
}
#if defined(LUDUS_RHI_TEST_READBACK)
bool ReadHeadlessPixels(std::span<uint8> pixels) noexcept
{
    @autoreleasepool
    {
        id<MTLTexture> headlessOrNull = GetActiveSurface().HeadlessOrNull;
        if (gRendering == false || GetActiveSurface().LayerOrNull != nil || gEncoder != nil || headlessOrNull == nil)
        {
            return false;
        }
        const usize width = headlessOrNull.width;
        const usize height = headlessOrNull.height;
        if (pixels.size() != width * height * 4)
        {
            return false;
        }
        for (auto command : gSubmitted)
        {
            [command waitUntilCompleted];
            if (command != nil && CheckCommand(command) == false)
            {
                return false;
            }
        }
        const usize pitch = (width * 4 + 255) / 256 * 256;
        id<MTLBuffer> buffer = [gDevice newBufferWithLength:pitch * height options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        if (buffer == nil || encoder == nil)
        {
            [encoder endEncoding];
            return false;
        }
        [encoder copyFromTexture:headlessOrNull
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOrigin{0, 0, 0}
                          sourceSize:MTLSize{width, height, 1}
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:pitch
            destinationBytesPerImage:pitch * height];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (CheckCommand(command) == false)
        {
            return false;
        }
        const auto* bytes = static_cast<const uint8*>(buffer.contents);
        for (usize y = 0; y < height; ++y)
        {
            for (usize x = 0; x < width; ++x)
            {
                const usize source = y * pitch + x * 4, target = (y * width + x) * 4;
                pixels[target] = bytes[source + 2];
                pixels[target + 1] = bytes[source + 1];
                pixels[target + 2] = bytes[source];
                pixels[target + 3] = bytes[source + 3];
            }
        }
        return true;
    }
}
#endif
} // namespace ludus::graphics::rhi::backend

#include "internal/gpu_scope_metal.h"
#include "internal/raster_metal.h"
#include "internal/spatial_upscale_metal.h"

#include "internal/lifetime_metal.h"

#include "internal/ray_metal.h"

#include "internal/measurement_metal.h"

#include "internal/native_interop_metal.h"

namespace ludus::graphics::rhi::backend
{
id<MTLDevice> GetSpatialUpscaleMetalDevice() noexcept
{
    return gDevice;
}
id<MTLTexture> GetSpatialUpscaleMetalTexture(foundation::usize slot) noexcept
{
    return gRasterTextures[slot];
}
id<MTLCommandBuffer> GetSpatialUpscaleMetalCommandBuffer() noexcept
{
    return gCommand;
}
} // namespace ludus::graphics::rhi::backend
