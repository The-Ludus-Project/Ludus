#pragma once

#include "profile.h"
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
    internal::RasterShaderInfo Info;
};
struct RasterMetalPipeline final
{
    id<MTLRenderPipelineState> Object = nil;
    id<MTLDepthStencilState> Depth = nil;
    internal::RasterPipelineInfo Info;
};
internal::StorageTable<id<MTLBuffer>> gRasterBuffers;
internal::StorageTable<id<MTLTexture>> gRasterTextures;
internal::StorageTable<id<MTLTexture>> gRasterDepths;
internal::StorageTable<id<MTLTexture>> gRasterViews;
internal::StorageTable<id<MTLTexture>> gRasterViewDepths;
internal::StorageTable<id<MTLSamplerState>> gRasterSamplers;
internal::StorageTable<RasterMetalShader> gRasterShaders;
internal::StorageTable<internal::RasterLayout> gRasterLayouts;
internal::StorageTable<internal::RasterSet> gRasterSets;
internal::StorageTable<RasterMetalPipeline> gRasterPipelines;
internal::StorageTable<id<MTLComputePipelineState>> gComputePipelines;
internal::StorageTable<internal::ComputePipelineInfo> gComputeInfos;
uint64 gRasterOrdinals[FRAMES]{};
uint64 gRasterCompleted = 0;
uint32 gRasterWidth = 0, gRasterHeight = 0;
RasterPassDescription gRasterRegion{};
bool RasterMetalBindings(NSArray<id<MTLBinding>>* bindings,
                         const internal::RasterShaderInfo& shader,
                         const internal::RasterPipelineInfo& pipeline) noexcept
{
    for (id<MTLBinding> binding in bindings)
    {
        if (!binding.used)
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
        }
        if (!found)
        {
            LUDUS_LOG_WARN(LOG_RHI,
                           "Raster Metal binding {} index {} type {} is absent from stage reflection",
                           binding.name.UTF8String,
                           static_cast<uint64>(binding.index),
                           static_cast<uint64>(binding.type));
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
    if (gDevice == nil || (internal::CompressedTexture(format) && !gDevice.supportsBCTextureCompression))
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
        result.SampleCounts = 1U | ([gDevice supportsTextureSampleCount:4] ? 4U : 0U);
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
    if (gDevice == nil)
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
    const usize max = internal::GetResourceSizeLimit(gDevice.maxBufferLength);
    return
    {
        .MaxBufferSize = gDevice.maxBufferLength < max ? gDevice.maxBufferLength : max,
        .MaxTextureDimension2D = MAX_DIMENSION,
        .UniformOffsetAlignment = 256,
        .MaxUniformRange = 16384,
        .ResourcesPerKind = internal::CurrentStorageLimits().ResourcesPerKind,
        .DrawsPerFrame = internal::CurrentStorageLimits().DrawsPerFrame,
        .MaxVertexAttributes = 16,
        .HdrColor = true,
        .MaxTextureUploadBytes = max,
    };
}
// Thanks to Apple, "MTLComputeCommandEncoder" and Metal's tracked-resource
// synchronization: encoder boundaries order storage writes before raster reads.
// https://developer.apple.com/documentation/metal/mtlcomputecommandencoder
ComputeCapabilities ComputeLimits() noexcept
{
    if (gDevice == nil)
    {
        return {};
    }
    const MTLSize threads = gDevice.maxThreadsPerThreadgroup;
    const usize range = RasterLimits().MaxBufferSize;
    return {gDevice.maxBufferLength < range ? gDevice.maxBufferLength : range,
            256,
            {static_cast<uint32>(threads.width < 256 ? threads.width : 256),
             static_cast<uint32>(threads.height < 256 ? threads.height : 256),
             static_cast<uint32>(threads.depth < 64 ? threads.depth : 64)},
            256,
            {65535, 65535, 65535},
            static_cast<uint32>(internal::RASTER_BINDINGS)};
}
RasterStatus ComputeCreatePipeline(usize slot, const internal::ComputePipelineInfo& info, uint32) noexcept
{
    @autoreleasepool
    {
        NSError* error = nil;
        MTLComputePipelineReflection* reflection = nil;
        auto pipeline = [gDevice newComputePipelineStateWithFunction:gRasterShaders[info.Shader].Function
                                                             options:BINDING_INFO | MTLPipelineOptionBufferTypeInfo
                                                          reflection:&reflection
                                                               error:&error];
        const uint32 threads = info.WorkgroupSize[0] * info.WorkgroupSize[1] * info.WorkgroupSize[2];
        if (pipeline == nil || reflection == nil || threads > pipeline.maxTotalThreadsPerThreadgroup ||
            !RasterMetalBindings(reflection.bindings, gRasterShaders[info.Shader].Info, {}))
        {
            LogError(error);
            return RasterStatus::Failed;
        }
        gComputePipelines[slot] = pipeline;
        gComputeInfos[slot] = info;
        return RasterStatus::Ready;
    }
}
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    auto encoder = [gCommand computeCommandEncoder];
    if (encoder == nil)
    {
        return RasterStatus::OutOfMemory;
    }
    [encoder setComputePipelineState:gComputePipelines[packet.Pipeline]];
    const auto& set = gRasterSets[packet.Set];
    const internal::RasterLayout& layout = set.Description;
    for (usize i = 0; i < layout.Count; ++i)
    {
        [encoder setBuffer:gRasterBuffers[set.Slots[i]]
                    offset:set.Offsets[i] + packet.BindingOffsets.Bytes[layout.Entries[i].Binding]
                   atIndex:layout.Entries[i].Binding];
    }
    const auto& info = gComputeInfos[packet.Pipeline];
    [encoder dispatchThreadgroups:MTLSizeMake(packet.Groups[0], packet.Groups[1], packet.Groups[2])
            threadsPerThreadgroup:MTLSizeMake(info.WorkgroupSize[0], info.WorkgroupSize[1], info.WorkgroupSize[2])];
    [encoder endEncoding];
    return RasterStatus::Ready;
}
void ComputeBufferBarrier(usize, GraphAccessMode) noexcept {}
RasterStatus RasterCreateBuffer(usize slot, const BufferDescription&, std::span<const uint8> bytes, uint32) noexcept
{
    @autoreleasepool
    {
        gRasterBuffers[slot] = [gDevice newBufferWithBytes:bytes.data()
                                                    length:bytes.size()
                                                   options:MTLResourceStorageModeShared];
    }
    return gRasterBuffers[slot] != nil ? RasterStatus::Ready : RasterStatus::OutOfMemory;
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
                           (info.Attachment ? MTLTextureUsageRenderTarget : MTLTextureUsageUnknown);
        descriptor.storageMode = info.Attachment                            ? MTLStorageModePrivate
                                 : internal::CompressedTexture(info.Format) ? MTLStorageModeManaged
                                                                            : MTLStorageModeShared;
        auto texture = [gDevice newTextureWithDescriptor:descriptor];
        if (texture == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        if (!info.Attachment && !info.DeferredInitialization)
        {
            uint32 width = info.Width, height = info.Height;
            for (usize level = 0; level < info.MipLevels; ++level)
            {
                const auto mip = upload.Mips.empty() ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[level];
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
                                 withBytes:upload.Bytes.data() + mip.Offset
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
                                     withBytes:upload.Bytes.data() + mip.Offset + slice * imagePitch
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
            gRasterDepths[slot] = [gDevice newTextureWithDescriptor:depth];
            if (gRasterDepths[slot] == nil)
            {
                return RasterStatus::OutOfMemory;
            }
        }
        gRasterTextures[slot] = texture;
        return RasterStatus::Ready;
    }
}
// Thanks to Apple, MTLTexture.newTextureViewWithPixelFormat:textureType:levels:slices:,
// https://developer.apple.com/documentation/metal/mtltexture, for original-format
// alias views with explicit mip and slice lifetimes. R9 retains the source allocation.
RasterStatus RasterCreateView(usize slot, const internal::RasterViewSource& source, uint32) noexcept
{
    auto texture =
        source.View.Aspect == TextureAspect::Color ? gRasterTextures[source.Texture] : gRasterDepths[source.Texture];
    const auto format = source.View.Aspect == TextureAspect::Stencil ? MTLPixelFormatX32_Stencil8 : texture.pixelFormat;
    const auto type =
        texture.sampleCount == 4 ? MTLTextureType2DMultisample : RasterMetalTextureType(source.View.Dimension);
    gRasterViews[slot] =
        [texture newTextureViewWithPixelFormat:format
                                   textureType:type
                                        levels:NSMakeRange(source.View.BaseMip, source.View.MipCount)
                                        slices:NSMakeRange(source.View.BaseLayer, source.View.LayerCount)];
    if (source.View.Attachment)
    {
        auto depth = gRasterDepths[source.Texture];
        gRasterViewDepths[slot] = [depth newTextureViewWithPixelFormat:depth.pixelFormat
                                                           textureType:type
                                                                levels:NSMakeRange(source.View.BaseMip, 1)
                                                                slices:NSMakeRange(source.View.BaseLayer, 1)];
        if (gRasterViewDepths[slot] == nil)
        {
            return RasterStatus::OutOfMemory;
        }
    }
    return gRasterViews[slot] != nil ? RasterStatus::Ready : RasterStatus::OutOfMemory;
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
        gRasterSamplers[slot] = [gDevice newSamplerStateWithDescriptor:descriptor];
        return gRasterSamplers[slot] != nil ? RasterStatus::Ready : RasterStatus::OutOfMemory;
    }
}
RasterStatus RasterCreateShader(usize slot,
                                const ShaderDescription& artifact,
                                const internal::RasterShaderInfo& info,
                                uint32) noexcept
{
    @autoreleasepool
    {
        NSString* source = [[NSString alloc] initWithBytes:artifact.Msl.data()
                                                    length:artifact.Msl.size()
                                                  encoding:NSUTF8StringEncoding];
        NSString* name = [[NSString alloc] initWithBytes:artifact.MslEntry.data()
                                                  length:artifact.MslEntry.size()
                                                encoding:NSUTF8StringEncoding];
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_3;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:source options:options error:&error];
        id<MTLFunction> function = [library newFunctionWithName:name];
        if (function == nil || function.functionType != (info.Stage == ShaderStage::Vertex     ? MTLFunctionTypeVertex
                                                         : info.Stage == ShaderStage::Fragment ? MTLFunctionTypeFragment
                                                                                               : MTLFunctionTypeKernel))
        {
            LogError(error);
            return RasterStatus::Failed;
        }
        gRasterShaders[slot] = {function, info};
        return RasterStatus::Ready;
    }
}
RasterStatus RasterCreateLayout(usize slot, const internal::RasterLayout& info, uint32) noexcept
{
    gRasterLayouts[slot] = info;
    return RasterStatus::Ready;
}
RasterStatus
RasterCreateSet(usize slot, const internal::RasterLayout&, const internal::RasterSet& info, uint32) noexcept
{
    return gRasterSets[slot].TryCopyFrom(info) == true ? RasterStatus::Ready : internal::StorageFailure();
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32) noexcept
{
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = gRasterShaders[info.Vertex].Function;
        descriptor.fragmentFunction = gRasterShaders[info.Fragment].Function;
        descriptor.colorAttachments[0].pixelFormat =
            info.Target == RasterTarget::Rgba16Float  ? MTLPixelFormatRGBA16Float
            : info.Target == RasterTarget::Surface    ? MTLPixelFormatBGRA8Unorm
            : info.Target == RasterTarget::Rgba8Unorm ? MTLPixelFormatRGBA8Unorm
                                                      : MTLPixelFormatRGBA8Unorm_sRGB;
        descriptor.rasterSampleCount = info.SampleCount;
        descriptor.depthAttachmentPixelFormat =
            info.Stencil ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
        if (info.Stencil)
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
            auto color = descriptor.colorAttachments[i];
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
                info.Streams[i].PerInstance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            vertex.layouts[binding].stepRate = 1;
        }
        for (usize i = 0; i < info.AttributeCount; ++i)
        {
            const auto& attribute = info.Attributes[i];
            vertex.attributes[attribute.Location].format =
                attribute.Format == RasterVertexFormat::Float2   ? MTLVertexFormatFloat2
                : attribute.Format == RasterVertexFormat::Float3 ? MTLVertexFormatFloat3
                                                                 : MTLVertexFormatFloat4;
            vertex.attributes[attribute.Location].offset = attribute.Offset;
            vertex.attributes[attribute.Location].bufferIndex = internal::RASTER_BINDINGS + attribute.Stream;
        }
        descriptor.vertexDescriptor = vertex;
        NSError* error = nil;
        MTLRenderPipelineReflection* reflection = nil;
        auto pipeline = [gDevice newRenderPipelineStateWithDescriptor:descriptor
                                                              options:BINDING_INFO | MTLPipelineOptionBufferTypeInfo
                                                           reflection:&reflection
                                                                error:&error];
        if (pipeline == nil || reflection == nil ||
            !RasterMetalBindings(reflection.vertexBindings, gRasterShaders[info.Vertex].Info, info) ||
            !RasterMetalBindings(reflection.fragmentBindings, gRasterShaders[info.Fragment].Info, info))
        {
            LogError(error);
            return RasterStatus::Failed;
        }
        MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
        const MTLCompareFunction comparisons[]{MTLCompareFunctionLess,
                                               MTLCompareFunctionLessEqual,
                                               MTLCompareFunctionGreater,
                                               MTLCompareFunctionGreaterEqual,
                                               MTLCompareFunctionAlways};
        depth.depthCompareFunction =
            info.Depth ? comparisons[static_cast<usize>(info.DepthCompare)] : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = info.Depth && info.DepthWrite;
        if (info.Stencil)
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
            auto front = [MTLStencilDescriptor new];
            auto back = [MTLStencilDescriptor new];
            const RasterStencilFace faces[]{info.StencilFront, info.StencilBack};
            for (usize i = 0; i < 2; ++i)
            {
                auto face = i == 0 ? front : back;
                const auto& input = faces[i];
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
        auto state = [gDevice newDepthStencilStateWithDescriptor:depth];
        if (state == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        gRasterPipelines[slot] = {pipeline, state, info};
        return RasterStatus::Ready;
    }
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            gRasterBuffers[slot] = nil;
            break;
        case internal::RasterKind::Texture:
            gRasterTextures[slot] = nil;
            gRasterDepths[slot] = nil;
            break;
        case internal::RasterKind::View:
            gRasterViews[slot] = nil;
            gRasterViewDepths[slot] = nil;
            break;
        case internal::RasterKind::Sampler:
            gRasterSamplers[slot] = nil;
            break;
        case internal::RasterKind::Shader:
            gRasterShaders[slot] = {};
            break;
        case internal::RasterKind::Layout:
            gRasterLayouts[slot] = {};
            break;
        case internal::RasterKind::Set:
            gRasterSets[slot] = {};
            break;
        case internal::RasterKind::Pipeline:
            gRasterPipelines[slot] = {};
            break;
        case internal::RasterKind::ComputePipeline:
            gComputePipelines[slot] = nil;
            gComputeInfos[slot] = {};
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
    const auto& pipeline = gRasterPipelines[packet.Pipeline];
    const auto& set = gRasterSets[packet.Set];
    const internal::RasterLayout& layout = set.Description;
    [gEncoder setRenderPipelineState:pipeline.Object];
    [gEncoder setDepthStencilState:pipeline.Depth];
    [gEncoder setStencilReferenceValue:pipeline.Info.StencilReference];
    [gEncoder setFrontFacingWinding:MTLWindingCounterClockwise];
    for (usize i = 0; i < layout.DescriptorCount; ++i)
    {
        const RasterBinding& entry = layout.GetDescriptorBinding(i);
        const uint32 textureIndex = entry.Binding + layout.GetArrayElement(i);
        const bool vertex = (static_cast<uint8>(entry.Visibility) & 1) != 0;
        const bool fragment = (static_cast<uint8>(entry.Visibility) & 2) != 0;
        if (entry.Kind == RasterBindingKind::UniformBuffer)
        {
            if (vertex)
            {
                [gEncoder setVertexBuffer:gRasterBuffers[set.Slots[i]]
                                   offset:set.Offsets[i] + packet.BindingOffsets.Bytes[entry.Binding]
                                  atIndex:textureIndex];
            }
            if (fragment)
            {
                [gEncoder setFragmentBuffer:gRasterBuffers[set.Slots[i]]
                                     offset:set.Offsets[i] + packet.BindingOffsets.Bytes[entry.Binding]
                                    atIndex:textureIndex];
            }
        }
        else if (internal::TextureBinding(entry.Kind))
        {
            if (vertex)
            {
                [gEncoder setVertexTexture:gRasterViews[set.Slots[i]] atIndex:textureIndex];
            }
            if (fragment)
            {
                [gEncoder setFragmentTexture:gRasterViews[set.Slots[i]] atIndex:textureIndex];
            }
        }
        else
        {
            if (vertex)
            {
                [gEncoder setVertexSamplerState:gRasterSamplers[set.Slots[i]] atIndex:textureIndex];
            }
            if (fragment)
            {
                [gEncoder setFragmentSamplerState:gRasterSamplers[set.Slots[i]] atIndex:textureIndex];
            }
        }
    }
    for (usize i = 0; i < pipeline.Info.StreamCount; ++i)
    {
        [gEncoder setVertexBuffer:gRasterBuffers[packet.Vertices[i]]
                           offset:packet.Offsets[i]
                          atIndex:internal::RASTER_BINDINGS + i];
    }
    [gEncoder setCullMode:MTLCullModeNone];
    internal::RasterArea area;
    const auto width = gRasterWidth == 0 ? gFrame.Width : gRasterWidth;
    const auto height = gRasterHeight == 0 ? gFrame.Height : gRasterHeight;
    if (!internal::RasterResolveArea(gRasterRegion, width, height, area))
    {
        return RasterStatus::InvalidDescription;
    }
    if (area.Empty)
    {
        return RasterStatus::Ready;
    }
    const auto& v = area.Viewport;
    const auto& c = area.Scissor;
    [gEncoder setViewport:MTLViewport{static_cast<float64>(v.X),
                                      static_cast<float64>(v.Y),
                                      static_cast<float64>(v.Width),
                                      static_cast<float64>(v.Height),
                                      0,
                                      1}];
    [gEncoder setScissorRect:MTLScissorRect{c.X, c.Y, c.Width, c.Height}];
    if (packet.Indirect != internal::RASTER_INVALID)
    {
        [gEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                              indexType:packet.Index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                            indexBuffer:gRasterBuffers[packet.Indices]
                      indexBufferOffset:packet.IndexOffset
                         indirectBuffer:gRasterBuffers[packet.Indirect]
                   indirectBufferOffset:packet.IndirectOffset];
    }
    else
    {
        [gEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:packet.IndexCount
                              indexType:packet.Index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                            indexBuffer:gRasterBuffers[packet.Indices]
                      indexBufferOffset:packet.IndexOffset
                          instanceCount:packet.InstanceCount];
    }
    return RasterStatus::Ready;
}
void RasterFrameExtent() noexcept
{
    gRasterWidth = gRasterHeight = 0;
    gRasterRegion = {};
}
void RasterEndPass() noexcept
{
    if (gEncoder != nil)
    {
        [gEncoder endEncoding];
        gEncoder = nil;
    }
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo&) noexcept
{
    return RasterStatus::Ready;
}
void RasterTextureBarrier(usize, RasterTextureUse) noexcept {}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    @autoreleasepool
    {
        const auto& state = info.Description;
        const bool surface = info.Texture == internal::RASTER_INVALID;
        auto color = surface ? (gDrawable != nil ? gDrawable.texture : gHeadless) : gRasterTextures[info.Texture];
        auto depth = surface                ? gDepth
                     : info.ColorCount == 0 ? gRasterDepths[info.Texture]
                                            : gRasterViewDepths[info.Colors[0].View];
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
                target.texture = gRasterViews[c.View];
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
                    target.resolveTexture = gRasterViews[c.ResolveView];
                    target.storeAction = c.Store == RasterStore::Store ? MTLStoreActionStoreAndMultisampleResolve
                                                                       : MTLStoreActionMultisampleResolve;
                }
            }
        }
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = state.DepthLoad == RasterLoad::Clear  ? MTLLoadActionClear
                                          : state.DepthLoad == RasterLoad::Load ? MTLLoadActionLoad
                                                                                : MTLLoadActionDontCare;
        pass.depthAttachment.storeAction =
            state.DepthStore == RasterStore::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = static_cast<float64>(state.ClearDepth);
        if (info.Stencil)
        {
            pass.stencilAttachment.texture = depth;
            pass.stencilAttachment.loadAction = state.StencilLoad == RasterLoad::Clear  ? MTLLoadActionClear
                                                : state.StencilLoad == RasterLoad::Load ? MTLLoadActionLoad
                                                                                        : MTLLoadActionDontCare;
            pass.stencilAttachment.storeAction =
                state.StencilStore == RasterStore::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
            pass.stencilAttachment.clearStencil = state.ClearStencil;
        }
        gEncoder = [gCommand renderCommandEncoderWithDescriptor:pass];
        gRasterRegion = state;
        gRasterWidth = surface ? static_cast<uint32>(color.width) : info.Width;
        gRasterHeight = surface ? static_cast<uint32>(color.height) : info.Height;
        return gEncoder != nil ? RasterStatus::Ready : RasterStatus::Failed;
    }
}
RasterStatus RasterReserveSubmission() noexcept
{
    return RasterStatus::Ready;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    gRasterOrdinals[(gSlot + FRAMES - 1) % FRAMES] = ordinal;
}
uint64 RasterCompleted() noexcept
{
    for (usize i = 0; i < FRAMES; ++i)
    {
        if (gSubmitted[i] != nil && !CheckCommand(gSubmitted[i]))
        {
            return gRasterCompleted;
        }
        if (gSubmitted[i] != nil && gSubmitted[i].status == MTLCommandBufferStatusCompleted &&
            gRasterOrdinals[i] > gRasterCompleted)
        {
            gRasterCompleted = gRasterOrdinals[i];
        }
    }
    return gRasterCompleted;
}
void RasterReset() noexcept
{
    for (auto command : gSubmitted)
    {
        [command waitUntilCompleted];
    }
    for (auto& ordinal : gRasterOrdinals)
    {
        ordinal = 0;
    }
    gRasterCompleted = 0;
}
void RasterShutdown() noexcept {}

RasterStatus RasterGrow(internal::RasterKind kind, usize count) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            return gRasterBuffers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Texture:
            return gRasterTextures.EnsureSlots(count) && gRasterDepths.EnsureSlots(count) ? RasterStatus::Ready
                                                                                          : internal::StorageFailure();
        case internal::RasterKind::View:
            return gRasterViews.EnsureSlots(count) && gRasterViewDepths.EnsureSlots(count) ? RasterStatus::Ready
                                                                                           : internal::StorageFailure();
        case internal::RasterKind::Sampler:
            return gRasterSamplers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Shader:
            return gRasterShaders.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Layout:
            return gRasterLayouts.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Set:
            return gRasterSets.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Pipeline:
            return gRasterPipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::ComputePipeline:
            return gComputePipelines.EnsureSlots(count) && gComputeInfos.EnsureSlots(count)
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
