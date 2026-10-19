#pragma once

#include "native_interop.h"

// Thanks to Khronos, "Vulkan Specification", Synchronization and Cache Control,
// submission order and fence signal operations: a pipeline barrier publishes the
// producer image, and a zero-submit fence follows foreign work on the same queue.
// https://docs.vulkan.org/spec/latest/chapters/synchronization.html
namespace ludus::graphics::rhi::backend
{
namespace
{
struct NativeVulkanRead final
{
    VkCommandPool Pool = VK_NULL_HANDLE;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Publication = VK_NULL_HANDLE;
    VkFence Completion = VK_NULL_HANDLE;
    bool Released = false;
};
internal::StorageTable<NativeVulkanRead> gNativeVulkanReads;
RasterStatus NativeVkResult(VkResult result) noexcept
{
    return result == VK_SUCCESS                             ? RasterStatus::Ready
           : result == VK_NOT_READY || result == VK_TIMEOUT ? RasterStatus::Pending
           : result == VK_ERROR_DEVICE_LOST                 ? RasterStatus::DeviceLost
           : result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
               ? RasterStatus::OutOfMemory
               : RasterStatus::Failed;
}
} // namespace
bool NativeInteropSupported() noexcept
{
    return true;
}
RasterStatus NativeDeviceSnapshot(NativeDeviceInfo& output) noexcept
{
    if (gDevice == VK_NULL_HANDLE || gQueue == VK_NULL_HANDLE)
    {
        return RasterStatus::NotReady;
    }
    const auto* imported = internal::NativeDeviceRequest();
    NativeDeviceInfo result
    {
        .Identity =
        {
            .Api = Backend::Vulkan,
            .Instance = NativeInstance{gInstance},
            .PhysicalDevice = NativePhysicalDevice{gPhysical},
            .Device = NativeLogicalDevice{gDevice},
            .CommandQueue = NativeCommandQueue{gQueue},
            .QueueFamily = gFamily,
            .QueueIndex = imported == nullptr ? 0 : imported->Identity.QueueIndex,
        },
        .InstanceApiVersion = gNativeInstanceVersion,
    };
    VkPhysicalDeviceIDProperties identity
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &identity,
    };
    vkGetPhysicalDeviceProperties2(gPhysical, &properties);
    std::memcpy(result.DeviceUuid, identity.deviceUUID, sizeof(result.DeviceUuid));
    output = result;
    return RasterStatus::Ready;
}
RasterStatus NativeTexturePublish(const internal::NativeTexturePublication& publication,
                                  NativeTextureInfo& output) noexcept
{
    const auto slot = publication.Lease;
    const auto textureSlot = publication.Texture;
    if (gNativeVulkanReads.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    auto& read = gNativeVulkanReads[slot];
    VkCommandPoolCreateInfo pool
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = gFamily,
    };
    auto result = vkCreateCommandPool(gDevice, &pool, nullptr, &read.Pool);
    if (result != VK_SUCCESS)
    {
        return NativeVkResult(result);
    }
    VkCommandBufferAllocateInfo allocate
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = read.Pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = vkAllocateCommandBuffers(gDevice, &allocate, &read.Command);
    if (result != VK_SUCCESS)
    {
        return NativeVkResult(result);
    }
    VkFenceCreateInfo fence
    {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    result = vkCreateFence(gDevice, &fence, nullptr, &read.Publication);
    if (result != VK_SUCCESS)
    {
        return NativeVkResult(result);
    }
    result = vkCreateFence(gDevice, &fence, nullptr, &read.Completion);
    if (result != VK_SUCCESS)
    {
        return NativeVkResult(result);
    }
    VkCommandBufferBeginInfo begin
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(read.Command, &begin);
    if (result != VK_SUCCESS)
    {
        return NativeVkResult(result);
    }
    auto& texture = gRasterTextures[textureSlot];
    VkImageMemoryBarrier barrier
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = texture.ColorWriterAccess | texture.ColorAccess,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = texture.ColorLayout,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture.Object,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    // ALL_COMMANDS includes every enabled shader stage supported by this queue,
    // including a foreign geometry/tessellation/mesh consumer, without requiring
    // those optional features merely to build the publication barrier.
    constexpr VkPipelineStageFlags readers = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vkCmdPipelineBarrier(read.Command,
                         texture.ColorStage | texture.ColorWriterStage,
                         readers,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
    result = vkEndCommandBuffer(read.Command);
    if (result != VK_SUCCESS)
    {
        return NativeVkResult(result);
    }
    VkSubmitInfo submit
    {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &read.Command,
    };
    result = vkQueueSubmit(gQueue, 1, &submit, read.Publication);
    if (result != VK_SUCCESS)
    {
        return NativeVkResult(result);
    }
    texture.ColorLayout = barrier.newLayout;
    texture.ColorStage |= readers;
    texture.ColorAccess = VK_ACCESS_SHADER_READ_BIT;
    texture.SampleVisibility |= readers;
#if VK_USE_64_BIT_PTR_DEFINES
    output.VulkanImage = NativeVulkanImage{reinterpret_cast<uint64>(texture.Object)};
#else
    output.VulkanImage = NativeVulkanImage{texture.Object};
#endif
    output.VulkanLayout = static_cast<uint32>(barrier.newLayout);
    return RasterStatus::Ready;
}
RasterStatus NativeTextureRelease(usize slot) noexcept
{
    auto& read = gNativeVulkanReads[slot];
    if (read.Released)
    {
        return RasterStatus::Pending;
    }
    const auto result = vkQueueSubmit(gQueue, 0, nullptr, read.Completion);
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
    {
        read.Released = true;
    }
    return NativeVkResult(result);
}
RasterStatus NativeTexturePoll(usize slot, bool wait) noexcept
{
    auto& read = gNativeVulkanReads[slot];
    const auto fence = read.Released ? read.Completion : read.Publication;
    return NativeVkResult(wait ? vkWaitForFences(gDevice, 1, &fence, VK_TRUE, 5000000000ULL)
                               : vkGetFenceStatus(gDevice, fence));
}
void NativeTextureDestroy(usize slot) noexcept
{
    if (slot >= gNativeVulkanReads.GetSize())
    {
        return;
    }
    auto& read = gNativeVulkanReads[slot];
    vkDestroyFence(gDevice, read.Publication, nullptr);
    vkDestroyFence(gDevice, read.Completion, nullptr);
    vkDestroyCommandPool(gDevice, read.Pool, nullptr);
    read = {};
}
void NativeInteropDetachSurface() noexcept
{
    // A borrowed context is headless: keep its private pools/targets until the
    // last native pin retires. In particular, do not wait on a foreign consumer
    // whose forward progress may depend on this shutdown returning to its owner.
    if (gHostedProducer.IsConnected() == true)
    {
        // The hosted window's channel is borrowed only until the surface detaches.
        WaitIdle();
        gHostedProducer.Disconnect();
    }
    if (gSurface == VK_NULL_HANDLE)
    {
        gWindow = {};
        return;
    }
    RetireAcquiredImage();
    WaitIdle();
    gEncoding = false;
    ReleaseTargets();
    vkDestroyRenderPass(gDevice, gPass, nullptr);
    gPass = VK_NULL_HANDLE;
    if (gSurface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(gInstance, gSurface, nullptr);
        gSurface = VK_NULL_HANDLE;
    }
    gWindow = {};
}
void NativeInteropReset() noexcept {}
} // namespace ludus::graphics::rhi::backend
