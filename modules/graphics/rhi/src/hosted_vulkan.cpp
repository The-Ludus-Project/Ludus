#include "internal/hosted_vulkan.h"

#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include <cstring>
#include <unistd.h>

namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace foundation;
constexpr logging::LogCategory LOG_RHI_HOSTED{"RHI"};
} // namespace

void HostedProducerVulkan::Connect(const HostedVulkanDevice& device,
                                   platform::HostedSurfaceChannel& inoutChannel) noexcept
{
    Disconnect();
    mDevice = device;
    mChannelOrNull = &inoutChannel;
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    vkGetPhysicalDeviceProperties2(device.PhysicalDevice, &properties);
    static_assert(VK_UUID_SIZE == platform::HOSTED_ADAPTER_UUID_BYTES);
    std::memcpy(mDeviceUuid.GetData(), identity.deviceUUID, VK_UUID_SIZE);
    std::memcpy(mDriverUuid.GetData(), identity.driverUUID, VK_UUID_SIZE);
}

void HostedProducerVulkan::Disconnect() noexcept
{
    if (mDevice.Device != VK_NULL_HANDLE)
    {
        for (usize slot = 0; slot < internal::MAX_HOSTED_GENERATIONS; ++slot)
        {
            destroyGeneration(slot);
        }
    }
    mRing.Reset();
    mPending = {};
    mHasPending = false;
    mIsAcquired = false;
    mIsChannelLost = false;
    mChannelOrNull = nullptr;
    mDevice = {};
}

bool HostedProducerVulkan::tryFindMemoryType(uint32 typeBits, uint32& outIndex) const noexcept
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(mDevice.PhysicalDevice, &properties);
    for (uint32 i = 0; i < properties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1U << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
        {
            outIndex = i;
            return true;
        }
    }
    return false;
}

bool HostedProducerVulkan::createGeneration(usize slot, VkRenderPass renderPass, VkImageView depthView) noexcept
{
    const uint32 width = mRing.GetWidth(slot);
    const uint32 height = mRing.GetHeight(slot);
    platform::HostedSurfaceImages images;
    images.Generation = mRing.GetGeneration(slot);
    images.Width = width;
    images.Height = height;
    images.Format = platform::HostedImageFormat::Rgba8Unorm;
    images.DeviceUuid = mDeviceUuid;
    images.DriverUuid = mDriverUuid;
    for (usize index = 0; index < internal::HOSTED_IMAGE_COUNT; ++index)
    {
        Image& image = mImages[slot][index];
        VkExternalMemoryImageCreateInfo external{};
        VkImageCreateInfo create{};
        FillHostedImageInfo(width, height, external, create);
        if (vkCreateImage(mDevice.Device, &create, nullptr, &image.Object) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(mDevice.Device, image.Object, &requirements);
        uint32 memoryType = 0;
        if (tryFindMemoryType(requirements.memoryTypeBits, memoryType) == false ||
            (index > 0 && memoryType != images.MemoryTypeIndex))
        {
            return false;
        }
        // Dedicated, exportable memory: the importer binds the payload to an identical image.
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = image.Object;
        VkExportMemoryAllocateInfo exportInfo{};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportInfo.pNext = &dedicated;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &exportInfo;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(mDevice.Device, &allocate, nullptr, &image.Memory) != VK_SUCCESS ||
            vkBindImageMemory(mDevice.Device, image.Object, image.Memory, 0) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryGetFdInfoKHR exported{};
        exported.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        exported.memory = image.Memory;
        exported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int32 descriptor = -1;
        if (vkGetMemoryFdKHR == nullptr || vkGetMemoryFdKHR(mDevice.Device, &exported, &descriptor) != VK_SUCCESS)
        {
            return false;
        }
        images.Handles[index] = platform::HostedImageHandle::FromFileDescriptor(descriptor);
        images.AllocationSizes[index] = requirements.size;
        images.MemoryTypeIndex = memoryType;

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image.Object;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = HOSTED_IMAGE_FORMAT;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(mDevice.Device, &view, nullptr, &image.View) != VK_SUCCESS)
        {
            return false;
        }
        const StaticArray<VkImageView, 2> attachments = {image.View, depthView};
        VkFramebufferCreateInfo target{};
        target.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        target.renderPass = renderPass;
        target.attachmentCount = static_cast<uint32>(attachments.GetSize());
        target.pAttachments = attachments.GetData();
        target.width = width;
        target.height = height;
        target.layers = 1;
        if (vkCreateFramebuffer(mDevice.Device, &target, nullptr, &image.Framebuffer) != VK_SUCCESS)
        {
            return false;
        }
    }
    mPending = foundation::Move(images);
    mHasPending = true;
    return true;
}

void HostedProducerVulkan::destroyGeneration(usize slot) noexcept
{
    for (Image& image : mImages[slot])
    {
        vkDestroyFramebuffer(mDevice.Device, image.Framebuffer, nullptr);
        vkDestroyImageView(mDevice.Device, image.View, nullptr);
        vkDestroyImage(mDevice.Device, image.Object, nullptr);
        vkFreeMemory(mDevice.Device, image.Memory, nullptr);
        image = {};
    }
}

bool HostedProducerVulkan::publishPending() noexcept
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

