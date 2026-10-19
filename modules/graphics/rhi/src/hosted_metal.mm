#include "internal/hosted_metal.h"

#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <IOSurface/IOSurfaceRef.h>

// Thanks to Apple for the IOSurface reference (https://developer.apple.com/documentation/iosurface):
// IOSurfaceCreateMachPort passes a surface to another task "without making the
// surface global to the entire system", which is why the hosted surface shares
// Mach send rights and never global surface IDs. The textures are created with
// -[MTLDevice newTextureWithDescriptor:iosurface:plane:].
namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace foundation;
constexpr logging::LogCategory LOG_RHI_HOSTED{"RHI"};
constexpr int32 BYTES_PER_PIXEL = 4;
/// kCVPixelFormatType_32RGBA: bytes R, G, B, A, matching MTLPixelFormatRGBA8Unorm.
constexpr int32 RGBA_PIXEL_FORMAT = static_cast<int32>('RGBA');

void SetNumber(CFMutableDictionaryRef dictionary, CFStringRef key, int64 value) noexcept
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    if (number != nullptr)
    {
        CFDictionarySetValue(dictionary, key, number);
        CFRelease(number);
    }
}

IOSurfaceRef CreateSurface(uint32 width, uint32 height) noexcept
{
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                  0,
                                                                  &kCFTypeDictionaryKeyCallBacks,
                                                                  &kCFTypeDictionaryValueCallBacks);
    if (properties == nullptr)
    {
        return nullptr;
    }
    const usize bytesPerRow =
        IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, static_cast<usize>(width) * BYTES_PER_PIXEL);
    SetNumber(properties, kIOSurfaceWidth, width);
    SetNumber(properties, kIOSurfaceHeight, height);
    SetNumber(properties, kIOSurfaceBytesPerElement, BYTES_PER_PIXEL);
    SetNumber(properties, kIOSurfaceBytesPerRow, static_cast<int64>(bytesPerRow));
    SetNumber(properties, kIOSurfacePixelFormat, RGBA_PIXEL_FORMAT);
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    return surface;
}
} // namespace

void HostedProducerMetal::Connect(id<MTLDevice> device, platform::HostedSurfaceChannel& inoutChannel) noexcept
{
    Disconnect();
    mDevice = device;
    mChannelOrNull = &inoutChannel;
}

void HostedProducerMetal::Disconnect() noexcept
{
    @autoreleasepool
    {
        for (usize slot = 0; slot < internal::MAX_HOSTED_GENERATIONS; ++slot)
        {
            destroyGeneration(slot);
        }
        mRing.Reset();
        mPending = {};
        mHasPending = false;
        mIsAcquired = false;
        mIsChannelLost = false;
        mChannelOrNull = nullptr;
        mDevice = nil;
    }
}

bool HostedProducerMetal::createGeneration(usize slot) noexcept
{
    const uint32 width = mRing.GetWidth(slot);
    const uint32 height = mRing.GetHeight(slot);
    platform::HostedSurfaceImages images;
    images.Generation = mRing.GetGeneration(slot);
    images.Width = width;
    images.Height = height;
    images.Format = platform::HostedImageFormat::Rgba8Unorm;
    MTLTextureDescriptor* description =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    description.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    // IOSurface-backed textures cannot be private; unified memory shares them directly.
    description.storageMode = mDevice.hasUnifiedMemory == YES ? MTLStorageModeShared : MTLStorageModeManaged;
    for (usize image = 0; image < internal::HOSTED_IMAGE_COUNT; ++image)
    {
        IOSurfaceRef surface = CreateSurface(width, height);
        if (surface == nullptr)
        {
            return false;
        }
        mTextures[slot][image] = [mDevice newTextureWithDescriptor:description iosurface:surface plane:0];
        images.Handles[image] = platform::HostedImageHandle::FromMachPort(IOSurfaceCreateMachPort(surface));
        // The texture keeps its surface alive.
        CFRelease(surface);
        if (mTextures[slot][image] == nil || images.Handles[image].IsValid() == false)
        {
            return false;
        }
    }
    mPending = foundation::Move(images);
    mHasPending = true;
    return true;
}

void HostedProducerMetal::destroyGeneration(usize slot) noexcept
{
    for (usize image = 0; image < internal::HOSTED_IMAGE_COUNT; ++image)
    {
        [mCommands[slot][image] waitUntilCompleted];
        mCommands[slot][image] = nil;
        mTextures[slot][image] = nil;
    }
}

