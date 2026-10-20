#include "internal/hosted_consumer.h"

#include <ludus/foundation/containers/static_array.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/MTLCommandBuffer.h>
#import <Metal/MTLCommandQueue.h>
#import <Metal/MTLDevice.h>
#import <Metal/MTLTexture.h>

#include <new>

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

/// Metal objects of one consumer; every pointer is a retained (CFBridgingRetain) Objective-C object or null.
struct HostedConsumerBackend final
{
    /// @param device A Metal identity with its `id<MTLDevice>` and, optionally, its `id<MTLCommandQueue>`.
    explicit HostedConsumerBackend(const NativeDeviceIdentity& device) noexcept
        : Device(device.Device.GetObject()), QueueOrNull(device.CommandQueue.GetObject())
    {
    }

    /// The consumer's `id<MTLDevice>`, borrowed.
    void* Device;
    /// The consumer's `id<MTLCommandQueue>`, borrowed; null when the identity has none.
    void* QueueOrNull;
    /// IOSurface-backed `id<MTLTexture>` objects per generation slot.
    StaticArray<StaticArray<void*, platform::HOSTED_SURFACE_IMAGE_COUNT>, HOSTED_GENERATION_SLOTS> Textures{};
    /// Queued release markers, `id<MTLCommandBuffer>`, per generation slot.
    StaticArray<StaticArray<void*, platform::HOSTED_SURFACE_IMAGE_COUNT>, HOSTED_GENERATION_SLOTS> Markers{};
};

namespace
{
void ReleaseObject(void*& inoutObject) noexcept
{
    if (inoutObject != nullptr)
    {
        CFRelease(inoutObject);
        inoutObject = nullptr;
    }
}

void ReleaseSlot(HostedConsumerBackend& inoutBackend, usize slot) noexcept
{
    for (usize i = 0; i < platform::HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        ReleaseObject(inoutBackend.Textures[slot][i]);
        ReleaseObject(inoutBackend.Markers[slot][i]);
    }
}
} // namespace

void HostedConsumerBackendDeleter::operator()(HostedConsumerBackend* backendOrNull) const noexcept
{
    if (backendOrNull == nullptr)
    {
        return;
    }
    for (usize slot = 0; slot < HOSTED_GENERATION_SLOTS; ++slot)
    {
        ReleaseSlot(*backendOrNull, slot);
    }
    delete backendOrNull;
}

bool IsHostedImportAvailable(Backend api) noexcept
{
    return api == Backend::Metal;
}

HostedConsumerStatus CreateHostedConsumerBackend(const NativeDeviceIdentity& device,
                                                 HostedConsumerBackendOwner& outBackend) noexcept
{
    if (device.Api != Backend::Metal || device.Device.IsNull() == true)
    {
        return HostedConsumerStatus::InvalidArgument;
    }
    HostedConsumerBackend* backendOrNull = new (std::nothrow) HostedConsumerBackend(device);
    if (backendOrNull == nullptr)
    {
        return HostedConsumerStatus::Unavailable;
    }
    outBackend.Reset(backendOrNull);
    return HostedConsumerStatus::Ok;
}

HostedConsumerRefusal ImportHostedGeneration(HostedConsumerBackend& inoutBackend,
                                             usize slot,
                                             platform::HostedSurfaceImages& inoutImages) noexcept
{
    @autoreleasepool
    {
        HostedConsumerRefusal refusal = HostedConsumerRefusal::None;
        id<MTLDevice> metal = (__bridge id<MTLDevice>)inoutBackend.Device;
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
            inoutBackend.Textures[slot][i] = (__bridge_retained void*)texture;
        }
        for (platform::HostedImageHandle& handle : inoutImages.Handles)
        {
            handle.Reset();
        }
        if (refusal != HostedConsumerRefusal::None)
        {
            ReleaseSlot(inoutBackend, slot);
        }
        return refusal;
    }
}

void DestroyHostedGeneration(HostedConsumerBackend& inoutBackend, usize slot) noexcept
{
    ReleaseSlot(inoutBackend, slot);
}

// Metal tracks hazards of shared IOSurface textures itself; there is nothing to submit.
bool AcquireHostedImage(HostedConsumerBackend& inoutBackend, usize slot, uint32 image, HostedFrame& inoutFrame) noexcept
{
    inoutFrame.MetalTexture = NativeMetalTexture(inoutBackend.Textures[slot][image]);
    return true;
}

// Metal runs a queue's command buffers in commit order, so an empty buffer committed
// after the editor's sampling completes only once that sampling has finished. This is
// the same queue-order guarantee RHI's native texture reads rely on.
bool EnqueueHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    @autoreleasepool
    {
        if (inoutBackend.QueueOrNull == nullptr || inoutBackend.Markers[slot][image] != nullptr)
        {
            return false;
        }
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)inoutBackend.QueueOrNull;
        id<MTLCommandBuffer> marker = [queue commandBuffer];
        if (marker == nil)
        {
            return false;
        }
        marker.label = @"Ludus hosted frame release";
        [marker commit];
        inoutBackend.Markers[slot][image] = (__bridge_retained void*)marker;
        return true;
    }
}

bool TryCompleteHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    void*& marker = inoutBackend.Markers[slot][image];
    if (marker == nullptr)
    {
        return true;
    }
    const MTLCommandBufferStatus status = ((__bridge id<MTLCommandBuffer>)marker).status;
    if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError)
    {
        return false;
    }
    ReleaseObject(marker);
    return true;
}

void CompleteHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    void*& marker = inoutBackend.Markers[slot][image];
    if (marker != nullptr)
    {
        [(__bridge id<MTLCommandBuffer>)marker waitUntilCompleted];
        ReleaseObject(marker);
    }
}
} // namespace ludus::graphics::rhi::internal
