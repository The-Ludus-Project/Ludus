#pragma once

#include "resources.h"
#include <ludus/foundation/containers/static_array.hpp>
// Included only by rhi_vulkan.cpp, after the shared VMA allocator implementation.
// Thanks to Khronos, "Ray Tracing" (Vulkan Guide) and "Synchronization Examples"
// (acceleration-structure builds and trace reads), for independently negotiated
// static/query features and the serialized build/read dependency sequence:
// https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
// R16b follows the Vulkan specification, "Acceleration Structures", for update-mode builds
// from a separate source, compacted-size queries and compacting copies:
// https://docs.vulkan.org/spec/latest/chapters/accelstructures.html
// VMA owns native buffer allocations; each ray incarnation stays in RHI's registry.
namespace ludus::graphics::rhi::backend
{
namespace
{
struct RayVulkanBuffer final
{
    VkBuffer Object = VK_NULL_HANDLE;
    VmaAllocation Allocation = nullptr;
    void* Mapped = nullptr;
    usize Size = 0;
};
struct RayVulkanStructure final
{
    VkAccelerationStructureKHR Object = VK_NULL_HANDLE;
    RayVulkanBuffer Storage, Inputs, Indices, Opacity, Scratch;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    /// R16b: one compacted-size query written by the build of compaction-enabled geometry.
    VkQueryPool CompactedSizeOrNull = VK_NULL_HANDLE;
};
struct RayVulkanProgram final
{
    VkShaderModule Shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout Bindings = VK_NULL_HANDLE;
    VkPipelineLayout Layout = VK_NULL_HANDLE;
    VkPipeline Pipeline = VK_NULL_HANDLE;
};
struct RayVulkanBatch final
{
    RayVulkanBuffer Inputs, Outputs, Parameters;
    VkDescriptorPool Pool = VK_NULL_HANDLE;
    VkDescriptorSet Set = VK_NULL_HANDLE;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    usize Count = 0;
};
/// Acceleration structures, ray programs and batches of one device owner (R14c).
struct RayVulkanState final
{
    internal::StorageTable<RayVulkanStructure> Geometry;
    internal::StorageTable<RayVulkanStructure> Scenes;
    internal::StorageTable<RayVulkanProgram> Programs;
    internal::StorageTable<RayVulkanBatch> Batches;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<RayVulkanState, MAX_DEVICE_OWNERS> gRayVulkanStates{};
/// RayVulkanState of the bound owner slot.
RayVulkanState& GetRayVulkan() noexcept
{
    return gRayVulkanStates[internal::GetBoundOwnerSlot()];
}
void RayVulkanDropBuffer(RayVulkanBuffer& buffer) noexcept
{
    if (buffer.Object != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(GetRasterVulkan().Allocator, buffer.Object, buffer.Allocation);
    }
    buffer = {};
}
VkResult RayVulkanBufferCreate(usize size,
                               VkBufferUsageFlags usage,
                               bool mapped,
                               const void* input,
                               RayVulkanBuffer& buffer,
                               usize alignment = 1) noexcept
{
    auto result = RasterAllocator();
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkBufferCreateInfo info{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (mapped)
    {
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo details{};
    result = vmaCreateBufferWithAlignment(GetRasterVulkan().Allocator,
                                          &info,
                                          &allocation,
                                          alignment,
                                          &buffer.Object,
                                          &buffer.Allocation,
                                          &details);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    buffer.Size = size;
    buffer.Mapped = details.pMappedData;
    if (mapped && buffer.Mapped == nullptr)
    {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (input != nullptr)
    {
        std::memcpy(buffer.Mapped, input, size);
        return vmaFlushAllocation(GetRasterVulkan().Allocator, buffer.Allocation, 0, size);
    }
    return VK_SUCCESS;
}
VkDeviceAddress RayVulkanAddress(const RayVulkanBuffer& buffer) noexcept
{
    VkBufferDeviceAddressInfo info{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.Object };
    return vkGetBufferDeviceAddress(GetVulkan().Device, &info);
}
VkResult RayVulkanBegin(VkCommandBuffer& command, VkFence& fence) noexcept
{
    VkCommandBufferAllocateInfo allocate
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = GetVulkan().Commands,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult result = vkAllocateCommandBuffers(GetVulkan().Device, &allocate, &command);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkFenceCreateInfo create{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    result = vkCreateFence(GetVulkan().Device, &create, nullptr, &fence);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkCommandBufferBeginInfo begin
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    return vkBeginCommandBuffer(command, &begin);
}
VkResult RayVulkanSubmit(VkCommandBuffer command, VkFence fence) noexcept
{
    auto result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkSubmitInfo submit{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command };
    return vkQueueSubmit(GetVulkan().Queue, 1, &submit, fence);
}
struct RayVulkanDependency final
{
    VkPipelineStageFlags SourceStage = 0, DestinationStage = 0;
    VkAccessFlags SourceAccess = 0, DestinationAccess = 0;
};
void RayVulkanBarrier(VkCommandBuffer command, const RayVulkanDependency& dependency) noexcept
{
    VkMemoryBarrier barrier
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = dependency.SourceAccess,
        .dstAccessMask = dependency.DestinationAccess,
    };
    vkCmdPipelineBarrier(command,
                         dependency.SourceStage,
                         dependency.DestinationStage,
                         0,
                         1,
                         &barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
}
void RayVulkanDropStructure(RayVulkanStructure& structure) noexcept
{
    if (structure.Object != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(GetVulkan().Device, structure.Object, nullptr);
    }
    if (structure.Command != VK_NULL_HANDLE)
    {
        vkFreeCommandBuffers(GetVulkan().Device, GetVulkan().Commands, 1, &structure.Command);
    }
    if (structure.Fence != VK_NULL_HANDLE)
    {
        vkDestroyFence(GetVulkan().Device, structure.Fence, nullptr);
    }
    if (structure.CompactedSizeOrNull != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(GetVulkan().Device, structure.CompactedSizeOrNull, nullptr);
    }
    RayVulkanDropBuffer(structure.Storage);
    RayVulkanDropBuffer(structure.Inputs);
    RayVulkanDropBuffer(structure.Indices);
    RayVulkanDropBuffer(structure.Opacity);
    RayVulkanDropBuffer(structure.Scratch);
    structure = {};
}
constexpr usize RAY_GEOMETRY_RANGE_COUNT = 2;
struct RayVulkanBuildDescription final
{
    VkAccelerationStructureTypeKHR Type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    const VkAccelerationStructureGeometryKHR* Geometry = nullptr;
    uint32 PrimitiveCount = 0;
    const VkAccelerationStructureGeometryKHR* SecondGeometryOrNull = nullptr;
    uint32 SecondPrimitiveCount = 0;
    usize InputBytes = 0, ByteBudget = 0;
    /// R16b: Ready update-enabled source of an update-mode build; null builds from scratch.
    VkAccelerationStructureKHR SourceOrNull = VK_NULL_HANDLE;
    bool ShouldAllowUpdate = false;
    bool ShouldAllowCompaction = false;
};
/// R16b: native build flags of a description's update and compaction options.
VkBuildAccelerationStructureFlagsKHR GetRayVulkanBuildFlags(bool shouldAllowUpdate, bool shouldAllowCompaction) noexcept
{
    VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (shouldAllowUpdate == true)
    {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    if (shouldAllowCompaction == true)
    {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }
    return flags;
}
/// Make prior acceleration-structure writes and host inputs visible to a build, refit or copy.
constexpr RayVulkanDependency RAY_VULKAN_BUILD_INPUTS
{
    .SourceStage = VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
    .DestinationStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
    .SourceAccess = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    .DestinationAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
};
/// Make a finished build or copy visible to later builds, size queries and compute queries.
constexpr RayVulkanDependency RAY_VULKAN_BUILD_OUTPUTS
{
    .SourceStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
    .DestinationStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    .SourceAccess = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    .DestinationAccess = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
};
RasterStatus RayVulkanBuild(RayVulkanStructure& structure,
                            const RayVulkanBuildDescription& description,
                            RayAllocation& allocation) noexcept
{
    const auto type = description.Type;
    const StaticArray<uint32, RAY_GEOMETRY_RANGE_COUNT> primitives{
        {description.PrimitiveCount, description.SecondPrimitiveCount}};
    StaticArray<VkAccelerationStructureGeometryKHR, RAY_GEOMETRY_RANGE_COUNT> geometries{};
    geometries[0] = *description.Geometry;
    const uint32 geometryCount = description.SecondGeometryOrNull != nullptr ? 2 : 1;
    if (description.SecondGeometryOrNull != nullptr)
    {
        geometries[1] = *description.SecondGeometryOrNull;
    }
    const auto inputs = description.InputBytes, budget = description.ByteBudget;

    if (geometryCount > GetVulkan().RayProperties.maxGeometryCount)
    {
        return RasterStatus::CapacityExceeded;
    }
    VkAccelerationStructureBuildGeometryInfoKHR build
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = type,
        .flags = GetRayVulkanBuildFlags(description.ShouldAllowUpdate, description.ShouldAllowCompaction),
        .mode = description.SourceOrNull != VK_NULL_HANDLE ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .srcAccelerationStructure = description.SourceOrNull,
        .geometryCount = geometryCount,
        .pGeometries = geometries.GetData(),
    };
    VkAccelerationStructureBuildSizesInfoKHR sizes
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    vkGetAccelerationStructureBuildSizesKHR(GetVulkan().Device,
                                            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &build,
                                            primitives.GetData(),
                                            &sizes);
    if (description.SourceOrNull != VK_NULL_HANDLE)
    {
        sizes.buildScratchSize = sizes.updateScratchSize;
    }
    const auto limit = RayLimits().MaxBufferBytes;
    if (sizes.accelerationStructureSize == 0 || sizes.accelerationStructureSize > limit ||
        sizes.buildScratchSize > limit || inputs > budget || sizes.accelerationStructureSize > budget - inputs ||
        sizes.buildScratchSize > budget - inputs - sizes.accelerationStructureSize)
    {
        return RasterStatus::CapacityExceeded;
    }
    if (description.ShouldAllowCompaction == true)
    {
        const VkQueryPoolCreateInfo query
        {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
            .queryCount = 1,
        };
        const VkResult created = vkCreateQueryPool(GetVulkan().Device, &query, nullptr, &structure.CompactedSizeOrNull);
        if (created != VK_SUCCESS)
        {
            return RasterError(created);
        }
    }
    auto result = RayVulkanBufferCreate(static_cast<usize>(sizes.accelerationStructureSize),
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                        false,
                                        nullptr,
                                        structure.Storage);
    if (result == VK_SUCCESS)
    {
        result = RayVulkanBufferCreate(static_cast<usize>(sizes.buildScratchSize),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       false,
                                       nullptr,
                                       structure.Scratch,
                                       GetVulkan().RayProperties.minAccelerationStructureScratchOffsetAlignment);
    }
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    VkAccelerationStructureCreateInfoKHR create
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = structure.Storage.Object,
        .size = sizes.accelerationStructureSize,
        .type = type,
    };
    result = vkCreateAccelerationStructureKHR(GetVulkan().Device, &create, nullptr, &structure.Object);
    if (result == VK_SUCCESS)
    {
        result = RayVulkanBegin(structure.Command, structure.Fence);
    }
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    build.dstAccelerationStructure = structure.Object;
    build.scratchData.deviceAddress = RayVulkanAddress(structure.Scratch);
    StaticArray<VkAccelerationStructureBuildRangeInfoKHR, RAY_GEOMETRY_RANGE_COUNT> rangesStorage{};
    rangesStorage[0].primitiveCount = primitives[0];
    rangesStorage[1].primitiveCount = primitives[1];
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = rangesStorage.GetData();
    if (structure.CompactedSizeOrNull != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(structure.Command, structure.CompactedSizeOrNull, 0, 1);
    }
    // An update reads its source, written by an earlier submission on this queue.
    RayVulkanBarrier(structure.Command, RAY_VULKAN_BUILD_INPUTS);
    vkCmdBuildAccelerationStructuresKHR(structure.Command, 1, &build, &ranges);
    RayVulkanBarrier(structure.Command, RAY_VULKAN_BUILD_OUTPUTS);
    if (structure.CompactedSizeOrNull != VK_NULL_HANDLE)
    {
        vkCmdWriteAccelerationStructuresPropertiesKHR(structure.Command,
                                                      1,
                                                      &structure.Object,
                                                      VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                      structure.CompactedSizeOrNull,
                                                      0);
    }
    result = RayVulkanSubmit(structure.Command, structure.Fence);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    allocation = {inputs + static_cast<usize>(sizes.accelerationStructureSize),
                  static_cast<usize>(sizes.buildScratchSize)};
    return RasterStatus::Pending;
}
} // namespace
RayCapabilities RayLimits() noexcept
{
    if (GetVulkan().IsRayEnabled == false || GetVulkan().Device == VK_NULL_HANDLE ||
        GetVulkan().HasQueueCompute == false)
    {
        return {};
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(GetVulkan().Physical, &properties);
    if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
        properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
    {
        return {};
    }
    const auto bufferLimit = ComputeLimits().MaxStorageRange;
    const auto requested = internal::CurrentStorageLimits().RayInstances;
    const uint32 supported = GetVulkan().RayProperties.maxInstanceCount < requested
                                 ? static_cast<uint32>(GetVulkan().RayProperties.maxInstanceCount)
                                 : requested;
    // Vulkan instanceCustomIndex is a 24-bit field; query results use that identity.
    return
    {
        .StaticTriangles = true,
        .InlineQueries = true,
        .Updates = true,
        .CanCompact = true,
        .MaxInstances = supported < 0x01000000U ? supported : 0x01000000U,
        .MaxRays = 4096,
        .MaxBufferBytes = bufferLimit,
    };
}
VkAccelerationStructureKHR GetRayVulkanScene(usize slot) noexcept
{
    return GetRayVulkan().Scenes[slot].Object;
}
RasterStatus RayCreateProgram(usize slot, const ShaderDescription& description) noexcept
{
    if (GetRayVulkan().Programs.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    constexpr usize ENTRY_CAPACITY = 64;
    if (description.Spirv.GetSize() < internal::SPIRV_HEADER_WORDS || description.Spirv[0] != internal::SPIRV_MAGIC ||
        description.SpirvEntry.empty() || description.SpirvEntry.size() >= ENTRY_CAPACITY)
    {
        return RasterStatus::InvalidDescription;
    }
    RayVulkanProgram& program = GetRayVulkan().Programs[slot];
    VkShaderModuleCreateInfo shader
    {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (description.Spirv.GetSize() * sizeof(ludus::foundation::uint32)),
        .pCode = description.Spirv.GetDataOrNull(),
    };
    VkResult result = vkCreateShaderModule(GetVulkan().Device, &shader, nullptr, &program.Shader);
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32 i = 0; i < 5; ++i)
    {
        bindings[i] =
        {
            .binding = i,
            .descriptorType = i == 0   ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                              : i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo bindingInfo
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5,
        .pBindings = bindings,
    };
    if (result == VK_SUCCESS)
    {
        result = vkCreateDescriptorSetLayout(GetVulkan().Device, &bindingInfo, nullptr, &program.Bindings);
    }
    VkPipelineLayoutCreateInfo layout
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &program.Bindings,
    };
    if (result == VK_SUCCESS)
    {
        result = vkCreatePipelineLayout(GetVulkan().Device, &layout, nullptr, &program.Layout);
    }
    char entry[ENTRY_CAPACITY]{};
    std::memcpy(entry, description.SpirvEntry.data(), description.SpirvEntry.size());
    VkComputePipelineCreateInfo pipeline
    {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage =
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = program.Shader,
            .pName = entry,
        },
        .layout = program.Layout,
    };
    if (result == VK_SUCCESS)
    {
        result = vkCreateComputePipelines(GetVulkan().Device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &program.Pipeline);
    }
    return RasterError(result);
}
namespace
{
/// Build geometry, or update from the Ready source structure when it is not null.
RasterStatus RayVulkanBuildGeometry(usize slot,
                                    VkAccelerationStructureKHR sourceOrNull,
                                    const RayGeometryDescription& description,
                                    RayAllocation& outAllocation) noexcept
{
    if (GetRayVulkan().Geometry.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto& d = description;
    constexpr usize VERTEX_POSITION_FLOATS = 3;
    constexpr usize TRIANGLE_INDICES = 3;
    const usize vertexBytes = d.VertexCount * VERTEX_POSITION_FLOATS * sizeof(float32);
    const usize indexBytes = d.IndexCount * sizeof(uint32);
    if (d.IndexCount / TRIANGLE_INDICES > GetVulkan().RayProperties.maxPrimitiveCount || vertexBytes > d.ByteBudget ||
        indexBytes > d.ByteBudget - vertexBytes)
    {
        return RasterStatus::CapacityExceeded;
    }
    RayVulkanStructure& geometry = GetRayVulkan().Geometry[slot];
    const auto usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto result = RayVulkanBufferCreate(vertexBytes, usage, true, d.Positions, geometry.Inputs);
    if (result == VK_SUCCESS)
    {
        result = RayVulkanBufferCreate(indexBytes, usage, true, d.Indices, geometry.Indices);
    }
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    VkAccelerationStructureGeometryKHR native
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
    };
    native.geometry.triangles =
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .vertexData = { .deviceAddress = RayVulkanAddress(geometry.Inputs) },
        .vertexStride = 3 * sizeof(float32),
        .maxVertex = static_cast<uint32>(d.VertexCount - 1),
        .indexType = VK_INDEX_TYPE_UINT32,
        .indexData = { .deviceAddress = RayVulkanAddress(geometry.Indices) },
    };
    constexpr usize TRIANGLE_CORNER_COUNT = 3;
    const uint32 triangleCount = static_cast<uint32>(d.IndexCount / TRIANGLE_CORNER_COUNT);
    VkAccelerationStructureGeometryKHR masked = native;
    if (d.OpaqueTriangleCount != 0)
    {
        native.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        masked.geometry.triangles.indexData.deviceAddress +=
            usize(d.OpaqueTriangleCount) * TRIANGLE_CORNER_COUNT * sizeof(uint32);
    }
    const bool hasMaskedSuffix = d.OpaqueTriangleCount != 0 && d.OpaqueTriangleCount < triangleCount;
    return RayVulkanBuild(geometry,
                          {
                              .Type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                              .Geometry = &native,
                              .PrimitiveCount = d.OpaqueTriangleCount != 0 ? d.OpaqueTriangleCount : triangleCount,
                              .SecondGeometryOrNull = hasMaskedSuffix == true ? &masked : nullptr,
                              .SecondPrimitiveCount = triangleCount - d.OpaqueTriangleCount,
                              .InputBytes = vertexBytes + indexBytes,
                              .ByteBudget = d.ByteBudget,
                              .SourceOrNull = sourceOrNull,
                              .ShouldAllowUpdate = d.ShouldAllowUpdate,
                              .ShouldAllowCompaction = d.ShouldAllowCompaction,
                          },
                          outAllocation);
}
} // namespace
RasterStatus
RayCreateGeometry(usize slot, const RayGeometryDescription& description, RayAllocation& allocation) noexcept
{
    return RayVulkanBuildGeometry(slot, VK_NULL_HANDLE, description, allocation);
}
RasterStatus RayUpdateGeometry(usize slot,
                               usize source,
                               const RayGeometryDescription& description,
                               RayAllocation& outAllocation) noexcept
{
    return RayVulkanBuildGeometry(slot, GetRayVulkan().Geometry[source].Object, description, outAllocation);
}
RasterStatus
RayCompactGeometry(usize slot, const internal::NativeRayCompaction& compaction, RayAllocation& outAllocation) noexcept
{
    const usize source = compaction.Source;
    const usize byteBudget = compaction.ByteBudget;
    if (GetRayVulkan().Geometry.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const RayVulkanStructure& from = GetRayVulkan().Geometry[source];
    if (from.CompactedSizeOrNull == VK_NULL_HANDLE)
    {
        return RasterStatus::Failed;
    }
    // The source fence has signaled, so the query result is available without waiting.
    uint64 compactedBytes = 0;
    VkResult result = vkGetQueryPoolResults(GetVulkan().Device,
                                            from.CompactedSizeOrNull,
                                            0,
                                            1,
                                            sizeof(compactedBytes),
                                            &compactedBytes,
                                            sizeof(compactedBytes),
                                            VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
    {
        return result == VK_NOT_READY ? RasterStatus::Failed : RasterError(result);
    }
    if (compactedBytes == 0 || compactedBytes > from.Storage.Size)
    {
        return RasterStatus::Failed;
    }
    if (compactedBytes > byteBudget || compactedBytes > RayLimits().MaxBufferBytes)
    {
        return RasterStatus::CapacityExceeded;
    }
    RayVulkanStructure& geometry = GetRayVulkan().Geometry[slot];
    result = RayVulkanBufferCreate(static_cast<usize>(compactedBytes),
                                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                   false,
                                   nullptr,
                                   geometry.Storage);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    const VkAccelerationStructureCreateInfoKHR create
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = geometry.Storage.Object,
        .size = compactedBytes,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    };
    result = vkCreateAccelerationStructureKHR(GetVulkan().Device, &create, nullptr, &geometry.Object);
    if (result == VK_SUCCESS)
    {
        result = RayVulkanBegin(geometry.Command, geometry.Fence);
    }
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    const VkCopyAccelerationStructureInfoKHR copy
    {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
        .src = from.Object,
        .dst = geometry.Object,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
    };
    RayVulkanBarrier(geometry.Command, RAY_VULKAN_BUILD_INPUTS);
    vkCmdCopyAccelerationStructureKHR(geometry.Command, &copy);
    RayVulkanBarrier(geometry.Command, RAY_VULKAN_BUILD_OUTPUTS);
    result = RayVulkanSubmit(geometry.Command, geometry.Fence);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    outAllocation = {static_cast<usize>(compactedBytes), 0};
    return RasterStatus::Pending;
}
namespace
{
/// Build a scene, or update from the Ready source structure when it is not null.
RasterStatus RayVulkanBuildScene(usize slot,
                                 VkAccelerationStructureKHR sourceOrNull,
                                 const internal::NativeRaySceneDescription& description,
                                 RayAllocation& outAllocation) noexcept
{
    if (GetRayVulkan().Scenes.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto* input = description.Instances;
    const auto* opacity = description.Opacity;
    const auto count = description.Count, scalarCount = description.ScalarCount, budget = description.ByteBudget;

    const usize instanceBytes = (count == 0 ? usize{1} : count) * sizeof(VkAccelerationStructureInstanceKHR);
    const usize opacityBytes = scalarCount * sizeof(float32);
    if (instanceBytes > budget || opacityBytes > budget - instanceBytes)
    {
        return RasterStatus::CapacityExceeded;
    }
    RayVulkanStructure& scene = GetRayVulkan().Scenes[slot];
    auto result = RayVulkanBufferCreate(instanceBytes,
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        true,
                                        nullptr,
                                        scene.Inputs);
    if (result == VK_SUCCESS)
    {
        result = RayVulkanBufferCreate(opacityBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, opacity, scene.Opacity);
    }
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    auto* instances = static_cast<VkAccelerationStructureInstanceKHR*>(scene.Inputs.Mapped);
    for (usize i = 0; i < count; ++i)
    {
        auto& instance = instances[i];
        instance = {};
        std::memcpy(&instance.transform, input[i].Transform, sizeof(instance.transform));
        instance.instanceCustomIndex = static_cast<uint32>(i);
        instance.mask = input[i].Mask;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                         (input[i].IsOpaque == true ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : 0);
        VkAccelerationStructureDeviceAddressInfoKHR address
        {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = GetRayVulkan().Geometry[input[i].Geometry].Object,
        };
        instance.accelerationStructureReference =
            vkGetAccelerationStructureDeviceAddressKHR(GetVulkan().Device, &address);
    }
    result = vmaFlushAllocation(GetRasterVulkan().Allocator, scene.Inputs.Allocation, 0, instanceBytes);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    VkAccelerationStructureGeometryKHR native
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
    };
    native.geometry.instances =
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .data = { .deviceAddress = RayVulkanAddress(scene.Inputs) },
    };
    return RayVulkanBuild(scene,
                          {
                              .Type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                              .Geometry = &native,
                              .PrimitiveCount = static_cast<uint32>(count),
                              .InputBytes = instanceBytes + opacityBytes,
                              .ByteBudget = budget,
                              .SourceOrNull = sourceOrNull,
                              .ShouldAllowUpdate = description.ShouldAllowUpdate,
                          },
                          outAllocation);
}
} // namespace
RasterStatus
RayCreateScene(usize slot, const internal::NativeRaySceneDescription& description, RayAllocation& allocation) noexcept
{
    return RayVulkanBuildScene(slot, VK_NULL_HANDLE, description, allocation);
}
RasterStatus RayUpdateScene(usize slot,
                            usize source,
                            const internal::NativeRaySceneDescription& description,
                            RayAllocation& outAllocation) noexcept
{
    return RayVulkanBuildScene(slot, GetRayVulkan().Scenes[source].Object, description, outAllocation);
}
RasterStatus RaySubmit(usize slot, const internal::NativeRaySubmission& submission, RayAllocation& allocation) noexcept
{
    if (GetRayVulkan().Batches.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto programSlot = submission.Program, sceneSlot = submission.Scene, count = submission.Count;
    const auto* rays = submission.Rays;

    RayVulkanBatch& batch = GetRayVulkan().Batches[slot];
    const RayVulkanProgram& program = GetRayVulkan().Programs[programSlot];
    const RayVulkanStructure& scene = GetRayVulkan().Scenes[sceneSlot];
    batch.Count = count;
    const uint32 parameters[4]{static_cast<uint32>(count), 0, 0, 0};
    auto result =
        RayVulkanBufferCreate(count * sizeof(RayInput), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, rays, batch.Inputs);
    if (result == VK_SUCCESS)
    {
        result = RayVulkanBufferCreate(count * sizeof(RayResult),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       true,
                                       nullptr,
                                       batch.Outputs);
    }
    if (result == VK_SUCCESS)
    {
        result = RayVulkanBufferCreate(sizeof(parameters),
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       true,
                                       parameters,
                                       batch.Parameters);
    }
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 3,
        .pPoolSizes = sizes,
    };
    if (result == VK_SUCCESS)
    {
        result = vkCreateDescriptorPool(GetVulkan().Device, &pool, nullptr, &batch.Pool);
    }
    VkDescriptorSetAllocateInfo allocate
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = batch.Pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &program.Bindings,
    };
    if (result == VK_SUCCESS)
    {
        result = vkAllocateDescriptorSets(GetVulkan().Device, &allocate, &batch.Set);
    }
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    VkWriteDescriptorSetAccelerationStructureKHR structure
    {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &scene.Object,
    };
    const VkDescriptorBufferInfo buffers[]{{batch.Inputs.Object, 0, batch.Inputs.Size},
                                           {batch.Outputs.Object, 0, batch.Outputs.Size},
                                           {scene.Opacity.Object, 0, scene.Opacity.Size},
                                           {batch.Parameters.Object, 0, batch.Parameters.Size}};
    // The scene, then the input, output and parameter buffers.
    constexpr uint32 BATCH_DESCRIPTOR_WRITES = 5;
    foundation::StaticArray<VkWriteDescriptorSet, BATCH_DESCRIPTOR_WRITES> writes{};
    for (uint32 i = 0; i < BATCH_DESCRIPTOR_WRITES; ++i)
    {
        writes[i] =
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = i == 0 ? &structure : nullptr,
            .dstSet = batch.Set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i == 0   ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                              : i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = i == 0 ? nullptr : &buffers[i - 1],
        };
    }
    vkUpdateDescriptorSets(GetVulkan().Device, BATCH_DESCRIPTOR_WRITES, writes.GetData(), 0, nullptr);
    result = RayVulkanBegin(batch.Command, batch.Fence);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    RayVulkanBarrier(
        batch.Command,
        {
            .SourceStage = VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .DestinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .SourceAccess = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .DestinationAccess =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_UNIFORM_READ_BIT,
        });
    vkCmdBindPipeline(batch.Command, VK_PIPELINE_BIND_POINT_COMPUTE, program.Pipeline);
    vkCmdBindDescriptorSets(batch.Command,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            program.Layout,
                            0,
                            1,
                            &batch.Set,
                            0,
                            nullptr);
    vkCmdDispatch(batch.Command, static_cast<uint32>((count + 63) / 64), 1, 1);
    RayVulkanBarrier(batch.Command,
                     {
                         .SourceStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         .DestinationStage = VK_PIPELINE_STAGE_HOST_BIT,
                         .SourceAccess = VK_ACCESS_SHADER_WRITE_BIT,
                         .DestinationAccess = VK_ACCESS_HOST_READ_BIT,
                     });
    result = RayVulkanSubmit(batch.Command, batch.Fence);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    allocation = {count * (sizeof(RayInput) + sizeof(RayResult)) + sizeof(parameters), 0};
    return RasterStatus::Pending;
}
RasterStatus RayPoll(internal::RasterKind kind, usize slot) noexcept
{
    if (kind == internal::RasterKind::RayProgram)
    {
        return GetRayVulkan().Programs[slot].Pipeline == VK_NULL_HANDLE ? RasterStatus::Failed : RasterStatus::Ready;
    }
    VkFence fence = kind == internal::RasterKind::RayBatch      ? GetRayVulkan().Batches[slot].Fence
                    : kind == internal::RasterKind::RayGeometry ? GetRayVulkan().Geometry[slot].Fence
                                                                : GetRayVulkan().Scenes[slot].Fence;
    if (fence == VK_NULL_HANDLE)
    {
        return RasterStatus::Failed;
    }
    const VkResult result = vkGetFenceStatus(GetVulkan().Device, fence);
    if (result == VK_NOT_READY)
    {
        return RasterStatus::Pending;
    }
    if (result == VK_SUCCESS && kind != internal::RasterKind::RayBatch)
    {
        RayVulkanStructure& structure =
            kind == internal::RasterKind::RayGeometry ? GetRayVulkan().Geometry[slot] : GetRayVulkan().Scenes[slot];
        RayVulkanDropBuffer(structure.Scratch);
    }
    return RasterError(result);
}
RasterStatus RayRead(usize slot, RayResult* output, usize count) noexcept
{
    RayVulkanBatch& batch = GetRayVulkan().Batches[slot];
    const auto ready = RayPoll(internal::RasterKind::RayBatch, slot);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    if (count != batch.Count || batch.Outputs.Mapped == nullptr)
    {
        return RasterStatus::InvalidDescription;
    }
    const VkResult result =
        vmaInvalidateAllocation(GetRasterVulkan().Allocator, batch.Outputs.Allocation, 0, batch.Outputs.Size);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    std::memcpy(output, batch.Outputs.Mapped, count * sizeof(RayResult));
    return RasterStatus::Ready;
}
void RayDestroy(internal::RasterKind kind, usize slot) noexcept
{
    if (kind == internal::RasterKind::RayGeometry && slot < GetRayVulkan().Geometry.GetSize())
    {
        RayVulkanDropStructure(GetRayVulkan().Geometry[slot]);
    }
    else if (kind == internal::RasterKind::RayScene && slot < GetRayVulkan().Scenes.GetSize())
    {
        RayVulkanDropStructure(GetRayVulkan().Scenes[slot]);
    }
    else if (kind == internal::RasterKind::RayProgram && slot < GetRayVulkan().Programs.GetSize())
    {
        RayVulkanProgram& program = GetRayVulkan().Programs[slot];
        if (program.Pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(GetVulkan().Device, program.Pipeline, nullptr);
        }
        if (program.Layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(GetVulkan().Device, program.Layout, nullptr);
        }
        if (program.Bindings != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(GetVulkan().Device, program.Bindings, nullptr);
        }
        if (program.Shader != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(GetVulkan().Device, program.Shader, nullptr);
        }
        program = {};
    }
    else if (kind == internal::RasterKind::RayBatch && slot < GetRayVulkan().Batches.GetSize())
    {
        RayVulkanBatch& batch = GetRayVulkan().Batches[slot];
        if (batch.Pool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(GetVulkan().Device, batch.Pool, nullptr);
        }
        if (batch.Command != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(GetVulkan().Device, GetVulkan().Commands, 1, &batch.Command);
        }
        if (batch.Fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(GetVulkan().Device, batch.Fence, nullptr);
        }
        RayVulkanDropBuffer(batch.Inputs);
        RayVulkanDropBuffer(batch.Outputs);
        RayVulkanDropBuffer(batch.Parameters);
        batch = {};
    }
}
void RayDrain() noexcept
{
    if (GetVulkan().Device != VK_NULL_HANDLE && GetVulkan().IsRayEnabled)
    {
        static_cast<void>(vkDeviceWaitIdle(GetVulkan().Device));
    }
}
} // namespace ludus::graphics::rhi::backend
