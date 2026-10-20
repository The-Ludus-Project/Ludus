#include "internal/hosted_consumer.h"

#import <CoreFoundation/CoreFoundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/MTLCommandBuffer.h>
#import <Metal/MTLCommandQueue.h>
#import <Metal/MTLDevice.h>
#import <Metal/MTLTexture.h>

// Thanks to Apple for the IOSurface reference (https://developer.apple.com/documentation/iosurface):
// IOSurfaceLookupFromMachPort recreates a surface from a received send right and
// does not destroy the port; the texture then wraps it with
// -[MTLDevice newTextureWithDescriptor:iosurface:plane:].
namespace ludus::graphics::rhi::internal
{
namespace
{
using namespace foundation;
/// kCVPixelFormatType_32RGBA, as the producer allocates it.
constexpr uint32 RGBA_PIXEL_FORMAT = 'RGBA';
constexpr usize BYTES_PER_PIXEL = 4;
} // namespace

bool IsHostedImportAvailable(Backend api) noexcept
{
    return api == Backend::Metal;
}

HostedConsumerRefusal ImportHostedGeneration(const NativeDeviceIdentity& device,
                                             platform::HostedSurfaceImages& inoutImages,
                                             HostedImport& outImport) noexcept
{
    @autoreleasepool
    {
        HostedImport imported;
        HostedConsumerRefusal refusal = HostedConsumerRefusal::None;
        id<MTLDevice> metal = (__bridge id<MTLDevice>)device.Device.GetObject();
        MTLTextureDescriptor* description =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:inoutImages.Width
                                                              height:inoutImages.Height
                                                           mipmapped:NO];
        description.usage = MTLTextureUsageShaderRead;
        description.storageMode = metal.hasUnifiedMemory == YES ? MTLStorageModeShared : MTLStorageModeManaged;
        for (usize i = 0; i < platform::HOSTED_SURFACE_IMAGE_COUNT && refusal == HostedConsumerRefusal::None; ++i)
        {
            platform::HostedImageHandle handle = foundation::Move(inoutImages.Handles[i]);
            if (handle.GetKind() != platform::HostedHandleKind::MachPort)
            {
                refusal = HostedConsumerRefusal::UnsupportedHandle;
                break;
            }
            IOSurfaceRef surfaceOrNull = IOSurfaceLookupFromMachPort(handle.GetMachPort());
            // The surface holds its own reference; the send right is no longer needed.
            handle.Reset();
            if (surfaceOrNull == nullptr)
            {
                refusal = HostedConsumerRefusal::ImportFailed;
                break;
            }
            const bool isMatching = IOSurfaceGetWidth(surfaceOrNull) == inoutImages.Width &&
                                    IOSurfaceGetHeight(surfaceOrNull) == inoutImages.Height &&
                                    IOSurfaceGetPixelFormat(surfaceOrNull) == RGBA_PIXEL_FORMAT &&
                                    IOSurfaceGetBytesPerElement(surfaceOrNull) == BYTES_PER_PIXEL &&
                                    IOSurfaceGetPlaneCount(surfaceOrNull) == 0;
            id<MTLTexture> texture =
                isMatching == true ? [metal newTextureWithDescriptor:description iosurface:surfaceOrNull plane:0] : nil;
            CFRelease(surfaceOrNull);
            if (texture == nil)
            {
                refusal = HostedConsumerRefusal::ImportFailed;
                break;
            }
            imported.MetalTextures[i] = (__bridge_retained void*)texture;
        }
        for (platform::HostedImageHandle& handle : inoutImages.Handles)
        {
            handle.Reset();
        }
        if (refusal != HostedConsumerRefusal::None)
        {
            DestroyHostedImport(device, imported);
            return refusal;
        }
        outImport = imported;
        return HostedConsumerRefusal::None;
    }
}

void DestroyHostedImport([[maybe_unused]] const NativeDeviceIdentity& device, HostedImport& inoutImport) noexcept
{
    for (void*& texture : inoutImport.MetalTextures)
    {
        if (texture != nullptr)
        {
            CFRelease(texture);
            texture = nullptr;
        }
    }
}

// Metal runs a queue's command buffers in commit order, so an empty buffer committed
// after the editor's sampling completes only once that sampling has finished. This is
// the same queue-order guarantee RHI's native texture reads rely on.
bool EnqueueHostedQueueMarker(const NativeDeviceIdentity& device, HostedQueueMarker& outMarker) noexcept
{
    @autoreleasepool
    {
        if (device.Api != Backend::Metal || device.CommandQueue.IsNull() == true)
        {
            return false;
        }
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device.CommandQueue.GetObject();
        id<MTLCommandBuffer> marker = [queue commandBuffer];
        if (marker == nil)
        {
            return false;
        }
        marker.label = @"Ludus hosted frame release";
        [marker commit];
        outMarker.MetalCommandBufferOrNull = (__bridge_retained void*)marker;
        return true;
    }
}

bool IsHostedQueueMarkerComplete(const HostedQueueMarker& marker) noexcept
{
    if (marker.MetalCommandBufferOrNull == nullptr)
    {
        return true;
    }
    id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)marker.MetalCommandBufferOrNull;
    const MTLCommandBufferStatus status = buffer.status;
    return status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError;
}

void WaitHostedQueueMarker(const HostedQueueMarker& marker) noexcept
{
    if (marker.MetalCommandBufferOrNull != nullptr)
    {
        [(__bridge id<MTLCommandBuffer>)marker.MetalCommandBufferOrNull waitUntilCompleted];
    }
}

void DestroyHostedQueueMarker(HostedQueueMarker& inoutMarker) noexcept
{
    if (inoutMarker.MetalCommandBufferOrNull != nullptr)
    {
        CFRelease(inoutMarker.MetalCommandBufferOrNull);
    }
    inoutMarker = {};
}
} // namespace ludus::graphics::rhi::internal
