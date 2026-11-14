#include "internal/buffer_access_vulkan.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::usize;
using ludus::graphics::rhi::GraphAccessMode;
using ludus::graphics::rhi::backend::GetVulkanBufferAccess;
using ludus::graphics::rhi::backend::VulkanBufferAccess;

TEST_CASE("Vulkan graph buffer uses resolve to legacy synchronization scopes", "[rhi][vulkan][synchronization]")
{
    constexpr VkPipelineStageFlags RASTER_SHADERS =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    constexpr VkPipelineStageFlags RASTER_AND_COMPUTE = RASTER_SHADERS | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags STORAGE_WRITERS =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    struct ExpectedAccess final
    {
        GraphAccessMode Use;
        bool HasComputeQueue;
        VulkanBufferAccess Scope;
    };
    constexpr usize CASE_COUNT = 11;
    constexpr StaticArray<ExpectedAccess, CASE_COUNT> CASES{{
        {GraphAccessMode::Vertex, false, {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, 0}},
        {GraphAccessMode::Index, false, {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT, 0}},
        {GraphAccessMode::Uniform, false, {RASTER_SHADERS, VK_ACCESS_UNIFORM_READ_BIT, 0}},
        {GraphAccessMode::Uniform, true, {RASTER_AND_COMPUTE, VK_ACCESS_UNIFORM_READ_BIT, 0}},
        {GraphAccessMode::Indirect,
         true,
         {VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, 0}},
        {GraphAccessMode::StorageRead, true, {RASTER_AND_COMPUTE, VK_ACCESS_SHADER_READ_BIT, 0}},
        {GraphAccessMode::StorageReadWrite,
         true,
         {STORAGE_WRITERS, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT}},
        {GraphAccessMode::Sampled, true, {}},
        {GraphAccessMode::ColorWrite, true, {}},
        {GraphAccessMode::ColorReadWrite, true, {}},
        {GraphAccessMode::StorageTextureWrite, true, {}},
    }};
    for (const ExpectedAccess& expected : CASES)
    {
        CAPTURE(static_cast<uint32>(expected.Use), expected.HasComputeQueue);
        const VulkanBufferAccess actual = GetVulkanBufferAccess(expected.Use, expected.HasComputeQueue);
        CHECK(actual.Stages == expected.Scope.Stages);
        CHECK(actual.ReadAccess == expected.Scope.ReadAccess);
        CHECK(actual.WriteAccess == expected.Scope.WriteAccess);
    }
}
