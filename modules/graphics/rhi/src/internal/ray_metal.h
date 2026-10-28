#pragma once
// Included only by rhi_metal.mm; the device and queue remain its private objects.
// Thanks to Apple, "Accelerating ray tracing using Metal" and "Enhance your app
// with Metal ray tracing", WWDC21 session 10149, for immutable BLAS/TLAS builds,
// candidate filtering and explicit useResource for indirectly referenced BLAS:
// https://developer.apple.com/documentation/metal/accelerating-ray-tracing-using-metal
// https://developer.apple.com/videos/play/wwdc2021/10149/
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
};
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
internal::StorageTable<RayMetalGeometry> gRayMetalGeometry;
internal::StorageTable<RayMetalScene> gRayMetalScene;
internal::StorageTable<RayMetalBatch> gRayMetalBatch;
internal::StorageTable<id<MTLComputePipelineState>> gRayMetalPrograms;
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
    if (gDevice == nil || gQueue == nil || gDevice.supportsRaytracing == false ||
        [gDevice supportsFamily:MTLGPUFamilyApple9] == false)
    {
        return {};
    }
    return
    {
        .StaticTriangles = true,
        .InlineQueries = true,
        .RayPipelines = gDevice.supportsFunctionPointers == true,
        .MaxInstances = internal::CurrentStorageLimits().RayInstances,
        .MaxRays = 4096,
        .MaxBufferBytes = internal::GetResourceSizeLimit(gDevice.maxBufferLength),
    };
}
void BindRayMetalScene(id<MTLComputeCommandEncoder> encoder, const RayMetalSceneBinding& description) noexcept
{
    const RayMetalScene& scene = gRayMetalScene[description.Scene];
    [encoder setAccelerationStructure:scene.Structure atBufferIndex:description.Binding];
    for (usize i = 0; i < scene.GeometryCount; ++i)
    {
        [encoder useResource:gRayMetalGeometry[scene.Geometries[i]].Structure usage:MTLResourceUsageRead];
    }
}
RasterStatus RayCreateProgram(usize slot, const ShaderDescription& description) noexcept
{
    if (gRayMetalPrograms.EnsureSlots(slot + 1) == false)
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
        if (@available(macOS 15.0, *)) // ludus-style: magic-number Metal mathMode availability version
        {
            options.mathMode = MTLMathModeSafe;
        }
#if __MAC_OS_X_VERSION_MIN_REQUIRED < 150000
        else
        {
            options.fastMathEnabled = NO;
        }
#endif
        NSError* error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:source options:options error:&error];
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
        id<MTLComputePipelineState> pipeline = [gDevice newComputePipelineStateWithFunction:function
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
        gRayMetalPrograms[slot] = pipeline;
        return RasterStatus::Ready;
    }
}
RasterStatus RayCreateGeometry(usize slot, const RayGeometryDescription& d, RayAllocation& allocation) noexcept
{
    if (gRayMetalGeometry.EnsureSlots(slot + 1) == false)
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
        geometry.Vertices = [gDevice newBufferWithBytes:d.Positions
                                                 length:vertexBytes
                                                options:MTLResourceStorageModeShared];
        geometry.Indices = [gDevice newBufferWithBytes:d.Indices
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
        const auto sizes = [gDevice accelerationStructureSizesWithDescriptor:descriptor];
        if (RayMetalSizes(sizes, vertexBytes + indexBytes, d.ByteBudget, allocation) == false)
        {
            return RasterStatus::CapacityExceeded;
        }
        geometry.Structure = [gDevice newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        geometry.Scratch = [gDevice newBufferWithLength:sizes.buildScratchBufferSize
                                                options:MTLResourceStorageModePrivate];
        geometry.Command = [gQueue commandBuffer];
        if (geometry.Structure == nil || geometry.Scratch == nil || geometry.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [geometry.Command accelerationStructureCommandEncoder];
        if (encoder == nil)
        {
            return RasterStatus::Failed;
        }
        [encoder buildAccelerationStructure:geometry.Structure
                                 descriptor:descriptor
                              scratchBuffer:geometry.Scratch
                        scratchBufferOffset:0];
        [encoder endEncoding];
        gRayMetalGeometry[slot] = geometry;
        [geometry.Command commit];
        return RasterStatus::Pending;
    }
}
RasterStatus
RayCreateScene(usize slot, const internal::NativeRaySceneDescription& description, RayAllocation& allocation) noexcept
{
    if (gRayMetalScene.EnsureSlots(slot + 1) == false)
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
        scene.Instances = [gDevice newBufferWithLength:instanceBytes options:MTLResourceStorageModeShared];
        scene.Opacity = [gDevice newBufferWithBytes:opacity length:opacityBytes options:MTLResourceStorageModeShared];
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
                [structures addObject:gRayMetalGeometry[source.Geometry].Structure];
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
        const auto sizes = [gDevice accelerationStructureSizesWithDescriptor:descriptor];
        if (RayMetalSizes(sizes, instanceBytes + opacityBytes, budget, allocation) == false)
        {
            return RasterStatus::CapacityExceeded;
        }
        scene.Structure = [gDevice newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        scene.Scratch = [gDevice newBufferWithLength:sizes.buildScratchBufferSize
                                             options:MTLResourceStorageModePrivate];
        scene.Command = [gQueue commandBuffer];
        if (scene.Structure == nil || scene.Scratch == nil || scene.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [scene.Command accelerationStructureCommandEncoder];
        if (encoder == nil)
        {
            return RasterStatus::Failed;
        }
        [encoder buildAccelerationStructure:scene.Structure
                                 descriptor:descriptor
                              scratchBuffer:scene.Scratch
                        scratchBufferOffset:0];
        [encoder endEncoding];
        gRayMetalScene[slot] = Move(scene);
        [gRayMetalScene[slot].Command commit];
        return RasterStatus::Pending;
    }
}
RasterStatus RaySubmit(usize slot, const internal::NativeRaySubmission& submission, RayAllocation& allocation) noexcept
{
    if (gRayMetalBatch.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    const auto program = submission.Program, scene = submission.Scene, count = submission.Count;
    const auto* rays = submission.Rays;

    @autoreleasepool
    {
        RayMetalBatch batch;
        batch.Count = count;
        batch.Inputs = [gDevice newBufferWithBytes:rays
                                            length:count * sizeof(RayInput)
                                           options:MTLResourceStorageModeShared];
        batch.Outputs = [gDevice newBufferWithLength:count * sizeof(RayResult) options:MTLResourceStorageModeShared];
        batch.Command = [gQueue commandBuffer];
        if (batch.Inputs == nil || batch.Outputs == nil || batch.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [batch.Command computeCommandEncoder];
        if (encoder == nil)
        {
            return RasterStatus::Failed;
        }
        const auto& source = gRayMetalScene[scene];
        [encoder setComputePipelineState:gRayMetalPrograms[program]];
        [encoder setAccelerationStructure:source.Structure atBufferIndex:0];
        [encoder setBuffer:batch.Inputs offset:0 atIndex:1];
        [encoder setBuffer:batch.Outputs offset:0 atIndex:2];
        [encoder setBuffer:source.Opacity offset:0 atIndex:3];
        const uint32 parameters[4]{static_cast<uint32>(count), 0, 0, 0};
        [encoder setBytes:parameters length:sizeof(parameters) atIndex:4];
        for (usize i = 0; i < source.GeometryCount; ++i)
        {
            [encoder useResource:gRayMetalGeometry[source.Geometries[i]].Structure usage:MTLResourceUsageRead];
        }
        [encoder dispatchThreadgroups:MTLSizeMake((count + 63) / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        gRayMetalBatch[slot] = batch;
        allocation = {count * (sizeof(RayInput) + sizeof(RayResult)), 0};
        [batch.Command commit];
        return RasterStatus::Pending;
    }
}
RasterStatus RayPoll(internal::RasterKind kind, usize slot) noexcept
{
    if (kind == internal::RasterKind::RayProgram)
    {
        return gRayMetalPrograms[slot] == nil ? RasterStatus::Failed : RasterStatus::Ready;
    }
    if (kind == internal::RasterKind::RayBatch)
    {
        return RayMetalCommandStatus(gRayMetalBatch[slot].Command);
    }
    if (kind == internal::RasterKind::RayGeometry)
    {
        const auto status = RayMetalCommandStatus(gRayMetalGeometry[slot].Command);
        if (status == RasterStatus::Ready)
        {
            gRayMetalGeometry[slot].Scratch = nil;
        }
        return status;
    }
    const auto status = RayMetalCommandStatus(gRayMetalScene[slot].Command);
    if (status == RasterStatus::Ready)
    {
        gRayMetalScene[slot].Scratch = nil;
    }
    return status;
}
RasterStatus RayRead(usize slot, RayResult* output, usize count) noexcept
{
    const auto& batch = gRayMetalBatch[slot];
    const auto status = RayMetalCommandStatus(batch.Command);
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
    if (kind == internal::RasterKind::RayGeometry && slot < gRayMetalGeometry.GetSize())
    {
        gRayMetalGeometry[slot] = {};
    }
    else if (kind == internal::RasterKind::RayScene && slot < gRayMetalScene.GetSize())
    {
        gRayMetalScene[slot] = {};
    }
    else if (kind == internal::RasterKind::RayBatch && slot < gRayMetalBatch.GetSize())
    {
        gRayMetalBatch[slot] = {};
    }
    else if (kind == internal::RasterKind::RayProgram && slot < gRayMetalPrograms.GetSize())
    {
        gRayMetalPrograms[slot] = nil;
    }
}
void RayDrain() noexcept
{
    for (auto& batch : gRayMetalBatch)
    {
        if (batch.Command != nil)
        {
            [batch.Command waitUntilCompleted];
        }
    }
    for (auto& scene : gRayMetalScene)
    {
        if (scene.Command != nil)
        {
            [scene.Command waitUntilCompleted];
        }
    }
    for (auto& geometry : gRayMetalGeometry)
    {
        if (geometry.Command != nil)
        {
            [geometry.Command waitUntilCompleted];
        }
    }
}
} // namespace ludus::graphics::rhi::backend
