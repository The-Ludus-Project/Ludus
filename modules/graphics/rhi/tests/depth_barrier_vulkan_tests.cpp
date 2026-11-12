#include "internal/depth_barrier_vulkan.h"

#include <catch2/catch_test_macros.hpp>

using ludus::graphics::rhi::backend::MakeVulkanDepthBarrier;
using ludus::graphics::rhi::backend::VulkanDepthBarrier;

TEST_CASE("Vulkan depth sampling and reuse respect graphics queue capabilities", "[rhi][vulkan][synchronization]")
{
    constexpr VkImageSubresourceRange DEPTH_STENCIL{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                                    0,
                                                    1,
                                                    0,
                                                    1};
    constexpr VkPipelineStageFlags DEPTH_TESTS =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkPipelineStageFlags GRAPHICS_SHADERS =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    const VulkanDepthBarrier initial =
        MakeVulkanDepthBarrier(VK_NULL_HANDLE, DEPTH_STENCIL, VK_IMAGE_LAYOUT_UNDEFINED, true, false);
    CHECK(initial.SourceStages == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    CHECK(initial.Image.srcAccessMask == 0);
    CHECK(initial.Image.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    const VulkanDepthBarrier sampled =
        MakeVulkanDepthBarrier(VK_NULL_HANDLE, DEPTH_STENCIL, initial.Image.newLayout, false, false);
    CHECK(sampled.SourceStages == (DEPTH_TESTS | GRAPHICS_SHADERS));
    CHECK(sampled.DestinationStages == GRAPHICS_SHADERS);
    CHECK((sampled.Image.srcAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);
    CHECK(sampled.Image.dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
    CHECK(sampled.Image.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const VulkanDepthBarrier rewritten =
        MakeVulkanDepthBarrier(VK_NULL_HANDLE, DEPTH_STENCIL, sampled.Image.newLayout, true, false);
    CHECK(rewritten.SourceStages == (DEPTH_TESTS | GRAPHICS_SHADERS));
    CHECK(rewritten.DestinationStages == DEPTH_TESTS);
    CHECK((rewritten.Image.dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);
    CHECK(rewritten.Image.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    CHECK(rewritten.Image.subresourceRange.aspectMask == DEPTH_STENCIL.aspectMask);

    const VulkanDepthBarrier reused =
        MakeVulkanDepthBarrier(VK_NULL_HANDLE, DEPTH_STENCIL, rewritten.Image.newLayout, true, false);
    CHECK(reused.Image.oldLayout == reused.Image.newLayout);
    CHECK((reused.Image.srcAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);
    CHECK((reused.Image.dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0);

    const VulkanDepthBarrier sampledWithCompute =
        MakeVulkanDepthBarrier(VK_NULL_HANDLE, DEPTH_STENCIL, initial.Image.newLayout, false, true);
    CHECK(sampledWithCompute.DestinationStages == (GRAPHICS_SHADERS | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT));

    const VulkanDepthBarrier rewrittenAfterCompute =
        MakeVulkanDepthBarrier(VK_NULL_HANDLE, DEPTH_STENCIL, sampledWithCompute.Image.newLayout, true, true);
    CHECK((rewrittenAfterCompute.SourceStages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) != 0);
    CHECK(rewrittenAfterCompute.DestinationStages == DEPTH_TESTS);
}
