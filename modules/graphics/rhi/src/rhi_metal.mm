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
#include <climits>
#if defined(LUDUS_RHI_TEST_READBACK)
#    include "internal/readback.h"
#endif

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/logging/log_format.hpp>
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
id<MTLTexture> gHeadless = nil;
id<MTLTexture> gDepth = nil;
id<CAMetalDrawable> gDrawable = nil;
CAMetalLayer* gLayer = nil;
CALayer* gPreviousLayer = nil;
NSView* __unsafe_unretained gView = nil;
NSWindow* __unsafe_unretained gWindow = nil;
BOOL gPreviousWantsLayer = NO;
/// Producer of a hosted window's surface; connected only for WindowSystem::Hosted.
HostedProducerMetal gHosted;
/// Color format of the frame target: BGRA8 drawables, or RGBA8 hosted images.
MTLPixelFormat gSurfaceFormat = MTLPixelFormatBGRA8Unorm;
/// Color attachment of the open frame: drawable, hosted image or headless texture.
id<MTLTexture> gSurfaceTexture = nil;
Shader gShaders[internal::RESOURCE_CAPACITY];
Uniform gUniforms[internal::RESOURCE_CAPACITY];
Pipeline gPipelines[internal::RESOURCE_CAPACITY];
FrameTarget gTarget{};
FrameInfo gFrame{};
usize gSlot = 0;
uint32 gToken = 0;
bool gConnected = false;
bool gRendering = false;

