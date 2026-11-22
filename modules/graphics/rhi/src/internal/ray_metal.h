#pragma once
// Included only by rhi_metal.mm; the device and queue remain its private objects.
// Thanks to Apple, "Accelerating ray tracing using Metal" and "Enhance your app
// with Metal ray tracing", WWDC21 session 10149, for immutable BLAS/TLAS builds,
// candidate filtering and explicit useResource for indirectly referenced BLAS:
// https://developer.apple.com/documentation/metal/accelerating-ray-tracing-using-metal
// https://developer.apple.com/videos/play/wwdc2021/10149/
// R16b follows the same sample's compaction sequence (write the compacted size in the
// build encoder, then copyAndCompact into a structure of that size) and Apple's
// MTLAccelerationStructureCommandEncoder refit reference for out-of-place refits:
// https://developer.apple.com/documentation/metal/mtlaccelerationstructurecommandencoder
namespace ludus::graphics::rhi::backend
{
namespace
{
/// Highest ray query binding index: the acceleration structure at 0, then four buffers.
constexpr NSUInteger LAST_RAY_QUERY_BINDING_INDEX = 4;
struct RayMetalGeometry final
{
    id<MTLAccelerationStructure> Structure = nil;
    id<MTLBuffer> Vertices = nil, Indices = nil, Scratch = nil;
    id<MTLCommandBuffer> Command = nil;
    /// R16b: shared uint64 compacted size written by the build of compaction-enabled geometry.
    id<MTLBuffer> CompactedSizeOrNil = nil;
};
/// R16b: geometry or scene slot of a build that has no refit source.
constexpr usize RAY_METAL_NO_SOURCE = ~usize{0};
/// R16b: native usage of a build, refittable when the description allows updates.
MTLAccelerationStructureUsage GetRayMetalUsage(bool shouldAllowUpdate) noexcept
{
    return shouldAllowUpdate == true ? MTLAccelerationStructureUsageRefit : MTLAccelerationStructureUsageNone;
}
/// R16b: a refit can need no scratch, but Metal returns no buffer for a zero length.
/// The reported allocation keeps the native size; one byte is never read.
usize GetRayMetalScratchAllocationBytes(usize scratchBytes) noexcept
{
    return scratchBytes == 0 ? 1 : scratchBytes;
}
/// R16b: a refit destination holds at least the source structure and the descriptor's size.
usize GetRayMetalStructureBytes(id<MTLAccelerationStructure> sourceOrNil, usize descriptorBytes) noexcept
{
    return sourceOrNil != nil && sourceOrNil.size > descriptorBytes ? sourceOrNil.size : descriptorBytes;
}
struct RayMetalScene final
{
    id<MTLAccelerationStructure> Structure = nil;
    id<MTLBuffer> Instances = nil, Opacity = nil, Scratch = nil;
    id<MTLCommandBuffer> Command = nil;
    internal::StorageArray<foundation::usize> Geometries;
    foundation::usize GeometryCount = 0;
};
struct RayMetalBatch final
{
    id<MTLBuffer> Inputs = nil, Outputs = nil;
    id<MTLCommandBuffer> Command = nil;
    foundation::usize Count = 0;
};
/// Native ray structures, batches and programs of one Metal owner (R14c).
struct RayMetalState final
{
    internal::StorageTable<RayMetalGeometry> Geometry;
    internal::StorageTable<RayMetalScene> Scenes;
    internal::StorageTable<RayMetalBatch> Batches;
    internal::StorageTable<id<MTLComputePipelineState>> Programs;
};
StaticArray<RayMetalState, MAX_DEVICE_OWNERS> gRayMetalStates{};
/// The native ray objects of the bound owner slot.
RayMetalState& GetRayMetal() noexcept
{
    return gRayMetalStates[internal::GetBoundOwnerSlot()];
}
RasterStatus RayMetalCommandStatus(id<MTLCommandBuffer> command) noexcept
{
    if (command == nil)
    {
        return RasterStatus::Failed;
    }
    if (command.status == MTLCommandBufferStatusError)
    {
        LogError(command.error);
        return RasterStatus::Failed;
    }
    return command.status == MTLCommandBufferStatusCompleted ? RasterStatus::Ready : RasterStatus::Pending;
}
bool RayMetalSizes(const MTLAccelerationStructureSizes& sizes,
                   usize inputs,
                   usize budget,
                   RayAllocation& output) noexcept
{
    const auto limit = RayLimits().MaxBufferBytes;
    if (sizes.accelerationStructureSize == 0 || sizes.accelerationStructureSize > limit ||
        sizes.buildScratchBufferSize > limit || inputs > budget || sizes.accelerationStructureSize > budget - inputs ||
        sizes.buildScratchBufferSize > budget - inputs - sizes.accelerationStructureSize)
    {
        return false;
    }
    output = {inputs + sizes.accelerationStructureSize, sizes.buildScratchBufferSize};
    return true;
}
} // namespace
RayCapabilities RayLimits() noexcept
{
    // Thanks to Apple, "Boost your graphics performance with the M5 and A19 GPUs",
    // Tech Talks, architecture overview (2:01), for the M3 hardware ray-traversal boundary:
    // https://developer.apple.com/videos/play/tech-talks/111431/ .
    // Apple GPU family 9 introduces hardware ray traversal. Older families can
    // expose Metal ray APIs through shader execution and are deliberately excluded.
    if (GetMetal().Device == nil || GetMetal().Queue == nil || GetMetal().Device.supportsRaytracing == false ||
        [GetMetal().Device supportsFamily:MTLGPUFamilyApple9] == false)
    {
        return {};
    }
    return
    {
        .StaticTriangles = true,
        .InlineQueries = true,
        .RayPipelines = GetMetal().Device.supportsFunctionPointers == true,
        .Updates = true,
        .CanCompact = true,
        .MaxInstances = internal::CurrentStorageLimits().RayInstances,
        .MaxRays = 4096,
        .MaxBufferBytes = internal::GetResourceSizeLimit(GetMetal().Device.maxBufferLength),
    };
}
void BindRayMetalScene(id<MTLComputeCommandEncoder> encoder, const RayMetalSceneBinding& description) noexcept
{
    const RayMetalScene& scene = GetRayMetal().Scenes[description.Scene];
    [encoder setAccelerationStructure:scene.Structure atBufferIndex:description.Binding];
    for (usize i = 0; i < scene.GeometryCount; ++i)
    {
        [encoder useResource:GetRayMetal().Geometry[scene.Geometries[i]].Structure usage:MTLResourceUsageRead];
    }
}
RasterStatus RayCreateProgram(usize slot, const ShaderDescription& description) noexcept
{
    if (GetRayMetal().Programs.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    @autoreleasepool
    {
        if (description.Msl.empty() || description.MslEntry.empty())
        {
            return RasterStatus::InvalidDescription;
        }
        NSString* source = [[NSString alloc] initWithBytes:description.Msl.data()
                                                    length:description.Msl.size()
                                                  encoding:NSUTF8StringEncoding];
        NSString* entry = [[NSString alloc] initWithBytes:description.MslEntry.data()
                                                   length:description.MslEntry.size()
                                                 encoding:NSUTF8StringEncoding];
        if (source == nil || entry == nil)
        {
            return RasterStatus::InvalidDescription;
        }
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_4;
        ConfigureMetalComputeMath(options);
        NSError* error = nil;
        id<MTLLibrary> library = [GetMetal().Device newLibraryWithSource:source options:options error:&error];
        if (library == nil)
        {
            LogError(error);
            return RasterStatus::Failed;
        }
        id<MTLFunction> function = [library newFunctionWithName:entry];
        if (function == nil || function.functionType != MTLFunctionTypeKernel)
        {
            return RasterStatus::InvalidDescription;
        }
        MTLComputePipelineReflection* reflection = nil;
        id<MTLComputePipelineState> pipeline = [GetMetal().Device newComputePipelineStateWithFunction:function
                                                                                              options:BINDING_INFO
                                                                                           reflection:&reflection
                                                                                                error:&error];
        if (pipeline == nil)
        {
            LogError(error);
            return RasterStatus::Failed;
        }
        if (pipeline.maxTotalThreadsPerThreadgroup < 64 || reflection.bindings.count != 5)
        {
            LUDUS_LOG_ERROR(LOG_RHI,
                            "Ray query reflection has {} bindings and {} maximum threads",
                            static_cast<uint64>(reflection.bindings.count),
                            static_cast<uint64>(pipeline.maxTotalThreadsPerThreadgroup));
            return RasterStatus::InvalidDescription;
        }
        for (id<MTLBinding> argument in reflection.bindings)
        {
            const bool structure =
                argument.type == MTLBindingTypeInstanceAccelerationStructure ||
                (argument.type == MTLBindingTypeBuffer &&
                 ((id<MTLBufferBinding>)argument).bufferDataType == MTLDataTypeInstanceAccelerationStructure);
            if (argument.index > LAST_RAY_QUERY_BINDING_INDEX ||
                (argument.index == 0 ? structure == false : argument.type != MTLBindingTypeBuffer))
            {
                LUDUS_LOG_ERROR(LOG_RHI,
                                "Ray query binding {} has native type {}",
                                static_cast<uint64>(argument.index),
                                static_cast<uint64>(argument.type));
                if (argument.type == MTLBindingTypeBuffer)
                {
                    LUDUS_LOG_ERROR(LOG_RHI,
                                    "Ray query buffer data type {}",
                                    static_cast<uint64>(((id<MTLBufferBinding>)argument).bufferDataType));
                    for (MTLStructMember* member in ((id<MTLBufferBinding>)argument).bufferStructType.members)
                    {
                        LUDUS_LOG_ERROR(LOG_RHI,
                                        "Ray query member {} has data type {} at {}",
                                        member.name.UTF8String,
                                        static_cast<uint64>(member.dataType),
                                        static_cast<uint64>(member.offset));
                    }
                }
                return RasterStatus::InvalidDescription;
            }
        }
        GetRayMetal().Programs[slot] = pipeline;
        return RasterStatus::Ready;
    }
}
namespace
{
/// Build geometry, or refit the Ready source slot when it is not RAY_METAL_NO_SOURCE.
RasterStatus
RayMetalBuildGeometry(usize slot, usize source, const RayGeometryDescription& d, RayAllocation& outAllocation) noexcept
{
    if (GetRayMetal().Geometry.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    @autoreleasepool
    {
        const usize vertexBytes = d.VertexCount * 3 * sizeof(float32), indexBytes = d.IndexCount * sizeof(uint32);
        if (vertexBytes > d.ByteBudget || indexBytes > d.ByteBudget - vertexBytes)
        {
            return RasterStatus::CapacityExceeded;
        }
        RayMetalGeometry geometry;
        geometry.Vertices = [GetMetal().Device newBufferWithBytes:d.Positions
                                                           length:vertexBytes
                                                          options:MTLResourceStorageModeShared];
        geometry.Indices = [GetMetal().Device newBufferWithBytes:d.Indices
                                                          length:indexBytes
                                                         options:MTLResourceStorageModeShared];
        if (geometry.Vertices == nil || geometry.Indices == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto* triangles = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        triangles.vertexBuffer = geometry.Vertices;
        triangles.vertexStride = 3 * sizeof(float32);
        triangles.vertexFormat = MTLAttributeFormatFloat3;
        triangles.indexBuffer = geometry.Indices;
        triangles.indexType = MTLIndexTypeUInt32;
        constexpr usize TRIANGLE_CORNER_COUNT = 3;
        const usize triangleCount = d.IndexCount / TRIANGLE_CORNER_COUNT;
        triangles.triangleCount = d.OpaqueTriangleCount != 0 ? d.OpaqueTriangleCount : triangleCount;
        triangles.opaque = d.OpaqueTriangleCount != 0;
        auto* descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        if (d.OpaqueTriangleCount != 0 && d.OpaqueTriangleCount < triangleCount)
        {
            MTLAccelerationStructureTriangleGeometryDescriptor* maskedOrNull = [triangles copy];
            if (maskedOrNull == nil)
            {
                return RasterStatus::OutOfMemory;
            }
            maskedOrNull.indexBufferOffset = usize(d.OpaqueTriangleCount) * TRIANGLE_CORNER_COUNT * sizeof(uint32);
            maskedOrNull.triangleCount = triangleCount - d.OpaqueTriangleCount;
            maskedOrNull.opaque = NO;
            descriptor.geometryDescriptors = @[ triangles, maskedOrNull ];
        }
        else
        {
            descriptor.geometryDescriptors = @[ triangles ];
        }
        descriptor.usage = GetRayMetalUsage(d.ShouldAllowUpdate);
        const id<MTLAccelerationStructure> sourceOrNil =
            source == RAY_METAL_NO_SOURCE ? nil : GetRayMetal().Geometry[source].Structure;
        MTLAccelerationStructureSizes sizes = [GetMetal().Device accelerationStructureSizesWithDescriptor:descriptor];
        sizes.accelerationStructureSize = GetRayMetalStructureBytes(sourceOrNil, sizes.accelerationStructureSize);
        if (sourceOrNil != nil)
        {
            sizes.buildScratchBufferSize = sizes.refitScratchBufferSize;
        }
        if (RayMetalSizes(sizes, vertexBytes + indexBytes, d.ByteBudget, outAllocation) == false)
        {
            return RasterStatus::CapacityExceeded;
        }
        if (d.ShouldAllowCompaction == true)
        {
            geometry.CompactedSizeOrNil = [GetMetal().Device newBufferWithLength:sizeof(uint64)
                                                                         options:MTLResourceStorageModeShared];
            if (geometry.CompactedSizeOrNil == nil)
            {
                return RasterStatus::OutOfMemory;
            }
        }
        geometry.Structure = [GetMetal().Device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        geometry.Scratch =
            [GetMetal().Device newBufferWithLength:GetRayMetalScratchAllocationBytes(sizes.buildScratchBufferSize)
                                           options:MTLResourceStorageModePrivate];
        geometry.Command = [GetMetal().Queue commandBuffer];
        if (geometry.Structure == nil || geometry.Scratch == nil || geometry.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [geometry.Command accelerationStructureCommandEncoder];
        if (encoder == nil)
        {
            return RasterStatus::Failed;
        }
        if (sourceOrNil == nil)
        {
            [encoder buildAccelerationStructure:geometry.Structure
                                     descriptor:descriptor
                                  scratchBuffer:geometry.Scratch
                            scratchBufferOffset:0];
        }
        else
        {
            [encoder refitAccelerationStructure:sourceOrNil
                                     descriptor:descriptor
                                    destination:geometry.Structure
                                  scratchBuffer:geometry.Scratch
                            scratchBufferOffset:0];
        }
        if (geometry.CompactedSizeOrNil != nil)
        {
            [encoder writeCompactedAccelerationStructureSize:geometry.Structure
                                                    toBuffer:geometry.CompactedSizeOrNil
                                                      offset:0
                                                sizeDataType:MTLDataTypeULong];
        }
        [encoder endEncoding];
        GetRayMetal().Geometry[slot] = geometry;
        [geometry.Command commit];
        return RasterStatus::Pending;
    }
}
} // namespace
RasterStatus
RayCreateGeometry(usize slot, const RayGeometryDescription& description, RayAllocation& outAllocation) noexcept
{
    return RayMetalBuildGeometry(slot, RAY_METAL_NO_SOURCE, description, outAllocation);
}
RasterStatus RayUpdateGeometry(usize slot,
                               usize source,
                               const RayGeometryDescription& description,
                               RayAllocation& outAllocation) noexcept
{
    return RayMetalBuildGeometry(slot, source, description, outAllocation);
}
RasterStatus
RayCompactGeometry(usize slot, const internal::NativeRayCompaction& compaction, RayAllocation& outAllocation) noexcept
{
    const usize source = compaction.Source;
    const usize byteBudget = compaction.ByteBudget;
    if (GetRayMetal().Geometry.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    @autoreleasepool
    {
        const RayMetalGeometry& from = GetRayMetal().Geometry[source];
        if (from.CompactedSizeOrNil == nil || from.CompactedSizeOrNil.contents == nullptr)
        {
            return RasterStatus::Failed;
        }
        uint64 compactedBytes = 0;
        std::memcpy(&compactedBytes, from.CompactedSizeOrNil.contents, sizeof(compactedBytes));
        if (compactedBytes == 0 || compactedBytes > from.Structure.size)
        {
            return RasterStatus::Failed;
        }
        if (compactedBytes > byteBudget || compactedBytes > RayLimits().MaxBufferBytes)
        {
            return RasterStatus::CapacityExceeded;
        }
        RayMetalGeometry geometry;
        geometry.Structure = [GetMetal().Device newAccelerationStructureWithSize:static_cast<usize>(compactedBytes)];
        geometry.Command = [GetMetal().Queue commandBuffer];
        if (geometry.Structure == nil || geometry.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        id<MTLAccelerationStructureCommandEncoder> encoder = [geometry.Command accelerationStructureCommandEncoder];
        if (encoder == nil)
        {
            return RasterStatus::Failed;
        }
        [encoder copyAndCompactAccelerationStructure:from.Structure toAccelerationStructure:geometry.Structure];
        [encoder endEncoding];
        GetRayMetal().Geometry[slot] = geometry;
        outAllocation = {static_cast<usize>(compactedBytes), 0};
        [geometry.Command commit];
        return RasterStatus::Pending;
    }
}
namespace
{
/// Build a scene, or refit the Ready sourceScene slot when it is not RAY_METAL_NO_SOURCE.
RasterStatus RayMetalBuildScene(usize slot,
                                usize sourceScene,
                                const internal::NativeRaySceneDescription& description,
                                RayAllocation& outAllocation) noexcept
{
    if (GetRayMetal().Scenes.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto* input = description.Instances;
    const auto* opacity = description.Opacity;
    const auto count = description.Count, scalarCount = description.ScalarCount, budget = description.ByteBudget;

    @autoreleasepool
    {
        const usize instanceBytes =
            (count == 0 ? usize{1} : count) * sizeof(MTLAccelerationStructureInstanceDescriptor);
        const usize opacityBytes = scalarCount * sizeof(float32);
        if (instanceBytes > budget || opacityBytes > budget - instanceBytes)
        {
            return RasterStatus::CapacityExceeded;
        }
        RayMetalScene scene;
        if (scene.Geometries.TryResize(count) == false)
        {
            return internal::StorageFailure();
        }
        scene.Instances = [GetMetal().Device newBufferWithLength:instanceBytes options:MTLResourceStorageModeShared];
        scene.Opacity = [GetMetal().Device newBufferWithBytes:opacity
                                                       length:opacityBytes
                                                      options:MTLResourceStorageModeShared];
        if (scene.Instances == nil || scene.Opacity == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto* instances = static_cast<MTLAccelerationStructureInstanceDescriptor*>(scene.Instances.contents);
        auto* structures = [NSMutableArray<id<MTLAccelerationStructure>> array];
        for (usize i = 0; i < count; ++i)
        {
            const auto& source = input[i];
            usize index = 0;
            while (index < scene.GeometryCount && scene.Geometries[index] != source.Geometry)
            {
                ++index;
            }
            if (index == scene.GeometryCount)
            {
                scene.Geometries[index] = source.Geometry;
                [structures addObject:GetRayMetal().Geometry[source.Geometry].Structure];
                ++scene.GeometryCount;
            }
            auto& instance = instances[i];
            instance = {};
            for (usize column = 0; column < 4; ++column)
            {
                instance.transformationMatrix.columns[column].x = source.Transform[column];
                instance.transformationMatrix.columns[column].y = source.Transform[4 + column];
                instance.transformationMatrix.columns[column].z = source.Transform[8 + column];
            }
            // Thanks to Apple, "Your guide to Metal ray tracing", WWDC23 session 10128,
            // https://developer.apple.com/videos/play/wwdc2023/10128/ : specialize opacity
            // per instance, leaving masked candidates available to the calling shader.
            instance.options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling |
                               (source.IsOpaque == true ? MTLAccelerationStructureInstanceOptionOpaque : 0);
            instance.mask = source.Mask;
            instance.accelerationStructureIndex = static_cast<uint32>(index);
        }
        auto* descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
        descriptor.instanceDescriptorBuffer = scene.Instances;
        descriptor.instanceCount = count;
        descriptor.instancedAccelerationStructures = structures;
        descriptor.usage = GetRayMetalUsage(description.ShouldAllowUpdate);
        const id<MTLAccelerationStructure> sourceOrNil =
            sourceScene == RAY_METAL_NO_SOURCE ? nil : GetRayMetal().Scenes[sourceScene].Structure;
        MTLAccelerationStructureSizes sizes = [GetMetal().Device accelerationStructureSizesWithDescriptor:descriptor];
        sizes.accelerationStructureSize = GetRayMetalStructureBytes(sourceOrNil, sizes.accelerationStructureSize);
        if (sourceOrNil != nil)
        {
            sizes.buildScratchBufferSize = sizes.refitScratchBufferSize;
        }
        if (RayMetalSizes(sizes, instanceBytes + opacityBytes, budget, outAllocation) == false)
        {
            return RasterStatus::CapacityExceeded;
        }
        scene.Structure = [GetMetal().Device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        scene.Scratch =
            [GetMetal().Device newBufferWithLength:GetRayMetalScratchAllocationBytes(sizes.buildScratchBufferSize)
                                           options:MTLResourceStorageModePrivate];
        scene.Command = [GetMetal().Queue commandBuffer];
        if (scene.Structure == nil || scene.Scratch == nil || scene.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [scene.Command accelerationStructureCommandEncoder];
        if (encoder == nil)
        {
            return RasterStatus::Failed;
        }
        if (sourceOrNil == nil)
        {
            [encoder buildAccelerationStructure:scene.Structure
                                     descriptor:descriptor
                                  scratchBuffer:scene.Scratch
                            scratchBufferOffset:0];
        }
        else
        {
            [encoder refitAccelerationStructure:sourceOrNil
                                     descriptor:descriptor
                                    destination:scene.Structure
                                  scratchBuffer:scene.Scratch
                            scratchBufferOffset:0];
        }
        [encoder endEncoding];
        GetRayMetal().Scenes[slot] = Move(scene);
        [GetRayMetal().Scenes[slot].Command commit];
        return RasterStatus::Pending;
    }
}
} // namespace
RasterStatus
RayCreateScene(usize slot, const internal::NativeRaySceneDescription& description, RayAllocation& allocation) noexcept
{
    return RayMetalBuildScene(slot, RAY_METAL_NO_SOURCE, description, allocation);
}
RasterStatus RayUpdateScene(usize slot,
                            usize source,
                            const internal::NativeRaySceneDescription& description,
                            RayAllocation& outAllocation) noexcept
{
    return RayMetalBuildScene(slot, source, description, outAllocation);
}
RasterStatus RaySubmit(usize slot, const internal::NativeRaySubmission& submission, RayAllocation& allocation) noexcept
{
    if (GetRayMetal().Batches.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto program = submission.Program, scene = submission.Scene, count = submission.Count;
    const auto* rays = submission.Rays;

    @autoreleasepool
    {
        RayMetalBatch batch;
        batch.Count = count;
        batch.Inputs = [GetMetal().Device newBufferWithBytes:rays
                                                      length:count * sizeof(RayInput)
                                                     options:MTLResourceStorageModeShared];
        batch.Outputs = [GetMetal().Device newBufferWithLength:count * sizeof(RayResult)
                                                       options:MTLResourceStorageModeShared];
        batch.Command = [GetMetal().Queue commandBuffer];
        if (batch.Inputs == nil || batch.Outputs == nil || batch.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [batch.Command computeCommandEncoder];
        if (encoder == nil)
        {
            return RasterStatus::Failed;
        }
        const RayMetalScene& source = GetRayMetal().Scenes[scene];
        [encoder setComputePipelineState:GetRayMetal().Programs[program]];
        [encoder setAccelerationStructure:source.Structure atBufferIndex:0];
        [encoder setBuffer:batch.Inputs offset:0 atIndex:1];
        [encoder setBuffer:batch.Outputs offset:0 atIndex:2];
        [encoder setBuffer:source.Opacity offset:0 atIndex:3];
        const uint32 parameters[4]{static_cast<uint32>(count), 0, 0, 0};
        [encoder setBytes:parameters length:sizeof(parameters) atIndex:4];
        for (usize i = 0; i < source.GeometryCount; ++i)
        {
            [encoder useResource:GetRayMetal().Geometry[source.Geometries[i]].Structure usage:MTLResourceUsageRead];
        }
        [encoder dispatchThreadgroups:MTLSizeMake((count + 63) / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        GetRayMetal().Batches[slot] = batch;
        allocation = {count * (sizeof(RayInput) + sizeof(RayResult)), 0};
        [batch.Command commit];
        return RasterStatus::Pending;
    }
}
RasterStatus RayPoll(internal::RasterKind kind, usize slot) noexcept
{
    if (kind == internal::RasterKind::RayProgram)
    {
        return GetRayMetal().Programs[slot] == nil ? RasterStatus::Failed : RasterStatus::Ready;
    }
    if (kind == internal::RasterKind::RayBatch)
    {
        return RayMetalCommandStatus(GetRayMetal().Batches[slot].Command);
    }
    if (kind == internal::RasterKind::RayGeometry)
    {
        const RasterStatus status = RayMetalCommandStatus(GetRayMetal().Geometry[slot].Command);
        if (status == RasterStatus::Ready)
        {
            GetRayMetal().Geometry[slot].Scratch = nil;
        }
        return status;
    }
    const RasterStatus status = RayMetalCommandStatus(GetRayMetal().Scenes[slot].Command);
    if (status == RasterStatus::Ready)
    {
        GetRayMetal().Scenes[slot].Scratch = nil;
    }
    return status;
}
RasterStatus RayRead(usize slot, RayResult* output, usize count) noexcept
{
    const RayMetalBatch& batch = GetRayMetal().Batches[slot];
    const RasterStatus status = RayMetalCommandStatus(batch.Command);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    if (batch.Count != count || batch.Outputs.contents == nullptr)
    {
        return RasterStatus::InvalidDescription;
    }
    std::memcpy(output, batch.Outputs.contents, count * sizeof(RayResult));
    return RasterStatus::Ready;
}
void RayDestroy(internal::RasterKind kind, usize slot) noexcept
{
    if (kind == internal::RasterKind::RayGeometry && slot < GetRayMetal().Geometry.GetSize())
    {
        GetRayMetal().Geometry[slot] = {};
    }
    else if (kind == internal::RasterKind::RayScene && slot < GetRayMetal().Scenes.GetSize())
    {
        GetRayMetal().Scenes[slot] = {};
    }
    else if (kind == internal::RasterKind::RayBatch && slot < GetRayMetal().Batches.GetSize())
    {
        GetRayMetal().Batches[slot] = {};
    }
    else if (kind == internal::RasterKind::RayProgram && slot < GetRayMetal().Programs.GetSize())
    {
        GetRayMetal().Programs[slot] = nil;
    }
}
void RayDrain() noexcept
{
    for (RayMetalBatch& batch : GetRayMetal().Batches)
    {
        if (batch.Command != nil)
        {
            [batch.Command waitUntilCompleted];
        }
    }
    for (RayMetalScene& scene : GetRayMetal().Scenes)
    {
        if (scene.Command != nil)
        {
            [scene.Command waitUntilCompleted];
        }
    }
    for (RayMetalGeometry& geometry : GetRayMetal().Geometry)
    {
        if (geometry.Command != nil)
        {
            [geometry.Command waitUntilCompleted];
        }
    }
}
} // namespace ludus::graphics::rhi::backend