void HostedProducerVulkan::MarkSignaled(VkFence fence) noexcept
{
    for (StaticArray<Image, internal::HOSTED_IMAGE_COUNT>& generation : mImages)
    {
        for (Image& image : generation)
        {
            if (image.Fence == fence && fence != VK_NULL_HANDLE)
            {
                image.IsSignaled = true;
            }
        }
    }
}

bool HostedProducerVulkan::Poll() noexcept
{
    if (mChannelOrNull == nullptr)
    {
        return true;
    }
    internal::HostedSubmission submission;
    while (mRing.TryPeekSubmission(submission) == true)
    {
        Image& image = mImages[submission.Slot][submission.Image];
        if (image.IsSignaled == false)
        {
            // The backend reuses a frame fence only after waiting for it and calling
            // MarkSignaled(), so an unsignalled fence still guards this frame.
            const VkResult result = vkGetFenceStatus(mDevice.Device, image.Fence);
            if (result == VK_NOT_READY)
            {
                break;
            }
            if (result != VK_SUCCESS)
            {
                return false;
            }
        }
        bool isPresented = false;
        if (mIsChannelLost == false && mRing.IsCurrentSubmission(submission) == true)
        {
            const platform::HostedSurfaceStatus status =
                mChannelOrNull->Present(mRing.GetGeneration(submission.Slot), submission.Image, submission.Sequence);
            if (status == platform::HostedSurfaceStatus::Busy)
            {
                // Completion is recorded, so the fence may be reused before the retry.
                image.IsSignaled = true;
                break;
            }
            isPresented = status == platform::HostedSurfaceStatus::Ok;
            mIsChannelLost = isPresented == false;
        }
        image.Fence = VK_NULL_HANDLE;
        image.IsSignaled = false;
        mRing.CompleteSubmission(isPresented);
    }
    return true;
}

HostedVulkanAcquireStatus HostedProducerVulkan::Acquire(uint32 width,
                                                        uint32 height,
                                                        VkRenderPass renderPass,
                                                        VkImageView depthView,
                                                        uint64 depthEpoch,
                                                        VkFramebuffer& outFramebuffer) noexcept
{
    if (mChannelOrNull == nullptr || mIsAcquired == true || Poll() == false)
    {
        return HostedVulkanAcquireStatus::Failed;
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
        // Every image of a retired generation is free: its frames completed and were released.
        destroyGeneration(retired);
    }
    if (mIsChannelLost == true)
    {
        return HostedVulkanAcquireStatus::Skipped;
    }
    if (mRing.IsCurrentExtent(width, height) == false || mDepthEpochs[mRing.GetCurrentSlot()] != depthEpoch)
    {
        if (mHasPending == true)
        {
            const usize abandoned = mRing.GetCurrentSlot();
            mRing.AbandonGeneration(abandoned);
            destroyGeneration(abandoned);
            mPending = {};
            mHasPending = false;
        }
        usize slot = 0;
        if (mRing.TryBeginGeneration(width, height, slot) == false)
        {
            return HostedVulkanAcquireStatus::Skipped;
        }
        mDepthEpochs[slot] = depthEpoch;
        if (createGeneration(slot, renderPass, depthView) == false)
        {
            LUDUS_LOG_ERROR(LOG_RHI_HOSTED, "Cannot create or export the hosted surface images");
            mRing.AbandonGeneration(slot);
            destroyGeneration(slot);
            mPending = {};
            mHasPending = false;
            return HostedVulkanAcquireStatus::Failed;
        }
    }
    if (publishPending() == false)
    {
        return HostedVulkanAcquireStatus::Skipped;
    }
    uint32 image = 0;
    if (mRing.TryAcquire(image) == false)
    {
        return HostedVulkanAcquireStatus::Skipped;
    }
    mIsAcquired = true;
    mAcquiredImage = image;
    outFramebuffer = mImages[mRing.GetCurrentSlot()][image].Framebuffer;
    return HostedVulkanAcquireStatus::Ready;
}

void HostedProducerVulkan::RecordRelease(VkCommandBuffer command) const noexcept
{
    if (mIsAcquired == false)
    {
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // A release has no destination access in this queue; the consumer's acquire supplies it.
    barrier.dstAccessMask = 0;
    barrier.oldLayout = HOSTED_RENDERED_LAYOUT;
    barrier.newLayout = HOSTED_PRESENTED_LAYOUT;
    barrier.srcQueueFamilyIndex = mDevice.QueueFamily;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = mImages[mRing.GetCurrentSlot()][mAcquiredImage].Object;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
}

void HostedProducerVulkan::Submit(VkFence fence) noexcept
{
    if (mIsAcquired == false)
    {
        return;
    }
    Image& image = mImages[mRing.GetCurrentSlot()][mAcquiredImage];
    image.Fence = fence;
    image.IsSignaled = false;
    static_cast<void>(mRing.Submit(mAcquiredImage));
    mIsAcquired = false;
}

void HostedProducerVulkan::Cancel() noexcept
{
    if (mIsAcquired == true)
    {
        mRing.CancelAcquire(mAcquiredImage);
        mIsAcquired = false;
    }
}
} // namespace ludus::graphics::rhi::backend
