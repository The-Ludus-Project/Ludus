#pragma once

#include <ludus/foundation/base/compiler.h>

#include <ludus/graphics/rhi/graph.h>

#include <vulkan/vulkan_core.h>

namespace ludus::graphics::rhi::backend
{
struct VulkanBufferAccess final
{
    VkPipelineStageFlags Stages = 0;
    VkAccessFlags ReadAccess = 0;
    VkAccessFlags WriteAccess = 0;
};

// Thanks to Tobias Hector, "Simplified Vulkan Synchronization", alpha.9, thsvsGetAccessInfo.
// https://github.com/Tobski/simple_vulkan_synchronization/blob/a4c7ede63d29e2612d5ae1545d3b507c506fc5ed/thsvs_simpler_vulkan_synchronization.h#L866-L899
// Translate Ludus graph uses to Vulkan stage and access masks. The encoder owns prior uses,
// writer history and barrier recording. Texture-only uses have no buffer scope.
LUDUS_INLINE constexpr VulkanBufferAccess GetVulkanBufferAccess(GraphAccessMode use, bool hasComputeQueue) noexcept
{
    constexpr VkPipelineStageFlags RASTER_SHADER_STAGES =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    switch (use)
    {
        case GraphAccessMode::Vertex:
            return {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, 0};
        case GraphAccessMode::Index:
            return {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT, 0};
        case GraphAccessMode::Uniform:
            return {RASTER_SHADER_STAGES | (hasComputeQueue == true ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0U),
                    VK_ACCESS_UNIFORM_READ_BIT,
                    0};
        case GraphAccessMode::Indirect:
            return {VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, 0};
        // Storage uses here carry no stage. Include all shader stages admitted for the use.
        case GraphAccessMode::StorageRead:
            return {RASTER_SHADER_STAGES | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0};
        case GraphAccessMode::StorageReadWrite:
            return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT};
        case GraphAccessMode::Sampled:
        case GraphAccessMode::ColorWrite:
        case GraphAccessMode::ColorReadWrite:
        case GraphAccessMode::StorageTextureWrite:
            break;
    }
    return {};
}
} // namespace ludus::graphics::rhi::backend
