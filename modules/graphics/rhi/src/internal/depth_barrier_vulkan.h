#pragma once

#include <vulkan/vulkan_core.h>

namespace ludus::graphics::rhi::backend
{
struct VulkanDepthBarrier final
{
    VkPipelineStageFlags SourceStages;
    VkPipelineStageFlags DestinationStages;
    VkImageMemoryBarrier Image;
};

// Thanks to the Khronos Group, Vulkan Guide, "Synchronization Examples",
// Graphics to Graphics Dependencies and the depth attachment reuse example.
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
// Keep a memory dependency for depth attachment reuse, and scope shader stages
// to the selected queue. The barrier covers the whole depth/stencil image.
inline VulkanDepthBarrier MakeVulkanDepthBarrier(VkImage image,
                                                 VkImageSubresourceRange subresources,
                                                 VkImageLayout previousLayout,
                                                 bool isAttachmentUse,
                                                 bool hasComputeQueue) noexcept
{
    constexpr VkPipelineStageFlags DEPTH_TESTS =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags DEPTH_ACCESS =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                         (hasComputeQueue == true ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0U);
    const bool isUndefined = previousLayout == VK_IMAGE_LAYOUT_UNDEFINED;
    return
    {
        .SourceStages = isUndefined == true ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : DEPTH_TESTS | shaders,
        .DestinationStages = isAttachmentUse == true ? DEPTH_TESTS : shaders,
        .Image =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = isUndefined == true ? 0U
                             : previousLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 ? VK_ACCESS_SHADER_READ_BIT
                                 : DEPTH_ACCESS,
            .dstAccessMask = isAttachmentUse == true ? DEPTH_ACCESS : VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = previousLayout,
            .newLayout = isAttachmentUse == true ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = subresources,
        },
    };
}
} // namespace ludus::graphics::rhi::backend