void LogError(NSError* error) noexcept
{
    const char* message = error.localizedDescription.UTF8String;
    LUDUS_LOG_ERROR(LOG_RHI, "Metal: {}", message != nullptr ? message : "operation failed");
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
} // namespace
Backend Kind() noexcept
{
    return Backend::Metal;
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
            gDevice = MTLCreateSystemDefaultDevice();
        }
        // Thanks to Apple's "Metal Feature Set Tables" (resource limits),
        // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf:
        // Mac2 / Apple4 and later guarantee the conservative 16384 texture limit.
        if (gDevice == nil || ([gDevice supportsFamily:MTLGPUFamilyMac2] == false &&
                               [gDevice supportsFamily:MTLGPUFamilyApple4] == false))
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
        if (window.System == platform::WindowSystem::Cocoa)
        {
            gWindow = (__bridge NSWindow*)window.CocoaWindow;
            gView = (__bridge NSView*)window.CocoaView;
            gLayer = [CAMetalLayer layer];
            if (gLayer == nil)
            {
                gWindow = nil;
                gView = nil;
                return false;
            }
            gLayer.device = gDevice;
            gLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            gLayer.framebufferOnly = YES;
            gLayer.maximumDrawableCount = FRAMES;
            gLayer.allowsNextDrawableTimeout = YES;
            gPreviousWantsLayer = gView.wantsLayer;
            gPreviousLayer = gView.layer;
            gView.wantsLayer = YES;
            gView.layer = gLayer;
        }
        gSurfaceFormat = MTLPixelFormatBGRA8Unorm;
        if (window.System == platform::WindowSystem::Hosted)
        {
            // The editor presents the hosted images; they are RGBA8 (docs/architecture/editor-gui-systems.md).
            gHosted.Connect(gDevice, *window.HostedSurfaceOrNull);
            gSurfaceFormat = MTLPixelFormatRGBA8Unorm;
        }
        gTarget = { .Width = window.Width, .Height = window.Height };
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
        [gEncoder endEncoding];
        gEncoder = nil;
        gCommand = nil;
        gDrawable = nil;
        for (auto& command : gSubmitted)
        {
            [command waitUntilCompleted];
            command = nil;
        }
        gHosted.Cancel();
        gSurfaceTexture = nil;
        gHeadless = nil;
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
        gHosted.Disconnect();
        gSurfaceFormat = MTLPixelFormatBGRA8Unorm;
        if (gView != nil && gView.layer == gLayer)
        {
            gView.layer = gPreviousLayer;
            gView.wantsLayer = gPreviousWantsLayer;
        }
        gView = nil;
        gWindow = nil;
        gLayer = nil;
        gPreviousLayer = nil;
        gPreviousWantsLayer = NO;
        gQueue = nil;
        gDevice = nil;
        gTarget = {};
        gConnected = false;
        gToken = 0;
    }
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
    gTarget = target;
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
        if (gHosted.IsConnected() == true && gHosted.Poll() == false)
        {
            return FrameFailure();
        }
        if (gTarget.Width == 0 || gTarget.Height == 0 ||
            (gWindow != nil && (gWindow.miniaturized || gWindow.visible == false)))
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
        if (gHosted.IsConnected() == true)
        {
            const HostedAcquireStatus acquired = gHosted.Acquire(gTarget.Width, gTarget.Height, texture);
            if (acquired == HostedAcquireStatus::Skipped)
            {
                return FrameStatus::Skipped;
            }
            if (acquired == HostedAcquireStatus::Failed)
            {
                return FrameFailure();
            }
        }
        else if (gLayer != nil)
        {
            gLayer.frame = gView.bounds;
            gLayer.contentsScale = gWindow.backingScaleFactor;
            gLayer.drawableSize = CGSizeMake(gTarget.Width, gTarget.Height);
            gDrawable = [gLayer nextDrawable];
            if (gDrawable == nil)
            {
                return FrameStatus::Skipped;
            }
            texture = gDrawable.texture;
        }
        else
        {
            if (gHeadless == nil || gHeadless.width != gTarget.Width || gHeadless.height != gTarget.Height)
            {
                MTLTextureDescriptor* description =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                       width:gTarget.Width
                                                                      height:gTarget.Height
                                                                   mipmapped:NO];
                description.usage = MTLTextureUsageRenderTarget;
                description.storageMode = MTLStorageModePrivate;
                gHeadless = [gDevice newTextureWithDescriptor:description];
            }
            texture = gHeadless;
        }
        if (texture == nil)
        {
            return FrameFailure();
        }
        if (gDepth == nil || gDepth.width != texture.width || gDepth.height != texture.height)
        {
            MTLTextureDescriptor* depth =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                   width:texture.width
                                                                  height:texture.height
                                                               mipmapped:NO];
            depth.usage = MTLTextureUsageRenderTarget;
            depth.storageMode = MTLStorageModePrivate;
            gDepth = [gDevice newTextureWithDescriptor:depth];
        }
        if (gDepth == nil)
        {
            gHosted.Cancel();
            return FrameFailure();
        }
        gCommand = [gQueue commandBuffer];
        BeginPassTimingFrame();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = gDepth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 1;
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake(gTarget.Red, gTarget.Green, gTarget.Blue, gTarget.Alpha);
        gEncoder = [gCommand renderCommandEncoderWithDescriptor:pass];
        if (gEncoder == nil)
        {
            gHosted.Cancel();
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
            [gEncoder endEncoding];
        }
        gEncoder = nil;
        if (gDrawable != nil)
        {
            [gCommand presentDrawable:gDrawable];
        }
        TimingEnd();
        SubmitPassTimings();
        [gCommand commit];
        if (gHosted.IsConnected() == true)
        {
            // Announced with Present once the command buffer completes (CPU fence).
            gHosted.Submit(gCommand);
            static_cast<void>(gHosted.Poll());
        }
        gSurfaceTexture = nil;
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
void UpdateUniform(usize slot, std::span<const uint8> bytes) noexcept
{
    std::memcpy(gUniforms[slot].Bytes, bytes.data(), bytes.size());
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
    [gEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    return ResourceStatus::Ready;
}
#if defined(LUDUS_RHI_TEST_READBACK)
bool ReadHeadlessPixels(std::span<uint8> pixels) noexcept
{
    @autoreleasepool
    {
        if (gRendering == false || gLayer != nil || gEncoder != nil || gHeadless == nil)
        {
            return false;
        }
        const usize width = gHeadless.width, height = gHeadless.height;
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
        [encoder copyFromTexture:gHeadless
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

#include "internal/lifetime_metal.h"

#include "internal/ray_metal.h"

#include "internal/measurement_metal.h"

#include "internal/native_interop_metal.h"
