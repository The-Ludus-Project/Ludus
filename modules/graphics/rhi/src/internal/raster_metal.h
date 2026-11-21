#pragma once

#include "native_ray_metal.h"
#include "profile.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
// Private implementation included only by rhi_metal.mm: native device/frame
// ownership remains in the existing backend translation unit.
// Thanks to Apple, "Resource Fundamentals", Metal Programming Guide (retained
// command buffers), and "MTLVertexDescriptor" (separate stream/attribute maps):
// https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/ResourceFundamentals/ResourceFundamentals.html
// https://developer.apple.com/documentation/metal/mtlvertexdescriptor
namespace ludus::graphics::rhi::backend
{
namespace
{
MTLTextureType RasterMetalTextureType(TextureDimension dimension) noexcept
{
    return dimension == TextureDimension::D2Array ? MTLTextureType2DArray
           : dimension == TextureDimension::Cube  ? MTLTextureTypeCube
           : dimension == TextureDimension::D3    ? MTLTextureType3D
                                                  : MTLTextureType2D;
}
struct RasterMetalShader final
{
    id<MTLFunction> Function = nil;
    MetalRayFunctions RayFunctions;
    internal::RasterShaderInfo Info;
};
struct RasterMetalPipeline final
{
    id<MTLRenderPipelineState> Object = nil;
    id<MTLDepthStencilState> Depth = nil;
    internal::RasterPipelineInfo Info;
};
// Thanks to KhronosGroup, SPIRV-Cross CompilerMSL::Options::buffer_size_buffer_index:
// https://github.com/KhronosGroup/SPIRV-Cross/blob/main/spirv_msl.hpp
// Its OpArrayLength lowering reads byte ranges indexed by native buffer slot.
// Keep the reserved auxiliary slot separate from the public group-zero layout.
constexpr uint32 METAL_BUFFER_SIZE_TABLE_SLOT = 25;
/// Multisample count the attachment profile reports when the device supports it, beside one sample.
constexpr uint32 METAL_MULTISAMPLE_COUNT = 4;
using MetalBufferSizeTable = foundation::StaticArray<uint32, internal::RASTER_BINDINGS>;
struct MetalComputePipeline final
{
    MetalRayPipeline RayPipeline;
    id<MTLComputePipelineState> ObjectOrNull = nil;
    bool HasBufferSizeTable = false;
    id<MTLCommandQueue> NativeQueueOrNull = nil;
    id<MTLSharedEvent> NativeEventOrNull = nil;
    uint64 NativeEventValue = 0;
};
/// Native raster and compute objects and submission ordinals of one Metal owner (R14c).
struct RasterMetalState final
{
    internal::StorageTable<id<MTLBuffer>> Buffers;
    internal::StorageTable<id<MTLTexture>> Textures;
    internal::StorageTable<id<MTLTexture>> Depths;
    internal::StorageTable<id<MTLTexture>> Views;
    internal::StorageTable<id<MTLTexture>> ViewDepths;
    internal::StorageTable<id<MTLSamplerState>> Samplers;
    internal::StorageTable<RasterMetalShader> Shaders;
    internal::StorageTable<internal::RasterLayout> Layouts;
    internal::StorageTable<internal::RasterSet> Sets;
    internal::StorageTable<RasterMetalPipeline> Pipelines;
    internal::StorageTable<MetalComputePipeline> ComputePipelines;
    internal::StorageTable<internal::ComputePipelineInfo> ComputeInfos;
    uint64 Ordinals[FRAMES]{};
    uint64 Completed = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    RasterPassDescription Region{};
};
StaticArray<RasterMetalState, MAX_DEVICE_OWNERS> gRasterMetalStates{};
/// The native raster objects of the bound owner slot.
RasterMetalState& GetRasterMetal() noexcept
{
    return gRasterMetalStates[internal::GetBoundOwnerSlot()];
}
bool IsMetalBufferSizeTable(id<MTLBinding> binding, ShaderStage stage) noexcept
{
    return stage == ShaderStage::Compute && binding.type == MTLBindingTypeBuffer &&
           binding.index == METAL_BUFFER_SIZE_TABLE_SLOT && binding.access == MTLBindingAccessReadOnly &&
           [binding.name isEqualToString:@"spvBufferSizeConstants"] == true &&
           ((id<MTLBufferBinding>)binding).bufferDataSize <= sizeof(MetalBufferSizeTable);
}
bool RasterMetalBindings(NSArray<id<MTLBinding>>* bindings,
                         const internal::RasterShaderInfo& shader,
                         const internal::RasterPipelineInfo& pipeline) noexcept
{
    for (id<MTLBinding> binding in bindings)
    {
        if (binding.used == false)
        {
            continue;
        }
        // Metal reports stage-in vertex streams as buffer bindings as well as
        // user shader resources. Their reserved indices follow the verified
        // vertex descriptor, independently of the group-zero resource layout.
        if (shader.Stage == ShaderStage::Vertex && binding.type == MTLBindingTypeBuffer &&
            binding.index >= internal::RASTER_BINDINGS &&
            binding.index < internal::RASTER_BINDINGS + pipeline.StreamCount)
        {
            continue;
        }
        if (shader.HasNativeRayStages == true && ((binding.type == MTLBindingTypeVisibleFunctionTable &&
                                                   binding.index == LUDUS_METAL_RAY_FUNCTION_TABLE_BINDING) ||
                                                  (binding.type == MTLBindingTypeIntersectionFunctionTable &&
                                                   binding.index == LUDUS_METAL_RAY_INTERSECTION_TABLE_BINDING)))
        {
            continue;
        }
        if (IsMetalBufferSizeTable(binding, shader.Stage) == true)
        {
            continue;
        }
        bool found = false;
        for (usize i = 0; i < shader.Layout.Count; ++i)
        {
            const auto& entry = shader.Layout.Entries[i];
            if (entry.Binding != binding.index)
            {
                continue;
            }
            if ((entry.Kind == RasterBindingKind::UniformBuffer || entry.Kind == RasterBindingKind::StorageRead ||
                 entry.Kind == RasterBindingKind::StorageReadWrite) &&
                binding.type == MTLBindingTypeBuffer &&
                (binding.access == MTLBindingAccessReadOnly ||
                 (entry.Kind == RasterBindingKind::StorageReadWrite && binding.access == MTLBindingAccessReadWrite)))
            {
                found = ((id<MTLBufferBinding>)binding).bufferDataSize <= entry.MinSize;
            }
            else if (entry.Kind == RasterBindingKind::RayScene)
            {
                found = binding.type == MTLBindingTypeInstanceAccelerationStructure ||
                        (binding.type == MTLBindingTypeBuffer &&
                         ((id<MTLBufferBinding>)binding).bufferDataType == MTLDataTypeInstanceAccelerationStructure);
            }
            else if (internal::TextureBinding(entry.Kind) && binding.type == MTLBindingTypeTexture)
            {
                found = ((id<MTLTextureBinding>)binding).textureType ==
                            (entry.Kind == RasterBindingKind::Texture2DArray ? MTLTextureType2DArray
                             : entry.Kind == RasterBindingKind::TextureCube  ? MTLTextureTypeCube
                             : entry.Kind == RasterBindingKind::Texture3D    ? MTLTextureType3D
                                                                             : MTLTextureType2D) &&
                        binding.access == MTLBindingAccessReadOnly &&
                        ((id<MTLTextureBinding>)binding).arrayLength == entry.Count;
            }
            else if (entry.Kind == RasterBindingKind::Sampler && binding.type == MTLBindingTypeSampler)
            {
                found = true;
            }
            else if (internal::StorageTextureBinding(entry.Kind) == true && binding.type == MTLBindingTypeTexture)
            {
                // Metal storage textures carry no texel format; the layout's format binds the view.
                const MTLBindingAccess access = entry.Kind == RasterBindingKind::StorageTexture2DReadWrite
                                                    ? MTLBindingAccessReadWrite
                                                    : MTLBindingAccessWriteOnly;
                found = ((id<MTLTextureBinding>)binding).textureType == MTLTextureType2D && binding.access == access &&
                        ((id<MTLTextureBinding>)binding).arrayLength == 1;
            }
        }
        if (found == false)
        {
            LUDUS_LOG_WARN(LOG_RHI,
                           "Raster Metal binding {} index {} type {} is absent from stage reflection",
                           binding.name.UTF8String,
                           static_cast<uint64>(binding.index),
                           static_cast<uint64>(binding.type));
            return false;
        }
    }
    for (usize entry = 0; entry < shader.Layout.Count; ++entry)
    {
        const RasterBinding& declared = shader.Layout.Entries[entry];
        if (declared.Kind != RasterBindingKind::RayScene)
        {
            continue;
        }
        bool hasNativeScene = false;
        for (id<MTLBinding> binding in bindings)
        {
            hasNativeScene =
                hasNativeScene == true ||
                (binding.used == true && binding.index == declared.Binding &&
                 (binding.type == MTLBindingTypeInstanceAccelerationStructure ||
                  (binding.type == MTLBindingTypeBuffer &&
                   ((id<MTLBufferBinding>)binding).bufferDataType == MTLDataTypeInstanceAccelerationStructure)));
        }
        if (hasNativeScene == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace
// Thanks to Apple, "Metal Feature Set Tables", ordinary 8/16/32-bit normalized formats.
// https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
// Intersect guaranteed normalized formats and enabled HDR with implemented uses;
// no storage/MSAA or R8/RG8 attachment path is advertised. R8 copies use RGBA8Unorm.
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    if (GetMetal().Device == nil ||
        (internal::CompressedTexture(format) && GetMetal().Device.supportsBCTextureCompression == false))
    {
        return {};
    }
    FormatCapabilities result = internal::PortableFormatLimits(format, RasterLimits());
    result.HasUnalignedBlockExtents = internal::CompressedTexture(format);
    if (result.Uses.Sampled)
    {
        // Thanks to Apple, Metal Feature Set Tables, Resources (Mac2/Apple4+):
        // https://developer.apple.com/metal/limits/ — 2048 array layers and 3D texels.
        result.MaxArrayLayers = 2048;
        result.MaxCubeDimension = result.MaxSampledDimension2D;
        result.MaxDimension3D = internal::CompressedTexture(format) ? 0U : 2048U;
        result.PartialViews = true;
    }
    if (result.Uses.ColorAttachment)
    {
        result.MaxAttachmentMipLevels = result.MaxMipLevels;
        result.MaxAttachmentLayers = result.MaxArrayLayers;
        result.AttachmentViews = true;
        result.DepthSampled = true;
        result.StencilAttachment = true;
        result.StencilSampled = true;
        result.SampleCounts = 1U | ([GetMetal().Device supportsTextureSampleCount:METAL_MULTISAMPLE_COUNT] == YES
                                        ? METAL_MULTISAMPLE_COUNT
                                        : 0U);
    }
    result.MaxAttachmentCubeDimension = result.Uses.ColorAttachment ? result.MaxCubeDimension : 0;
    return result;
}
uint32 RasterColorAttachmentLimit() noexcept
{
    return 4;
}
uint32 RasterVertexStrideLimit() noexcept
{
    return 256;
}
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage&) noexcept
{
    return false;
}
BindingCapabilities BindingLimits() noexcept
{
    if (GetMetal().Device == nil)
    {
        return {};
    }
    return
    {
        .MaxDynamicUniformBuffers = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxDynamicStorageBuffers = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxTextureArrayElements = 31,
        .MaxDescriptorsPerSet = internal::CurrentStorageLimits().DescriptorsPerSet,
        .HasUniformTextureIndexing = true,
        .HasBindlessTextureIndexing = internal::IsBindlessRequested(),
        .MaxBindings = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxUniformBuffersPerStage = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxSampledTexturesPerStage = 31,
        .MaxSamplersPerStage = 16,
        .MaxUniformBuffersPerSet = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxSamplersPerSet = static_cast<uint32>(internal::RASTER_BINDINGS),
        .HasSamplerDescriptorArrays = true,
        .HasPartiallyBoundArrays = true,
    };
}
RasterCapabilities RasterLimits() noexcept
{
    const usize max = internal::GetResourceSizeLimit(GetMetal().Device.maxBufferLength);
    return
    {
        .MaxBufferSize = GetMetal().Device.maxBufferLength < max ? GetMetal().Device.maxBufferLength : max,
        .MaxTextureDimension2D = MAX_DIMENSION,
        .UniformOffsetAlignment = 256,
        .MaxUniformRange = 16384,
        .ResourcesPerKind = internal::CurrentStorageLimits().ResourcesPerKind,
        .DrawsPerFrame = internal::CurrentStorageLimits().DrawsPerFrame,
        .MaxVertexAttributes = 16,
        .HdrColor = true,
        .MaxTextureUploadBytes = max,
        .HasLineFill = true,
    };
}
// Thanks to Apple, "Metal Feature Set Tables" (writable textures in fragment functions,
// Apple3+/Mac2), https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf.
// Fragment/vertex storage buffers share the group-zero buffer slots; R12b storage
// textures use texture slots equal to their binding numbers, like sampled textures.
// Tracked resources and encoder boundaries order these writes before later reads.
RasterStorageCapabilities RasterStorageLimits() noexcept
{
    if (GetMetal().Device == nil)
    {
        return {};
    }
    const bool hasWritableTextures = [GetMetal().Device supportsFamily:MTLGPUFamilyApple3] == YES ||
                                     [GetMetal().Device supportsFamily:MTLGPUFamilyMac2] == YES;
    const uint32 slots = static_cast<uint32>(internal::RASTER_BINDINGS);
    return
    {
        .MaxVertexStorageBuffers = slots,
        .MaxFragmentStorageBuffers = slots,
        .HasFragmentStorageWrites = true,
        .MaxFragmentStorageTextures = hasWritableTextures == true ? slots : 0U,
        .MaxComputeStorageTextures = slots,
    };
}
// Thanks to Apple, "Metal Feature Set Tables" (base vertex/instance drawing and indirect
// draws/dispatches, Apple3+/Mac2) and "MTLDrawIndexedPrimitivesIndirectArguments",
// "MTLDrawPrimitivesIndirectArguments" and "MTLDispatchThreadgroupsIndirectArguments", whose
// layouts match the engine ABI. https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
IndirectCapabilities IndirectLimits() noexcept
{
    if (GetMetal().Device == nil)
    {
        return {};
    }
    const bool hasBaseVertexInstance = [GetMetal().Device supportsFamily:MTLGPUFamilyApple3] == YES ||
                                       [GetMetal().Device supportsFamily:MTLGPUFamilyMac2] == YES;
    return
    {
        .ArgumentAlignment = internal::INDIRECT_ARGUMENT_ALIGNMENT_BYTES,
        .MaxCommandsPerDraw = internal::MAX_INDIRECT_COMMANDS_PER_DRAW,
        .HasNonIndexedDraws = true,
        .HasDrawOffsets = hasBaseVertexInstance,
        .HasFirstInstance = hasBaseVertexInstance,
        .HasIndirectDispatch = true,
    };
}
// Thanks to Apple, "MTLComputeCommandEncoder" and Metal's tracked-resource
// synchronization: encoder boundaries order storage writes before raster reads.
// https://developer.apple.com/documentation/metal/mtlcomputecommandencoder
ComputeCapabilities ComputeLimits() noexcept
{
    if (GetMetal().Device == nil)
    {
        return {};
    }
    const MTLSize threads = GetMetal().Device.maxThreadsPerThreadgroup;
    const usize range = RasterLimits().MaxBufferSize;
    return {GetMetal().Device.maxBufferLength < range ? GetMetal().Device.maxBufferLength : range,
            256,
            {static_cast<uint32>(threads.width < 256 ? threads.width : 256),
             static_cast<uint32>(threads.height < 256 ? threads.height : 256),
             static_cast<uint32>(threads.depth < 64 ? threads.depth : 64)},
            256,
            {65535, 65535, 65535},
            static_cast<uint32>(internal::RASTER_BINDINGS)};
}
// Thanks to Apple, "MTLCompileOptions.mathMode" and "MTLMathMode", Metal documentation:
// https://developer.apple.com/documentation/metal/mtlcompileoptions/mathmode.
// Compute libraries disable Metal's default fast math. With fast or relaxed math, the merged render-graph
// ray kernels lost private values of whole SIMD groups at random on an Apple M5, although their MSL is well
// defined: wave exports returned exact zeros and a closure program read a zeroed register. The repetition
// test in wave_scene_graph_tests.cpp exposed it in every run; safe math removed it in thousands of captures.
// Safe math still contracts a*b + c written as one MSL expression. Render stages keep fast math.
// See docs/architecture/renderer-systems.md#metal-compute-floating-point.
void ConfigureMetalComputeMath(MTLCompileOptions* options) noexcept
{
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
}
// Thanks to Apple, "MTLDevice", Metal documentation, for library/pipeline creation:
// https://developer.apple.com/documentation/metal/mtldevice.
// Retain immutable device/function inputs on a dedicated preparation lane;
// publish results and retire resources exclusively on the RHI owner thread.
struct MetalShaderPreparation final : NativePreparationTask
{
    id<MTLDevice> DeviceOrNull = nil;
    NSString* SourceOrNull = nil;
    NSString* EntryOrNull = nil;
    NSError* ErrorOrNull = nil;
    RasterMetalShader Shader;
    RasterStatus Outcome = RasterStatus::Failed;
    void Execute() noexcept override
    {
        @autoreleasepool
        {
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion2_4;
            if (Shader.Info.Stage == ShaderStage::Compute)
            {
                ConfigureMetalComputeMath(options);
            }
            NSError* errorOrNull = nil;
            id<MTLLibrary> libraryOrNull = [DeviceOrNull newLibraryWithSource:SourceOrNull
                                                                      options:options
                                                                        error:&errorOrNull];
            ErrorOrNull = errorOrNull;
            id<MTLFunction> functionOrNull = [libraryOrNull newFunctionWithName:EntryOrNull];
            const MTLFunctionType expected = Shader.Info.Stage == ShaderStage::Vertex     ? MTLFunctionTypeVertex
                                             : Shader.Info.Stage == ShaderStage::Fragment ? MTLFunctionTypeFragment
                                                                                          : MTLFunctionTypeKernel;
            if (functionOrNull != nil && functionOrNull.functionType == expected)
            {
                Shader.Function = functionOrNull;
                if (Shader.Info.HasNativeRayStages == false ||
                    (DeviceOrNull.supportsFunctionPointers == true &&
                     LoadMetalRayFunctions(libraryOrNull, Shader.RayFunctions) == true))
                {
                    Outcome = RasterStatus::Ready;
                }
            }
        }
    }
    RasterStatus Complete(bool) noexcept override
    {
        if (Outcome == RasterStatus::Ready)
        {
            GetRasterMetal().Shaders[ResourceSlot] = Shader;
        }
        else
        {
            LogError(ErrorOrNull);
        }
        internal::RasterComplete(Request, Outcome);
        return Outcome;
    }
};
struct MetalComputePreparation final : NativePreparationTask
{
    MetalRayPipeline RayPipeline;
    id<MTLDevice> DeviceOrNull = nil;
    RasterMetalShader Shader;
    internal::ComputePipelineInfo Info;
    bool HasBufferSizeTable = false;
    id<MTLComputePipelineState> ObjectOrNull = nil;
    NSError* ErrorOrNull = nil;
    RasterStatus Outcome = RasterStatus::Failed;
    RasterStatus Build() noexcept;
    void Execute() noexcept override
    {
        Outcome = Build();
    }
    RasterStatus Complete(bool) noexcept override
    {
        if (Outcome == RasterStatus::Ready)
        {
            GetRasterMetal().ComputePipelines[ResourceSlot].ObjectOrNull = ObjectOrNull;
            GetRasterMetal().ComputePipelines[ResourceSlot].HasBufferSizeTable = HasBufferSizeTable;
            GetRasterMetal().ComputePipelines[ResourceSlot].RayPipeline = RayPipeline;
            GetRasterMetal().ComputeInfos[ResourceSlot] = Info;
        }
        else
        {
            LogError(ErrorOrNull);
        }
        internal::RasterComplete(Request, Outcome);
        return Outcome;
    }
};
struct MetalRasterPreparation final : NativePreparationTask
{
    id<MTLDevice> DeviceOrNull = nil;
    RasterMetalShader Vertex;
    RasterMetalShader Fragment;
    internal::RasterPipelineInfo Info;
    RasterMetalPipeline Pipeline;
    NSError* ErrorOrNull = nil;
    RasterStatus Outcome = RasterStatus::Failed;
    RasterStatus Build() noexcept;
    void Execute() noexcept override
    {
        Outcome = Build();
    }
    RasterStatus Complete(bool) noexcept override
    {
        if (Outcome == RasterStatus::Ready)
        {
            GetRasterMetal().Pipelines[ResourceSlot] = Pipeline;
        }
        else
        {
            LogError(ErrorOrNull);
        }
        internal::RasterComplete(Request, Outcome);
        return Outcome;
    }
};
RasterStatus MetalComputePreparation::Build() noexcept
{
    const internal::ComputePipelineInfo& info = Info;
    @autoreleasepool
    {

        MTLComputePipelineReflection* reflectionOrNull = nil;
        NSError* errorOrNull = nil;
        MTLComputePipelineDescriptor* descriptor = [MTLComputePipelineDescriptor new];
        descriptor.computeFunction = Shader.Function;
        if (Shader.Info.HasNativeRayStages == true)
        {
            MTLLinkedFunctions* linked = [MTLLinkedFunctions new];
            linked.functions = @[
                Shader.RayFunctions.Stages[0],
                Shader.RayFunctions.Stages[1],
                Shader.RayFunctions.Stages[2],
                Shader.RayFunctions.IntersectionOrNull
            ];
            descriptor.linkedFunctions = linked;
            descriptor.maxCallStackDepth = LUDUS_METAL_RAY_MAX_CALL_DEPTH;
        }
        id<MTLComputePipelineState> pipelineOrNull =
            [DeviceOrNull newComputePipelineStateWithDescriptor:descriptor
                                                        options:BINDING_INFO | MTLPipelineOptionBufferTypeInfo
                                                     reflection:&reflectionOrNull
                                                          error:&errorOrNull];
        ErrorOrNull = errorOrNull;
        const uint32 threads = info.WorkgroupSize[0] * info.WorkgroupSize[1] * info.WorkgroupSize[2];
        if (pipelineOrNull == nil || reflectionOrNull == nil ||
            threads > pipelineOrNull.maxTotalThreadsPerThreadgroup ||
            RasterMetalBindings(reflectionOrNull.bindings, Shader.Info, {}) == false)
        {

            return RasterStatus::Failed;
        }
        for (id<MTLBinding> binding in reflectionOrNull.bindings)
        {
            HasBufferSizeTable =
                HasBufferSizeTable == true ||
                (binding.used == true && IsMetalBufferSizeTable(binding, ShaderStage::Compute) == true);
        }
        if (Shader.Info.HasNativeRayStages == true &&
            CreateMetalRayPipeline(pipelineOrNull, Shader.RayFunctions, RayPipeline) == false)
        {
            return RasterStatus::Failed;
        }
        ObjectOrNull = pipelineOrNull;
        return RasterStatus::Ready;
    }
}
RasterStatus ComputeCreatePipeline(usize slot, const internal::ComputePipelineInfo& info, uint32 request) noexcept
{
    if (info.Native.SubmitOrNull != nullptr)
    {
        MetalComputePipeline& pipeline = GetRasterMetal().ComputePipelines[slot];
        if (pipeline.NativeQueueOrNull == nil)
        {
            pipeline.NativeQueueOrNull = [GetMetal().Device newCommandQueue];
            pipeline.NativeEventOrNull = [GetMetal().Device newSharedEvent];
        }
        if (pipeline.NativeQueueOrNull == nil || pipeline.NativeEventOrNull == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        const internal::RasterSet& set = GetRasterMetal().Sets[info.NativeSet];
        StaticArray<NativeComputeBuffer, internal::RASTER_BINDINGS> buffers{};
        for (usize index = 0; index < set.Description.Count; ++index)
        {
            buffers[index] = {(__bridge void*)GetRasterMetal().Buffers[set.Slots[index]],
                              set.Offsets[index],
                              set.Sizes[index]};
        }
        const NativeDeviceIdentity identity
        {
            .Api = Backend::Metal,
            .Device = NativeLogicalDevice{(__bridge void*)GetMetal().Device},
            .CommandQueue = NativeCommandQueue{(__bridge void*)pipeline.NativeQueueOrNull},
        };
        const RasterStatus status =
            info.Native.PrepareOrNull(info.Native.ContextOrNull, identity, {buffers.GetData(), set.Description.Count});
        if (status == RasterStatus::Ready || status == RasterStatus::Pending)
        {
            GetRasterMetal().ComputeInfos[slot] = info;
        }
        return status;
    }
    UniquePtr<MetalComputePreparation> task = MakePreparationTask<MetalComputePreparation>();
    if (task.Get() == nullptr)
    {
        return internal::StorageFailure();
    }
    task->IsPipelinePreparation = true;
    task->Kind = internal::RasterKind::ComputePipeline;
    task->DeviceOrNull = GetMetal().Device;
    task->Shader = GetRasterMetal().Shaders[info.Shader];
    task->Info = info;
    task->ResourceSlot = slot;
    task->Request = request;
    return SubmitNativePreparation(foundation::Move(task), info.ShouldPrepareAsynchronously);
}
struct RayMetalSceneBinding final
{
    usize Scene = 0;
    uint32 Binding = 0;
};
void BindRayMetalScene(id<MTLComputeCommandEncoder> encoder, const RayMetalSceneBinding& description) noexcept;
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    const internal::ComputePipelineInfo& info = GetRasterMetal().ComputeInfos[packet.Pipeline];
    if (info.Native.SubmitOrNull != nullptr)
    {
        MetalComputePipeline& pipeline = GetRasterMetal().ComputePipelines[packet.Pipeline];
        MetalCommandPrefixes& prefixes = GetMetal().CommandPrefixes[GetMetal().Slot];
        if (MAX_NATIVE_COMPUTE_PREFIXES - prefixes.Count < PREFIXES_PER_NATIVE_COMPUTE)
        {
            return RasterStatus::CapacityExceeded;
        }
        constexpr uint64 EVENT_STEPS = 2;
        if (pipeline.NativeEventValue > ~uint64{0} - EVENT_STEPS)
        {
            return RasterStatus::IdentityExhausted;
        }
        id<MTLCommandBuffer> nextOrNull = [GetMetal().Queue commandBuffer];
        id<MTLCommandBuffer> waitOrNull = [pipeline.NativeQueueOrNull commandBuffer];
        id<MTLCommandBuffer> completedOrNull = [pipeline.NativeQueueOrNull commandBuffer];
        if (nextOrNull == nil || waitOrNull == nil || completedOrNull == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        const uint64 producerValue = pipeline.NativeEventValue + 1;
        const uint64 completionValue = pipeline.NativeEventValue + EVENT_STEPS;
        pipeline.NativeEventValue = completionValue;
        PopScopeGroups();
        // The private queue can initialize models off-thread without submitting on
        // a borrowed framework queue. Native events order access to the shared data.
        [GetMetal().Command encodeSignalEvent:pipeline.NativeEventOrNull value:producerValue];
        prefixes.CommandsOrNull[prefixes.Count++] = GetMetal().Command;
        [GetMetal().Command commit];
        [waitOrNull encodeWaitForEvent:pipeline.NativeEventOrNull value:producerValue];
        prefixes.CommandsOrNull[prefixes.Count++] = waitOrNull;
        [waitOrNull commit];
        const RasterStatus status = info.Native.SubmitOrNull(info.Native.ContextOrNull);
        [completedOrNull encodeSignalEvent:pipeline.NativeEventOrNull value:completionValue];
        prefixes.CommandsOrNull[prefixes.Count++] = completedOrNull;
        [completedOrNull commit];
        GetMetal().Command = nextOrNull;
        [GetMetal().Command encodeWaitForEvent:pipeline.NativeEventOrNull value:completionValue];
        GetMetal().HasNativeComputeSplit = true;
        if (status != RasterStatus::Ready)
        {
            // Fence partial library work even when the graph cannot publish output.
            [GetMetal().Command commit];
            GetMetal().Submitted[GetMetal().Slot] = GetMetal().Command;
            GetMetal().Command = nil;
        }
        else
        {
            PushScopeGroups();
        }
        return status;
    }
    const MetalComputePipeline& pipeline = GetRasterMetal().ComputePipelines[packet.Pipeline];
    const internal::RasterSet& bindings = GetRasterMetal().Sets[packet.Set];
    MetalBufferSizeTable bufferSizes{};
    if (pipeline.HasBufferSizeTable == true)
    {
        for (usize i = 0; i < bindings.Description.Count; ++i)
        {
            const RasterBinding& entry = bindings.Description.Entries[i];
            if ((entry.Kind == RasterBindingKind::StorageRead || entry.Kind == RasterBindingKind::StorageReadWrite) &&
                foundation::TryIntegerCast(bindings.Sizes[i], bufferSizes[entry.Binding]) == false)
            {
                return RasterStatus::Unsupported;
            }
        }
    }
    MTLComputePassDescriptor* description = [MTLComputePassDescriptor computePassDescriptor];
    ConfigurePassTiming(description,
                        pipeline.RayPipeline.StagesOrNull != nil ? GpuPassKind::RayTracing : GpuPassKind::Compute);
    id<MTLComputeCommandEncoder> encoder = [GetMetal().Command computeCommandEncoderWithDescriptor:description];
    encoder.label = GetGpuScopeMetal().PassLabelOrNull;
    if (encoder == nil)
    {
        return RasterStatus::OutOfMemory;
    }
    [encoder setComputePipelineState:pipeline.ObjectOrNull];
    if (pipeline.HasBufferSizeTable == true)
    {
        [encoder setBytes:bufferSizes.GetData() length:sizeof(bufferSizes) atIndex:METAL_BUFFER_SIZE_TABLE_SLOT];
    }
    const internal::RasterSet& set = GetRasterMetal().Sets[packet.Set];
    const internal::RasterLayout& layout = set.Description;
    for (usize i = 0; i < layout.Count; ++i)
    {
        if (layout.Entries[i].Kind == RasterBindingKind::RayScene)
        {
            BindRayMetalScene(encoder, {set.Slots[i], layout.Entries[i].Binding});
            continue;
        }
        if (internal::ViewBinding(layout.Entries[i].Kind) == true)
        {
            [encoder setTexture:GetRasterMetal().Views[set.Slots[i]] atIndex:layout.Entries[i].Binding];
        }
        else
        {
            [encoder setBuffer:GetRasterMetal().Buffers[set.Slots[i]]
                        offset:set.Offsets[i] + packet.BindingOffsets.Bytes[layout.Entries[i].Binding]
                       atIndex:layout.Entries[i].Binding];
        }
    }
    if (pipeline.RayPipeline.StagesOrNull != nil)
    {
        id<MTLIntersectionFunctionTable> intersectionsOrNull =
            CreateMetalRayIntersections(pipeline.ObjectOrNull, pipeline.RayPipeline);
        if (intersectionsOrNull == nil)
        {
            [encoder endEncoding];
            return RasterStatus::OutOfMemory;
        }
        for (usize index = 0; index < layout.Count; ++index)
        {
            const uint32 binding = layout.Entries[index].Binding;
            if (binding == 0 || binding == 1 || binding == LUDUS_METAL_RAY_ALPHA_TEXTURE_BINDING)
            {
                [intersectionsOrNull setBuffer:GetRasterMetal().Buffers[set.Slots[index]]
                                        offset:set.Offsets[index] + packet.BindingOffsets.Bytes[binding]
                                       atIndex:binding];
            }
        }
        [encoder setVisibleFunctionTable:pipeline.RayPipeline.StagesOrNull
                           atBufferIndex:LUDUS_METAL_RAY_FUNCTION_TABLE_BINDING];
        [encoder setIntersectionFunctionTable:intersectionsOrNull
                                atBufferIndex:LUDUS_METAL_RAY_INTERSECTION_TABLE_BINDING];
    }
    const MTLSize threads = MTLSizeMake(info.WorkgroupSize[0], info.WorkgroupSize[1], info.WorkgroupSize[2]);
    if (packet.Indirect != internal::RASTER_INVALID)
    {
        [encoder dispatchThreadgroupsWithIndirectBuffer:GetRasterMetal().Buffers[packet.Indirect]
                                   indirectBufferOffset:packet.IndirectOffset
                                  threadsPerThreadgroup:threads];
    }
    else
    {
        [encoder dispatchThreadgroups:MTLSizeMake(packet.Groups[0], packet.Groups[1], packet.Groups[2])
                threadsPerThreadgroup:threads];
    }
    [encoder endEncoding];
    return RasterStatus::Ready;
}
void ComputeBufferBarrier(usize, GraphAccessMode) noexcept {}
RasterStatus RasterCreateBuffer(usize slot,
                                const BufferDescription&,
                                ludus::foundation::BorrowedView<const uint8> bytes,
                                uint32) noexcept
{
    @autoreleasepool
    {
        GetRasterMetal().Buffers[slot] = [GetMetal().Device newBufferWithBytes:bytes.GetDataOrNull()
                                                                        length:bytes.GetSize()
                                                                       options:MTLResourceStorageModeShared];
    }
    return GetRasterMetal().Buffers[slot] != nil ? RasterStatus::Ready : RasterStatus::OutOfMemory;
}
RasterStatus
RasterCreateTexture(usize slot, const TextureDescription& info, const TextureUpload& upload, uint32) noexcept
{
    @autoreleasepool
    {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:info.Format == RasterFormat::Bc1RgbaUnorm   ? MTLPixelFormatBC1_RGBA
                                               : info.Format == RasterFormat::Bc3RgbaUnorm ? MTLPixelFormatBC3_RGBA
                                               : info.Format == RasterFormat::Bc5RgUnorm   ? MTLPixelFormatBC5_RGUnorm
                                               : info.Format == RasterFormat::Rgba16Float  ? MTLPixelFormatRGBA16Float
                                               : info.Format == RasterFormat::Rg8Unorm     ? MTLPixelFormatRG8Unorm
                                               : info.Format == RasterFormat::R8Unorm      ? MTLPixelFormatR8Unorm
                                               : info.Format == RasterFormat::Rgba8Unorm ? MTLPixelFormatRGBA8Unorm
                                                                                         : MTLPixelFormatRGBA8Unorm_sRGB
                                         width:info.Width
                                        height:info.Height
                                     mipmapped:NO];
        descriptor.textureType =
            info.SampleCount == 4 ? MTLTextureType2DMultisample : RasterMetalTextureType(info.Dimension);
        descriptor.sampleCount = info.SampleCount;
        descriptor.depth = info.Dimension == TextureDimension::D3 ? info.DepthOrLayers : 1;
        descriptor.arrayLength = info.Dimension == TextureDimension::D2Array ? info.DepthOrLayers : 1;
        descriptor.mipmapLevelCount = info.MipLevels;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView |
                           (info.Attachment ? MTLTextureUsageRenderTarget : MTLTextureUsageUnknown) |
                           (info.IsStorage ? MTLTextureUsageShaderWrite : MTLTextureUsageUnknown);
        descriptor.storageMode = info.Attachment                            ? MTLStorageModePrivate
                                 : internal::CompressedTexture(info.Format) ? MTLStorageModeManaged
                                                                            : MTLStorageModeShared;
        auto texture = [GetMetal().Device newTextureWithDescriptor:descriptor];
        if (texture == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        if (info.Attachment == false && info.DeferredInitialization == false)
        {
            uint32 width = info.Width, height = info.Height;
            for (usize level = 0; level < info.MipLevels; ++level)
            {
                const TextureMipUpload mip =
                    upload.Mips.IsEmpty() == true ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[level];
                const usize imagePitch = mip.RowPitch * internal::TextureBlockRows(info.Format, height);
                if (info.Dimension == TextureDimension::D3)
                {
                    [texture replaceRegion:MTLRegionMake3D(0,
                                                           0,
                                                           0,
                                                           width,
                                                           height,
                                                           internal::TextureSlices(info, static_cast<uint32>(level)))
                               mipmapLevel:level
                                     slice:0
                                 withBytes:upload.Bytes.GetDataOrNull() + mip.Offset
                               bytesPerRow:mip.RowPitch
                             bytesPerImage:imagePitch];
                }
                else
                {
                    for (uint32 slice = 0; slice < info.DepthOrLayers; ++slice)
                    {
                        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                                   mipmapLevel:level
                                         slice:slice
                                     withBytes:upload.Bytes.GetDataOrNull() + mip.Offset + slice * imagePitch
                                   bytesPerRow:mip.RowPitch
                                 bytesPerImage:imagePitch];
                    }
                }
                width = width > 1 ? width / 2 : 1;
                height = height > 1 ? height / 2 : 1;
            }
        }
        else if (info.Attachment)
        {
            auto depth = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:info.Stencil ? MTLPixelFormatDepth32Float_Stencil8
                                                                : MTLPixelFormatDepth32Float
                                             width:info.Width
                                            height:info.Height
                                         mipmapped:NO];
            depth.textureType = descriptor.textureType;
            depth.arrayLength = descriptor.arrayLength;
            depth.mipmapLevelCount = info.MipLevels;
            depth.sampleCount = info.SampleCount;
            depth.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            depth.storageMode = MTLStorageModePrivate;
            GetRasterMetal().Depths[slot] = [GetMetal().Device newTextureWithDescriptor:depth];
            if (GetRasterMetal().Depths[slot] == nil)
            {
                return RasterStatus::OutOfMemory;
            }
        }
        GetRasterMetal().Textures[slot] = texture;
        return RasterStatus::Ready;
    }
}
// Thanks to Apple, MTLTexture.newTextureViewWithPixelFormat:textureType:levels:slices:,
// https://developer.apple.com/documentation/metal/mtltexture, for original-format
// alias views with explicit mip and slice lifetimes. R9 retains the source allocation.
RasterStatus RasterCreateView(usize slot, const internal::RasterViewSource& source, uint32) noexcept
{
    id<MTLTexture> texture = source.View.Aspect == TextureAspect::Color ? GetRasterMetal().Textures[source.Texture]
                                                                        : GetRasterMetal().Depths[source.Texture];
    const auto format = source.View.Aspect == TextureAspect::Stencil ? MTLPixelFormatX32_Stencil8 : texture.pixelFormat;
    const auto type =
        texture.sampleCount == 4 ? MTLTextureType2DMultisample : RasterMetalTextureType(source.View.Dimension);
    GetRasterMetal().Views[slot] =
        [texture newTextureViewWithPixelFormat:format
                                   textureType:type
                                        levels:NSMakeRange(source.View.BaseMip, source.View.MipCount)
                                        slices:NSMakeRange(source.View.BaseLayer, source.View.LayerCount)];
    if (source.View.Attachment)
    {
        id<MTLTexture> depth = GetRasterMetal().Depths[source.Texture];
        GetRasterMetal().ViewDepths[slot] = [depth newTextureViewWithPixelFormat:depth.pixelFormat
                                                                     textureType:type
                                                                          levels:NSMakeRange(source.View.BaseMip, 1)
                                                                          slices:NSMakeRange(source.View.BaseLayer, 1)];
        if (GetRasterMetal().ViewDepths[slot] == nil)
        {
            return RasterStatus::OutOfMemory;
        }
    }
    return GetRasterMetal().Views[slot] != nil ? RasterStatus::Ready : RasterStatus::OutOfMemory;
}
RasterStatus RasterCreateSampler(usize slot, const SamplerDescription& info, uint32) noexcept
{
    @autoreleasepool
    {
        MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
        descriptor.minFilter = descriptor.magFilter =
            info.Filter == RasterFilter::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        descriptor.mipFilter = info.MipFilter == RasterMipFilter::Linear    ? MTLSamplerMipFilterLinear
                               : info.MipFilter == RasterMipFilter::Nearest ? MTLSamplerMipFilterNearest
                                                                            : MTLSamplerMipFilterNotMipmapped;
        descriptor.lodMinClamp = info.MinLod;
        descriptor.lodMaxClamp = info.MipFilter == RasterMipFilter::None ? 0 : info.MaxLod;
        descriptor.sAddressMode =
            info.U == RasterAddress::Clamp ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        descriptor.tAddressMode =
            info.V == RasterAddress::Clamp ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        descriptor.rAddressMode =
            info.W == RasterAddress::Clamp ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        GetRasterMetal().Samplers[slot] = [GetMetal().Device newSamplerStateWithDescriptor:descriptor];
        return GetRasterMetal().Samplers[slot] != nil ? RasterStatus::Ready : RasterStatus::OutOfMemory;
    }
}
RasterStatus RasterCreateShader(usize slot,
                                const ShaderDescription& artifact,
                                const internal::RasterShaderInfo& info,
                                uint32 request) noexcept
{
    @autoreleasepool
    {
        UniquePtr<MetalShaderPreparation> task = MakePreparationTask<MetalShaderPreparation>();
        if (task.Get() == nullptr)
        {
            return internal::StorageFailure();
        }
        usize sourceBytes = 0;
        if (foundation::TryAdd(artifact.Msl.size(), artifact.MslEntry.size(), sourceBytes) == false ||
            internal::ReserveHostStorage(sourceBytes) == false)
        {
            return internal::StorageFailure();
        }
        task->AllocationBytes += sourceBytes;
        task->DeviceOrNull = GetMetal().Device;
        task->SourceOrNull = [[NSString alloc] initWithBytes:artifact.Msl.data()
                                                      length:artifact.Msl.size()
                                                    encoding:NSUTF8StringEncoding];
        task->EntryOrNull = [[NSString alloc] initWithBytes:artifact.MslEntry.data()
                                                     length:artifact.MslEntry.size()
                                                   encoding:NSUTF8StringEncoding];
        if (task->SourceOrNull == nil || task->EntryOrNull == nil)
        {
            return RasterStatus::InvalidDescription;
        }
        task->Shader.Info = info;
        task->ResourceSlot = slot;
        task->Request = request;
        return SubmitNativePreparation(foundation::Move(task), info.ShouldPrepareAsynchronously);
    }
}
RasterStatus RasterCreateLayout(usize slot, const internal::RasterLayout& info, uint32) noexcept
{
    for (usize index = 0; index < info.Count; ++index)
    {
        if (info.Entries[index].Kind == RasterBindingKind::StorageTexture2DReadWrite &&
            GetMetal().Device.readWriteTextureSupport < MTLReadWriteTextureTier2)
        {
            return RasterStatus::Unsupported;
        }
    }
    GetRasterMetal().Layouts[slot] = info;
    return RasterStatus::Ready;
}
RasterStatus
RasterCreateSet(usize slot, const internal::RasterLayout&, const internal::RasterSet& info, uint32) noexcept
{
    return GetRasterMetal().Sets[slot].TryCopyFrom(info) == true ? RasterStatus::Ready : internal::StorageFailure();
}
RasterStatus MetalRasterPreparation::Build() noexcept
{
    const internal::RasterPipelineInfo& info = Info;
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = Vertex.Function;
        descriptor.fragmentFunction = Fragment.Function;
        descriptor.colorAttachments[0].pixelFormat =
            info.Target == RasterTarget::Rgba16Float  ? MTLPixelFormatRGBA16Float
            : info.Target == RasterTarget::Surface    ? GetMetal().SurfaceFormat
            : info.Target == RasterTarget::Rgba8Unorm ? MTLPixelFormatRGBA8Unorm
                                                      : MTLPixelFormatRGBA8Unorm_sRGB;
        descriptor.rasterSampleCount = info.SampleCount;
        descriptor.alphaToCoverageEnabled = info.ShouldUseAlphaToCoverage;
        descriptor.depthAttachmentPixelFormat =
            info.Stencil == true ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
        if (info.Stencil == true)
        {
            descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        }
        for (usize i = 0; i < info.ColorCount; ++i)
        {
            descriptor.colorAttachments[i].pixelFormat =
                info.ColorFormats[i] == RasterFormat::Rgba16Float  ? MTLPixelFormatRGBA16Float
                : info.ColorFormats[i] == RasterFormat::Rgba8Unorm ? MTLPixelFormatRGBA8Unorm
                                                                   : MTLPixelFormatRGBA8Unorm_sRGB;
        }
        for (usize i = 0; i < (info.ColorCount == 0 ? 1 : info.ColorCount); ++i)
        {
            MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[i];
            color.writeMask = info.ShouldWriteColor == true ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            color.blendingEnabled = info.Blend;
            color.sourceRGBBlendFactor = color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationRGBBlendFactor = color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        MTLVertexDescriptor* vertex = [MTLVertexDescriptor vertexDescriptor];
        for (usize i = 0; i < info.StreamCount; ++i)
        {
            const usize binding = internal::RASTER_BINDINGS + i;
            vertex.layouts[binding].stride = info.Streams[i].Stride;
            vertex.layouts[binding].stepFunction =
                info.Streams[i].PerInstance == true ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            vertex.layouts[binding].stepRate = 1;
        }
        for (usize i = 0; i < info.AttributeCount; ++i)
        {
            const RasterVertexAttribute& attribute = info.Attributes[i];
            vertex.attributes[attribute.Location].format =
                attribute.Format == RasterVertexFormat::Float2   ? MTLVertexFormatFloat2
                : attribute.Format == RasterVertexFormat::Float3 ? MTLVertexFormatFloat3
                                                                 : MTLVertexFormatFloat4;
            vertex.attributes[attribute.Location].offset = attribute.Offset;
            vertex.attributes[attribute.Location].bufferIndex = internal::RASTER_BINDINGS + attribute.Stream;
        }
        descriptor.vertexDescriptor = vertex;

        MTLRenderPipelineReflection* reflectionOrNull = nil;
        NSError* errorOrNull = nil;
        id<MTLRenderPipelineState> pipelineOrNull =
            [DeviceOrNull newRenderPipelineStateWithDescriptor:descriptor
                                                       options:BINDING_INFO | MTLPipelineOptionBufferTypeInfo
                                                    reflection:&reflectionOrNull
                                                         error:&errorOrNull];
        ErrorOrNull = errorOrNull;
        if (pipelineOrNull == nil || reflectionOrNull == nil ||
            RasterMetalBindings(reflectionOrNull.vertexBindings, Vertex.Info, info) == false ||
            RasterMetalBindings(reflectionOrNull.fragmentBindings, Fragment.Info, info) == false)
        {

            return RasterStatus::Failed;
        }
        MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
        const MTLCompareFunction comparisons[]{MTLCompareFunctionLess,
                                               MTLCompareFunctionLessEqual,
                                               MTLCompareFunctionGreater,
                                               MTLCompareFunctionGreaterEqual,
                                               MTLCompareFunctionAlways};
        depth.depthCompareFunction =
            info.Depth == true ? comparisons[static_cast<usize>(info.DepthCompare)] : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = info.Depth == true && info.DepthWrite == true;
        if (info.Stencil == true)
        {
            const MTLCompareFunction compare[]{MTLCompareFunctionNever,
                                               MTLCompareFunctionLess,
                                               MTLCompareFunctionEqual,
                                               MTLCompareFunctionLessEqual,
                                               MTLCompareFunctionGreater,
                                               MTLCompareFunctionNotEqual,
                                               MTLCompareFunctionGreaterEqual,
                                               MTLCompareFunctionAlways};
            const MTLStencilOperation operations[]{MTLStencilOperationKeep,
                                                   MTLStencilOperationZero,
                                                   MTLStencilOperationReplace,
                                                   MTLStencilOperationIncrementClamp,
                                                   MTLStencilOperationDecrementClamp,
                                                   MTLStencilOperationInvert,
                                                   MTLStencilOperationIncrementWrap,
                                                   MTLStencilOperationDecrementWrap};
            MTLStencilDescriptor* front = [MTLStencilDescriptor new];
            MTLStencilDescriptor* back = [MTLStencilDescriptor new];
            const RasterStencilFace faces[]{info.StencilFront, info.StencilBack};
            for (usize i = 0; i < 2; ++i)
            {
                MTLStencilDescriptor* face = i == 0 ? front : back;
                const RasterStencilFace& input = faces[i];
                face.stencilCompareFunction = compare[static_cast<usize>(input.Compare)];
                face.stencilFailureOperation = operations[static_cast<usize>(input.Fail)];
                face.depthFailureOperation = operations[static_cast<usize>(input.DepthFail)];
                face.depthStencilPassOperation = operations[static_cast<usize>(input.Pass)];
                face.readMask = info.StencilReadMask;
                face.writeMask = info.StencilWriteMask;
            }
            depth.frontFaceStencil = front;
            depth.backFaceStencil = back;
        }
        id<MTLDepthStencilState> stateOrNull = [DeviceOrNull newDepthStencilStateWithDescriptor:depth];
        if (stateOrNull == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        Pipeline = {pipelineOrNull, stateOrNull, info};
        return RasterStatus::Ready;
    }
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32 request) noexcept
{
    UniquePtr<MetalRasterPreparation> task = MakePreparationTask<MetalRasterPreparation>();
    if (task.Get() == nullptr)
    {
        return internal::StorageFailure();
    }
    task->IsPipelinePreparation = true;
    task->Kind = internal::RasterKind::Pipeline;
    task->DeviceOrNull = GetMetal().Device;
    task->Vertex = GetRasterMetal().Shaders[info.Vertex];
    task->Fragment = GetRasterMetal().Shaders[info.Fragment];
    task->Info = info;
    task->ResourceSlot = slot;
    task->Request = request;
    return SubmitNativePreparation(foundation::Move(task), info.ShouldPrepareAsynchronously);
}
// Thanks to Apple, "label", MTLResource documentation:
// https://developer.apple.com/documentation/metal/mtlresource/label
// Labels are copied strings; they never extend or alter the labelled object's lifetime.
void RasterSetDebugName(internal::RasterKind kind, usize slot, const char* name) noexcept
{
    @autoreleasepool
    {
        NSString* labelOrNull = name[0] == '\0' ? nil : [NSString stringWithUTF8String:name];
        if (kind == internal::RasterKind::Buffer)
        {
            GetRasterMetal().Buffers[slot].label = labelOrNull;
        }
        else if (kind == internal::RasterKind::Texture)
        {
            GetRasterMetal().Textures[slot].label = labelOrNull;
            GetRasterMetal().Depths[slot].label = labelOrNull;
        }
    }
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            GetRasterMetal().Buffers[slot] = nil;
            break;
        case internal::RasterKind::Texture:
            GetRasterMetal().Textures[slot] = nil;
            GetRasterMetal().Depths[slot] = nil;
            break;
        case internal::RasterKind::View:
            GetRasterMetal().Views[slot] = nil;
            GetRasterMetal().ViewDepths[slot] = nil;
            break;
        case internal::RasterKind::Sampler:
            GetRasterMetal().Samplers[slot] = nil;
            break;
        case internal::RasterKind::Shader:
            GetRasterMetal().Shaders[slot] = {};
            break;
        case internal::RasterKind::Layout:
            GetRasterMetal().Layouts[slot] = {};
            break;
        case internal::RasterKind::Set:
            GetRasterMetal().Sets[slot] = {};
            break;
        case internal::RasterKind::Pipeline:
            GetRasterMetal().Pipelines[slot] = {};
            break;
        case internal::RasterKind::ComputePipeline:
            GetRasterMetal().ComputePipelines[slot] = {};
            GetRasterMetal().ComputeInfos[slot] = {};
            break;
        case internal::RasterKind::SpatialUpscaler:
            SpatialUpscaleDestroy(slot);
            break;
        case internal::RasterKind::RayGeometry:
        case internal::RasterKind::RayScene:
        case internal::RasterKind::RayProgram:
        case internal::RasterKind::RayBatch:
            RayDestroy(kind, slot);
            break;
        case internal::RasterKind::Count:
            break;
    }
}
RasterStatus RasterDraw(const internal::RasterPacket& packet) noexcept
{
    const RasterMetalPipeline& pipeline = GetRasterMetal().Pipelines[packet.Pipeline];
    const internal::RasterSet& set = GetRasterMetal().Sets[packet.Set];
    const internal::RasterLayout& layout = set.Description;
    [GetMetal().Encoder setRenderPipelineState:pipeline.Object];
    [GetMetal().Encoder setDepthStencilState:pipeline.Depth];
    [GetMetal().Encoder setStencilReferenceValue:pipeline.Info.StencilReference];
    [GetMetal().Encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    for (usize i = 0; i < layout.DescriptorCount; ++i)
    {
        const RasterBinding& entry = layout.GetDescriptorBinding(i);
        const uint32 textureIndex = entry.Binding + layout.GetArrayElement(i);
        const bool vertex = (static_cast<uint8>(entry.Visibility) & 1) != 0;
        const bool fragment = (static_cast<uint8>(entry.Visibility) & 2) != 0;
        if (entry.Kind == RasterBindingKind::UniformBuffer || entry.Kind == RasterBindingKind::StorageRead ||
            entry.Kind == RasterBindingKind::StorageReadWrite)
        {
            if (vertex)
            {
                [GetMetal().Encoder setVertexBuffer:GetRasterMetal().Buffers[set.Slots[i]]
                                             offset:set.Offsets[i] + packet.BindingOffsets.Bytes[entry.Binding]
                                            atIndex:textureIndex];
            }
            if (fragment)
            {
                [GetMetal().Encoder setFragmentBuffer:GetRasterMetal().Buffers[set.Slots[i]]
                                               offset:set.Offsets[i] + packet.BindingOffsets.Bytes[entry.Binding]
                                              atIndex:textureIndex];
            }
        }
        else if (internal::ViewBinding(entry.Kind))
        {
            if (vertex)
            {
                [GetMetal().Encoder setVertexTexture:GetRasterMetal().Views[set.Slots[i]] atIndex:textureIndex];
            }
            if (fragment)
            {
                [GetMetal().Encoder setFragmentTexture:GetRasterMetal().Views[set.Slots[i]] atIndex:textureIndex];
            }
        }
        else
        {
            if (vertex)
            {
                [GetMetal().Encoder setVertexSamplerState:GetRasterMetal().Samplers[set.Slots[i]] atIndex:textureIndex];
            }
            if (fragment)
            {
                [GetMetal().Encoder setFragmentSamplerState:GetRasterMetal().Samplers[set.Slots[i]]
                                                    atIndex:textureIndex];
            }
        }
    }
    for (usize i = 0; i < pipeline.Info.StreamCount; ++i)
    {
        [GetMetal().Encoder setVertexBuffer:GetRasterMetal().Buffers[packet.Vertices[i]]
                                     offset:packet.Offsets[i]
                                    atIndex:internal::RASTER_BINDINGS + i];
    }
    [GetMetal().Encoder setCullMode:MTLCullModeNone];
    // Encoder state outlives the draw, so every packet states its own fill mode.
    [GetMetal().Encoder setTriangleFillMode:pipeline.Info.FillMode == RasterFillMode::Lines ? MTLTriangleFillModeLines
                                                                                            : MTLTriangleFillModeFill];
    internal::RasterArea area;
    const uint32 width = GetRasterMetal().Width == 0 ? GetMetal().Frame.Width : GetRasterMetal().Width;
    const uint32 height = GetRasterMetal().Height == 0 ? GetMetal().Frame.Height : GetRasterMetal().Height;
    if (internal::RasterResolveArea(GetRasterMetal().Region, width, height, area) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    if (area.Empty)
    {
        return RasterStatus::Ready;
    }
    const auto& v = area.Viewport;
    const auto& c = area.Scissor;
    [GetMetal().Encoder setViewport:MTLViewport{static_cast<float64>(v.X),
                                                static_cast<float64>(v.Y),
                                                static_cast<float64>(v.Width),
                                                static_cast<float64>(v.Height),
                                                0,
                                                1}];
    [GetMetal().Encoder setScissorRect:MTLScissorRect{c.X, c.Y, c.Width, c.Height}];
    if (packet.Indirect != internal::RASTER_INVALID)
    {
        // R12a: ordered single commands; native multi-draw is outside this profile.
        for (uint32 command = 0; command < packet.IndirectCount; ++command)
        {
            const usize offset = packet.IndirectOffset + usize{command} * packet.IndirectStride;
            if (packet.Indices == internal::RASTER_INVALID)
            {
                [GetMetal().Encoder drawPrimitives:MTLPrimitiveTypeTriangle
                                    indirectBuffer:GetRasterMetal().Buffers[packet.Indirect]
                              indirectBufferOffset:offset];
            }
            else
            {
                [GetMetal().Encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                                indexType:packet.Index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                              indexBuffer:GetRasterMetal().Buffers[packet.Indices]
                                        indexBufferOffset:packet.IndexOffset
                                           indirectBuffer:GetRasterMetal().Buffers[packet.Indirect]
                                     indirectBufferOffset:offset];
            }
        }
    }
    else
    {
        [GetMetal().Encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                       indexCount:packet.IndexCount
                                        indexType:packet.Index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                      indexBuffer:GetRasterMetal().Buffers[packet.Indices]
                                indexBufferOffset:packet.IndexOffset
                                    instanceCount:packet.InstanceCount];
    }
    return RasterStatus::Ready;
}
void RasterFrameExtent() noexcept
{
    GetRasterMetal().Width = GetRasterMetal().Height = 0;
    GetRasterMetal().Region = {};
}
void RasterEndPass() noexcept
{
    if (GetMetal().Encoder != nil)
    {
        ClosePassLabels();
        [GetMetal().Encoder endEncoding];
        GetMetal().Encoder = nil;
    }
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo& info) noexcept
{
    // A surface graph may be prepared before a frame is acquired. Surface depth takes its extent from
    // the acquired color texture, so RasterBeginPass creates it once a frame is open.
    if (info.Texture == internal::RASTER_INVALID && GetMetal().SurfaceTexture != nil &&
        GetSurfaceDepthOrNull(GetMetal().SurfaceTexture, info.Description.DepthLoad, info.Description.DepthStore) ==
            nil)
    {
        return RasterStatus::OutOfMemory;
    }
    return RasterStatus::Ready;
}
void RasterTextureBarrier(usize, RasterTextureUse) noexcept {}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    @autoreleasepool
    {
        const auto& state = info.Description;
        const bool surface = info.Texture == internal::RASTER_INVALID;
        id<MTLTexture> color = surface ? GetMetal().SurfaceTexture : GetRasterMetal().Textures[info.Texture];
        id<MTLTexture> depthOrNull = surface ? GetSurfaceDepthOrNull(color, state.DepthLoad, state.DepthStore)
                                     : info.ColorCount == 0 ? GetRasterMetal().Depths[info.Texture]
                                                            : GetRasterMetal().ViewDepths[info.Colors[0].View];
        if (surface == true && depthOrNull == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        auto attachment = pass.colorAttachments[0];
        attachment.texture = color;
        attachment.loadAction = state.ColorLoad == RasterLoad::Clear  ? MTLLoadActionClear
                                : state.ColorLoad == RasterLoad::Load ? MTLLoadActionLoad
                                                                      : MTLLoadActionDontCare;
        attachment.storeAction = state.ColorStore == RasterStore::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
        attachment.clearColor = MTLClearColorMake(static_cast<float64>(state.Clear[0]),
                                                  static_cast<float64>(state.Clear[1]),
                                                  static_cast<float64>(state.Clear[2]),
                                                  static_cast<float64>(state.Clear[3]));
        if (info.ColorCount != 0)
        {
            for (usize i = 0; i < info.ColorCount; ++i)
            {
                const auto& c = info.Colors[i];
                auto target = pass.colorAttachments[i];
                target.texture = GetRasterMetal().Views[c.View];
                target.loadAction = c.Load == RasterLoad::Clear  ? MTLLoadActionClear
                                    : c.Load == RasterLoad::Load ? MTLLoadActionLoad
                                                                 : MTLLoadActionDontCare;
                target.storeAction = c.Store == RasterStore::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
                target.clearColor = MTLClearColorMake(static_cast<float64>(c.Clear[0]),
                                                      static_cast<float64>(c.Clear[1]),
                                                      static_cast<float64>(c.Clear[2]),
                                                      static_cast<float64>(c.Clear[3]));
                if (c.ResolveView != internal::RASTER_INVALID)
                {
                    target.resolveTexture = GetRasterMetal().Views[c.ResolveView];
                    target.storeAction = c.Store == RasterStore::Store ? MTLStoreActionStoreAndMultisampleResolve
                                                                       : MTLStoreActionMultisampleResolve;
                }
            }
        }
        pass.depthAttachment.texture = depthOrNull;
        pass.depthAttachment.loadAction = state.DepthLoad == RasterLoad::Clear  ? MTLLoadActionClear
                                          : state.DepthLoad == RasterLoad::Load ? MTLLoadActionLoad
                                                                                : MTLLoadActionDontCare;
        pass.depthAttachment.storeAction =
            state.DepthStore == RasterStore::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = static_cast<float64>(state.ClearDepth);
        if (info.Stencil)
        {
            pass.stencilAttachment.texture = depthOrNull;
            pass.stencilAttachment.loadAction = state.StencilLoad == RasterLoad::Clear  ? MTLLoadActionClear
                                                : state.StencilLoad == RasterLoad::Load ? MTLLoadActionLoad
                                                                                        : MTLLoadActionDontCare;
            pass.stencilAttachment.storeAction =
                state.StencilStore == RasterStore::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
            pass.stencilAttachment.clearStencil = state.ClearStencil;
        }
        ConfigurePassTiming(pass);
        GetMetal().Encoder = [GetMetal().Command renderCommandEncoderWithDescriptor:pass];
        GetMetal().Encoder.label = GetGpuScopeMetal().PassLabelOrNull;
        GetRasterMetal().Region = state;
        GetRasterMetal().Width = surface ? static_cast<uint32>(color.width) : info.Width;
        GetRasterMetal().Height = surface ? static_cast<uint32>(color.height) : info.Height;
        return GetMetal().Encoder != nil ? RasterStatus::Ready : RasterStatus::Failed;
    }
}
RasterStatus RasterReserveSubmission() noexcept
{
    return RasterStatus::Ready;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    GetRasterMetal().Ordinals[(GetMetal().Slot + FRAMES - 1) % FRAMES] = ordinal;
}
uint64 RasterCompleted() noexcept
{
    PollNativePreparation();
    for (usize i = 0; i < FRAMES; ++i)
    {
        for (usize prefix = 0; prefix < GetMetal().CommandPrefixes[i].Count; ++prefix)
        {
            if (CheckCommand(GetMetal().CommandPrefixes[i].CommandsOrNull[prefix]) == false)
            {
                return GetRasterMetal().Completed;
            }
        }
        if (GetMetal().Submitted[i] != nil && CheckCommand(GetMetal().Submitted[i]) == false)
        {
            return GetRasterMetal().Completed;
        }
        if (GetMetal().Submitted[i] != nil && GetMetal().Submitted[i].status == MTLCommandBufferStatusCompleted &&
            GetRasterMetal().Ordinals[i] > GetRasterMetal().Completed)
        {
            GetRasterMetal().Completed = GetRasterMetal().Ordinals[i];
        }
    }
    return GetRasterMetal().Completed;
}
void RasterReset() noexcept
{
    // Foreign work may already be queued when a later graph encoder fails.
    // Submit the trailing RHI buffer before waiting/releasing any retained context.
    if (GetMetal().HasNativeComputeSplit == true && GetMetal().Command != nil)
    {
        ClosePassLabels();
        [GetMetal().Encoder endEncoding];
        GetMetal().Encoder = nil;
        PopScopeGroups();
        [GetMetal().Command commit];
        GetMetal().Submitted[GetMetal().Slot] = GetMetal().Command;
        GetMetal().Command = nil;
    }
    // Failed producers may never execute their GPU signal. Unblock joins during
    // fault cleanup, then wait every private completion before releasing storage.
    const StartupState startup = internal::GetStartup().State;
    if (startup == StartupState::Failed || startup == StartupState::DeviceLost)
    {
        for (MetalComputePipeline& pipeline : GetRasterMetal().ComputePipelines)
        {
            if (pipeline.NativeEventOrNull != nil)
            {
                pipeline.NativeEventOrNull.signaledValue = pipeline.NativeEventValue;
            }
        }
    }
    DrainNativePreparation();
    for (id<MTLCommandBuffer> command : GetMetal().Submitted)
    {
        [command waitUntilCompleted];
    }
    for (uint64& ordinal : GetRasterMetal().Ordinals)
    {
        ordinal = 0;
    }
    GetRasterMetal().Completed = 0;
    for (MetalCommandPrefixes& prefixes : GetMetal().CommandPrefixes)
    {
        for (usize index = 0; index < prefixes.Count; ++index)
        {
            [prefixes.CommandsOrNull[index] waitUntilCompleted];
        }
        prefixes = {};
    }
}
void RasterShutdown() noexcept {}

RasterStatus RasterGrow(internal::RasterKind kind, usize count) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            return GetRasterMetal().Buffers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Texture:
            return GetRasterMetal().Textures.EnsureSlots(count) && GetRasterMetal().Depths.EnsureSlots(count)
                       ? RasterStatus::Ready
                       : internal::StorageFailure();
        case internal::RasterKind::View:
            return GetRasterMetal().Views.EnsureSlots(count) && GetRasterMetal().ViewDepths.EnsureSlots(count)
                       ? RasterStatus::Ready
                       : internal::StorageFailure();
        case internal::RasterKind::Sampler:
            return GetRasterMetal().Samplers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Shader:
            return GetRasterMetal().Shaders.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Layout:
            return GetRasterMetal().Layouts.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Set:
            return GetRasterMetal().Sets.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Pipeline:
            return GetRasterMetal().Pipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::ComputePipeline:
            return GetRasterMetal().ComputePipelines.EnsureSlots(count) &&
                           GetRasterMetal().ComputeInfos.EnsureSlots(count)
                       ? RasterStatus::Ready
                       : internal::StorageFailure();
        default:
            return RasterStatus::InvalidDescription;
    }
}
RasterStatus RasterPrepareStorage() noexcept
{
    return RasterStatus::Ready;
}
} // namespace ludus::graphics::rhi::backend
