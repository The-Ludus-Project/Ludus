#pragma once
// Included only by rhi_vulkan.cpp, after the shared VMA allocator implementation.
// Thanks to Khronos, "Ray Tracing" (Vulkan Guide) and "Synchronization Examples"
// (acceleration-structure builds and trace reads), for independently negotiated
// static/query features and the serialized build/read dependency sequence:
// https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
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
internal::StorageTable<RayVulkanStructure> gRayVulkanGeometry, gRayVulkanScene;
internal::StorageTable<RayVulkanProgram> gRayVulkanProgram;
internal::StorageTable<RayVulkanBatch> gRayVulkanBatch;
void RayVulkanDropBuffer(RayVulkanBuffer& buffer) noexcept
{
    if (buffer.Object != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(gRasterAllocator, buffer.Object, buffer.Allocation);
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
    result = vmaCreateBufferWithAlignment(gRasterAllocator,
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
        return vmaFlushAllocation(gRasterAllocator, buffer.Allocation, 0, size);
    }
    return VK_SUCCESS;
}
VkDeviceAddress RayVulkanAddress(const RayVulkanBuffer& buffer) noexcept
{
    VkBufferDeviceAddressInfo info{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.Object };
    return vkGetBufferDeviceAddress(gDevice, &info);
}
VkResult RayVulkanBegin(VkCommandBuffer& command, VkFence& fence) noexcept
{
    VkCommandBufferAllocateInfo allocate
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = gCommands,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    auto result = vkAllocateCommandBuffers(gDevice, &allocate, &command);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkFenceCreateInfo create{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    result = vkCreateFence(gDevice, &create, nullptr, &fence);
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
    return vkQueueSubmit(gQueue, 1, &submit, fence);
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
        vkDestroyAccelerationStructureKHR(gDevice, structure.Object, nullptr);
    }
    if (structure.Command != VK_NULL_HANDLE)
    {
        vkFreeCommandBuffers(gDevice, gCommands, 1, &structure.Command);
    }
    if (structure.Fence != VK_NULL_HANDLE)
    {
        vkDestroyFence(gDevice, structure.Fence, nullptr);
    }
    RayVulkanDropBuffer(structure.Storage);
    RayVulkanDropBuffer(structure.Inputs);
    RayVulkanDropBuffer(structure.Indices);
    RayVulkanDropBuffer(structure.Opacity);
    RayVulkanDropBuffer(structure.Scratch);
    structure = {};
}
struct RayVulkanBuildDescription final
{
    VkAccelerationStructureTypeKHR Type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    const VkAccelerationStructureGeometryKHR* Geometry = nullptr;
    uint32 PrimitiveCount = 0;
    usize InputBytes = 0, ByteBudget = 0;
};
RasterStatus RayVulkanBuild(RayVulkanStructure& structure,
                            const RayVulkanBuildDescription& description,
                            RayAllocation& allocation) noexcept
{
    const auto type = description.Type;
    const auto primitives = description.PrimitiveCount;
    const auto& geometry = *description.Geometry;
    const auto inputs = description.InputBytes, budget = description.ByteBudget;

    VkAccelerationStructureBuildGeometryInfoKHR build
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = type,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    VkAccelerationStructureBuildSizesInfoKHR sizes
    {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    vkGetAccelerationStructureBuildSizesKHR(gDevice,
                                            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &build,
                                            &primitives,
                                            &sizes);
    const auto limit = RayLimits().MaxBufferBytes;
    if (sizes.accelerationStructureSize == 0 || sizes.accelerationStructureSize > limit ||
        sizes.buildScratchSize > limit || inputs > budget || sizes.accelerationStructureSize > budget - inputs ||
        sizes.buildScratchSize > budget - inputs - sizes.accelerationStructureSize)
    {
        return RasterStatus::CapacityExceeded;
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
                                       gRayProperties.minAccelerationStructureScratchOffsetAlignment);
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
    result = vkCreateAccelerationStructureKHR(gDevice, &create, nullptr, &structure.Object);
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
    VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = primitives };
    const auto* ranges = &range;
    RayVulkanBarrier(structure.Command,
                     {
                         .SourceStage = VK_PIPELINE_STAGE_HOST_BIT,
                         .DestinationStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         .SourceAccess = VK_ACCESS_HOST_WRITE_BIT,
                         .DestinationAccess = VK_ACCESS_SHADER_READ_BIT,
                     });
    vkCmdBuildAccelerationStructuresKHR(structure.Command, 1, &build, &ranges);
    RayVulkanBarrier(structure.Command,
                     {
                         .SourceStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         .DestinationStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         .SourceAccess = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                         .DestinationAccess = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                     });
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
    if (gRayEnabled == false || gDevice == VK_NULL_HANDLE || gQueueCompute == false)
    {
        return {};
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
        properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
    {
        return {};
    }
    const auto bufferLimit = ComputeLimits().MaxStorageRange;
    const auto requested = internal::CurrentStorageLimits().RayInstances;
    const auto supported =
        gRayProperties.maxInstanceCount < requested ? static_cast<uint32>(gRayProperties.maxInstanceCount) : requested;
    // Vulkan instanceCustomIndex is a 24-bit field; query results use that identity.
    return
    {
        .StaticTriangles = true,
        .InlineQueries = true,
        .MaxInstances = supported < 0x01000000U ? supported : 0x01000000U,
        .MaxRays = 4096,
        .MaxBufferBytes = bufferLimit < usize{64} * 1024 * 1024 ? bufferLimit : usize{64} * 1024 * 1024,
    };
}
VkAccelerationStructureKHR GetRayVulkanScene(usize slot) noexcept
{
    return gRayVulkanScene[slot].Object;
}
RasterStatus RayCreateProgram(usize slot, const ShaderDescription& description) noexcept
{
    if (gRayVulkanProgram.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    if (description.Spirv.size() < 5 || description.Spirv[0] != 0x07230203 || description.SpirvEntry.empty() ||
        description.SpirvEntry.size() >= 64)
    {
        return RasterStatus::InvalidDescription;
    }
    auto& program = gRayVulkanProgram[slot];
    VkShaderModuleCreateInfo shader
    {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = description.Spirv.size_bytes(),
        .pCode = description.Spirv.data(),
    };
    auto result = vkCreateShaderModule(gDevice, &shader, nullptr, &program.Shader);
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
        result = vkCreateDescriptorSetLayout(gDevice, &bindingInfo, nullptr, &program.Bindings);
    }
    VkPipelineLayoutCreateInfo layout
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &program.Bindings,
    };
    if (result == VK_SUCCESS)
    {
        result = vkCreatePipelineLayout(gDevice, &layout, nullptr, &program.Layout);
    }
    char entry[64]{};
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
        result = vkCreateComputePipelines(gDevice, VK_NULL_HANDLE, 1, &pipeline, nullptr, &program.Pipeline);
    }
    return RasterError(result);
}
RasterStatus
RayCreateGeometry(usize slot, const RayGeometryDescription& description, RayAllocation& allocation) noexcept
{
    if (gRayVulkanGeometry.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto& d = description;
    const auto vertexBytes = d.VertexCount * 3 * sizeof(float32), indexBytes = d.IndexCount * sizeof(uint32);
    if (d.IndexCount / 3 > gRayProperties.maxPrimitiveCount || vertexBytes > d.ByteBudget ||
        indexBytes > d.ByteBudget - vertexBytes)
    {
        return RasterStatus::CapacityExceeded;
    }
    auto& geometry = gRayVulkanGeometry[slot];
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
    return RayVulkanBuild(geometry,
                          {
                              .Type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                              .Geometry = &native,
                              .PrimitiveCount = static_cast<uint32>(d.IndexCount / 3),
                              .InputBytes = vertexBytes + indexBytes,
                              .ByteBudget = d.ByteBudget,
                          },
                          allocation);
}
RasterStatus
RayCreateScene(usize slot, const internal::NativeRaySceneDescription& description, RayAllocation& allocation) noexcept
{
    if (gRayVulkanScene.EnsureSlots(slot + 1) == false)
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
    auto& scene = gRayVulkanScene[slot];
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
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR address
        {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = gRayVulkanGeometry[input[i].Geometry].Object,
        };
        instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(gDevice, &address);
    }
    result = vmaFlushAllocation(gRasterAllocator, scene.Inputs.Allocation, 0, instanceBytes);
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
                          },
                          allocation);
}
RasterStatus RaySubmit(usize slot, const internal::NativeRaySubmission& submission, RayAllocation& allocation) noexcept
{
    if (gRayVulkanBatch.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto programSlot = submission.Program, sceneSlot = submission.Scene, count = submission.Count;
    const auto* rays = submission.Rays;

    auto& batch = gRayVulkanBatch[slot];
    const auto& program = gRayVulkanProgram[programSlot];
    const auto& scene = gRayVulkanScene[sceneSlot];
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
        result = vkCreateDescriptorPool(gDevice, &pool, nullptr, &batch.Pool);
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
        result = vkAllocateDescriptorSets(gDevice, &allocate, &batch.Set);
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
    VkWriteDescriptorSet writes[5]{};
    for (uint32 i = 0; i < 5; ++i)
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
    vkUpdateDescriptorSets(gDevice, 5, writes, 0, nullptr);
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
        return gRayVulkanProgram[slot].Pipeline == VK_NULL_HANDLE ? RasterStatus::Failed : RasterStatus::Ready;
    }
    auto fence = kind == internal::RasterKind::RayBatch      ? gRayVulkanBatch[slot].Fence
                 : kind == internal::RasterKind::RayGeometry ? gRayVulkanGeometry[slot].Fence
                                                             : gRayVulkanScene[slot].Fence;
    if (fence == VK_NULL_HANDLE)
    {
        return RasterStatus::Failed;
    }
    const auto result = vkGetFenceStatus(gDevice, fence);
    if (result == VK_NOT_READY)
    {
        return RasterStatus::Pending;
    }
    if (result == VK_SUCCESS && kind != internal::RasterKind::RayBatch)
    {
        auto& structure = kind == internal::RasterKind::RayGeometry ? gRayVulkanGeometry[slot] : gRayVulkanScene[slot];
        RayVulkanDropBuffer(structure.Scratch);
    }
    return RasterError(result);
}
RasterStatus RayRead(usize slot, RayResult* output, usize count) noexcept
{
    auto& batch = gRayVulkanBatch[slot];
    const auto ready = RayPoll(internal::RasterKind::RayBatch, slot);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    if (count != batch.Count || batch.Outputs.Mapped == nullptr)
    {
        return RasterStatus::InvalidDescription;
    }
    const auto result = vmaInvalidateAllocation(gRasterAllocator, batch.Outputs.Allocation, 0, batch.Outputs.Size);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    std::memcpy(output, batch.Outputs.Mapped, count * sizeof(RayResult));
    return RasterStatus::Ready;
}
void RayDestroy(internal::RasterKind kind, usize slot) noexcept
{
    if (kind == internal::RasterKind::RayGeometry && slot < gRayVulkanGeometry.GetSize())
    {
        RayVulkanDropStructure(gRayVulkanGeometry[slot]);
    }
    else if (kind == internal::RasterKind::RayScene && slot < gRayVulkanScene.GetSize())
    {
        RayVulkanDropStructure(gRayVulkanScene[slot]);
    }
    else if (kind == internal::RasterKind::RayProgram && slot < gRayVulkanProgram.GetSize())
    {
        auto& program = gRayVulkanProgram[slot];
        if (program.Pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(gDevice, program.Pipeline, nullptr);
        }
        if (program.Layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(gDevice, program.Layout, nullptr);
        }
        if (program.Bindings != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(gDevice, program.Bindings, nullptr);
        }
        if (program.Shader != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(gDevice, program.Shader, nullptr);
        }
        program = {};
    }
    else if (kind == internal::RasterKind::RayBatch && slot < gRayVulkanBatch.GetSize())
    {
        auto& batch = gRayVulkanBatch[slot];
        if (batch.Pool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(gDevice, batch.Pool, nullptr);
        }
        if (batch.Command != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(gDevice, gCommands, 1, &batch.Command);
        }
        if (batch.Fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(gDevice, batch.Fence, nullptr);
        }
        RayVulkanDropBuffer(batch.Inputs);
        RayVulkanDropBuffer(batch.Outputs);
        RayVulkanDropBuffer(batch.Parameters);
        batch = {};
    }
}
void RayDrain() noexcept
{
    if (gDevice != VK_NULL_HANDLE && gRayEnabled)
    {
        static_cast<void>(vkDeviceWaitIdle(gDevice));
    }
}
} // namespace ludus::graphics::rhi::backend