bool HostedProducerMetal::publishPending() noexcept
{
    if (mHasPending == false)
    {
        return true;
    }
    const platform::HostedSurfaceStatus status = mChannelOrNull->PublishImages(mPending);
    if (status == platform::HostedSurfaceStatus::Busy)
    {
        return false;
    }
    if (status != platform::HostedSurfaceStatus::Ok)
    {
        LUDUS_LOG_ERROR(LOG_RHI_HOSTED, "The hosted window refused the surface images");
        mIsChannelLost = true;
        return false;
    }
    mRing.MarkPublished(mRing.GetCurrentSlot());
    mPending = {};
    mHasPending = false;
    return true;
}

bool HostedProducerMetal::Poll() noexcept
{
    if (mChannelOrNull == nullptr)
    {
        return true;
    }
    internal::HostedSubmission submission;
    while (mRing.TryPeekSubmission(submission) == true)
    {
        id<MTLCommandBuffer> command = mCommands[submission.Slot][submission.Image];
        if (command.status == MTLCommandBufferStatusError)
        {
            return false;
        }
        if (command != nil && command.status != MTLCommandBufferStatusCompleted)
        {
            break;
        }
        bool isPresented = false;
        if (mIsChannelLost == false && mRing.IsCurrentSubmission(submission) == true)
        {
            const platform::HostedSurfaceStatus status =
                mChannelOrNull->Present(mRing.GetGeneration(submission.Slot), submission.Image, submission.Sequence);
            if (status == platform::HostedSurfaceStatus::Busy)
            {
                break;
            }
            isPresented = status == platform::HostedSurfaceStatus::Ok;
            mIsChannelLost = isPresented == false;
        }
        mCommands[submission.Slot][submission.Image] = nil;
        mRing.CompleteSubmission(isPresented);
    }
    return true;
}

HostedAcquireStatus
HostedProducerMetal::Acquire(uint32 width, uint32 height, id<MTLTexture> __strong& outTexture) noexcept
{
    @autoreleasepool
    {
        if (mChannelOrNull == nullptr || mIsAcquired == true || Poll() == false)
        {
            return HostedAcquireStatus::Failed;
        }
        platform::HostedImageRelease release;
        platform::HostedSurfaceStatus status = platform::HostedSurfaceStatus::Ok;
        while (mIsChannelLost == false && status == platform::HostedSurfaceStatus::Ok)
        {
            status = mChannelOrNull->TakeRelease(release);
            if (status == platform::HostedSurfaceStatus::Ok &&
                mRing.Release(release.Generation, release.Image) != internal::HostedReleaseResult::Ok)
            {
                LUDUS_LOG_WARN(LOG_RHI_HOSTED, "Ignored a release of an image the editor was not shown");
            }
        }
        mIsChannelLost = mIsChannelLost == true || status == platform::HostedSurfaceStatus::Disconnected;
        usize retired = 0;
        while (mRing.TryTakeRetired(retired) == true)
        {
            destroyGeneration(retired);
        }
        if (mIsChannelLost == true)
        {
            return HostedAcquireStatus::Skipped;
        }
        if (mRing.IsCurrentExtent(width, height) == false)
        {
            if (mHasPending == true)
            {
                // A newer extent supersedes a generation the channel has not accepted yet.
                const usize abandoned = mRing.GetCurrentSlot();
                mRing.AbandonGeneration(abandoned);
                destroyGeneration(abandoned);
                mPending = {};
                mHasPending = false;
            }
            usize slot = 0;
            if (mRing.TryBeginGeneration(width, height, slot) == false)
            {
                return HostedAcquireStatus::Skipped;
            }
            if (createGeneration(slot) == false)
            {
                LUDUS_LOG_ERROR(LOG_RHI_HOSTED, "Cannot create the hosted surface images");
                mRing.AbandonGeneration(slot);
                destroyGeneration(slot);
                mPending = {};
                mHasPending = false;
                return HostedAcquireStatus::Failed;
            }
        }
        if (publishPending() == false)
        {
            return HostedAcquireStatus::Skipped;
        }
        uint32 image = 0;
        if (mRing.TryAcquire(image) == false)
        {
            return HostedAcquireStatus::Skipped;
        }
        mIsAcquired = true;
        mAcquiredImage = image;
        outTexture = mTextures[mRing.GetCurrentSlot()][image];
        return HostedAcquireStatus::Ready;
    }
}

void HostedProducerMetal::Submit(id<MTLCommandBuffer> command) noexcept
{
    if (mIsAcquired == false)
    {
        return;
    }
    mCommands[mRing.GetCurrentSlot()][mAcquiredImage] = command;
    static_cast<void>(mRing.Submit(mAcquiredImage));
    mIsAcquired = false;
}

void HostedProducerMetal::Cancel() noexcept
{
    if (mIsAcquired == true)
    {
        mRing.CancelAcquire(mAcquiredImage);
        mIsAcquired = false;
    }
}
} // namespace ludus::graphics::rhi::backend
