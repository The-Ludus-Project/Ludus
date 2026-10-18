#pragma once

#include "profile.h"
// Included only by the WebGPU backend. Thanks to W3C, WebGPU, sections
// "Error Scopes" and "GPUQueue" (validation and ordered completion):
// https://www.w3.org/TR/webgpu/#error-scopes and https://www.w3.org/TR/webgpu/#gpuqueue
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
{
namespace
{
struct RasterWebShader final
{
    WGPUShaderModule Object = nullptr;
    char Entry[64]{};
};
struct RasterWebPipeline final
{
    WGPURenderPipeline Object = nullptr;
    WGPUPipelineLayout Layout = nullptr;
    internal::RasterPipelineInfo Info;
    uint32 Request = 0;
};
struct RasterWebCompletion final
{
    uint64 Ordinal = 0;
    uint32 Session = 0;
    bool Busy = false;
    bool Submitted = false;
};
internal::StorageTable<WGPUBuffer> gRasterBuffers;
internal::StorageTable<WGPUTexture> gRasterTextures;
internal::StorageTable<TextureDescription> gRasterTextureDescriptions;
internal::StorageTable<WGPUTexture> gRasterDepths;
internal::StorageTable<WGPUTextureView> gRasterAttachmentViews;
internal::StorageTable<WGPUTextureView> gRasterViewDepths;
internal::StorageTable<WGPUTextureView> gRasterDepthViews;
internal::StorageTable<WGPUTextureView> gRasterViews;
internal::StorageTable<WGPUSampler> gRasterSamplers;
internal::StorageTable<RasterWebShader> gRasterShaders;
internal::StorageTable<WGPUBindGroupLayout> gRasterLayouts;
internal::StorageTable<WGPUBindGroup> gRasterSets;
internal::StorageTable<RasterWebPipeline> gRasterPipelines;
struct ComputeWebPipeline final
{
    WGPUComputePipeline Object = nullptr;
    WGPUPipelineLayout Layout = nullptr;
    uint32 Request = 0;
};
internal::StorageTable<ComputeWebPipeline> gComputePipelines;
internal::StorageTable<RasterWebCompletion> gRasterCompletions;
RasterWebCompletion* gRasterReserved = nullptr;
uint64 gRasterCompleted = 0;
void RasterValidated(WGPUPopErrorScopeStatus status,
                     WGPUErrorType error,
                     WGPUStringView message,
                     void* request,
                     void*) noexcept
{
    const auto result = status != WGPUPopErrorScopeStatus_Success ? RasterStatus::Failed
                        : error == WGPUErrorType_NoError          ? RasterStatus::Ready
                        : error == WGPUErrorType_OutOfMemory      ? RasterStatus::OutOfMemory
                                                                  : RasterStatus::Failed;
    if (result != RasterStatus::Ready)
    {
        Diagnose(message);
    }
    internal::RasterComplete(Token(request), result);
}
void RasterPipelineCreated(WGPUCreatePipelineAsyncStatus status,
                           WGPURenderPipeline object,
                           WGPUStringView message,
                           void* userdata,
                           void*) noexcept
{
    const auto request = Token(userdata);
    bool adopted = false;
    for (auto& pipeline : gRasterPipelines)
    {
        if (pipeline.Request == request)
        {
            pipeline.Object = object;
            adopted = true;
            break;
        }
    }
    if (adopted == false && object != nullptr)
    {
        wgpuRenderPipelineRelease(object);
    }
    if (status != WGPUCreatePipelineAsyncStatus_Success)
    {
        Diagnose(message);
    }
    internal::RasterComplete(request,
                             status == WGPUCreatePipelineAsyncStatus_Success && object != nullptr
                                 ? RasterStatus::Ready
                                 : RasterStatus::Failed);
}
void ComputePipelineCreated(WGPUCreatePipelineAsyncStatus status,
                            WGPUComputePipeline object,
                            WGPUStringView message,
                            void* userdata,
                            void*) noexcept
{
    const auto request = Token(userdata);
    bool adopted = false;
    for (auto& pipeline : gComputePipelines)
    {
        if (pipeline.Request == request)
        {
            pipeline.Object = object;
            adopted = true;
            break;
        }
    }
    if (adopted == false && object != nullptr)
    {
        wgpuComputePipelineRelease(object);
    }
    if (status != WGPUCreatePipelineAsyncStatus_Success)
    {
        Diagnose(message);
    }
    internal::RasterComplete(request,
                             status == WGPUCreatePipelineAsyncStatus_Success && object != nullptr
                                 ? RasterStatus::Ready
                                 : RasterStatus::Failed);
}
void RasterScopes(uint32 request) noexcept
{
    internal::RasterExpect(request, internal::RasterCallbacks::Two);
    wgpuDevicePushErrorScope(gDevice, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(gDevice, WGPUErrorFilter_Validation);
}
RasterStatus RasterPop(uint32 request) noexcept
{
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = RasterValidated;
    callback.userdata1 = Userdata(request);
    (void)wgpuDevicePopErrorScope(gDevice, callback);
    (void)wgpuDevicePopErrorScope(gDevice, callback);
    return RasterStatus::Pending;
}
void RasterDone(WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata, void*) noexcept
{
    auto& completion = *static_cast<RasterWebCompletion*>(userdata);
    const auto session = completion.Session;
    const auto ordinal = completion.Ordinal;
    completion = {};
    if (internal::Current(session) == false)
    {
        return;
    }
    if (status == WGPUQueueWorkDoneStatus_Success)
    {
        if (ordinal > gRasterCompleted)
        {
            gRasterCompleted = ordinal;
        }
    }
    else
    {
        internal::Fail(session, StartupError::DeviceLost);
    }
}
} // namespace
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
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (gDevice == nullptr || wgpuDeviceGetLimits(gDevice, &limits) != WGPUStatus_Success)
    {
        return {};
    }
    return
    {
        .MaxDynamicUniformBuffers = limits.maxDynamicUniformBuffersPerPipelineLayout < internal::RASTER_BINDINGS
                                        ? limits.maxDynamicUniformBuffersPerPipelineLayout
                                        : static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxDynamicStorageBuffers = limits.maxDynamicStorageBuffersPerPipelineLayout < internal::RASTER_BINDINGS
                                        ? limits.maxDynamicStorageBuffersPerPipelineLayout
                                        : static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxTextureArrayElements = 1,
        .MaxDescriptorsPerSet = internal::CurrentStorageLimits().DescriptorsPerSet < internal::RASTER_BINDINGS
                                    ? internal::CurrentStorageLimits().DescriptorsPerSet
                                    : static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxBindings = limits.maxBindingsPerBindGroup < internal::RASTER_BINDINGS
                           ? limits.maxBindingsPerBindGroup
                           : static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxUniformBuffersPerStage = limits.maxUniformBuffersPerShaderStage,
        .MaxSampledTexturesPerStage = limits.maxSampledTexturesPerShaderStage,
        .MaxSamplersPerStage = limits.maxSamplersPerShaderStage,
        .MaxUniformBuffersPerSet = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxSamplersPerSet = static_cast<uint32>(internal::RASTER_BINDINGS),
    };
}
RasterCapabilities RasterLimits() noexcept
{
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (gDevice == nullptr || wgpuDeviceGetLimits(gDevice, &limits) != WGPUStatus_Success ||
        limits.maxVertexBuffers < 2 || limits.maxVertexAttributes < 8 || limits.maxVertexBufferArrayStride < 256 ||
        limits.maxBindingsPerBindGroup < internal::RASTER_BINDINGS || limits.maxUniformBuffersPerShaderStage < 8 ||
        limits.maxSampledTexturesPerShaderStage < 8 || limits.maxSamplersPerShaderStage < 8)
    {
        return {};
    }
    const usize max = internal::GetResourceSizeLimit(limits.maxBufferSize);
    return {static_cast<usize>(limits.maxBufferSize < max ? limits.maxBufferSize : max),
            limits.maxTextureDimension2D,
            limits.minUniformBufferOffsetAlignment > 256 ? limits.minUniformBufferOffsetAlignment : 256,
            static_cast<usize>(limits.maxUniformBufferBindingSize < 16384 ? limits.maxUniformBufferBindingSize : 16384),
            internal::CurrentStorageLimits().ResourcesPerKind,
            internal::CurrentStorageLimits().DrawsPerFrame,
            limits.maxVertexAttributes < 16 ? limits.maxVertexAttributes : 16,
            true,
            max};
}
// Thanks to W3C GPU for the Web Working Group, "WebGPU", texture format capabilities.
// https://www.w3.org/TR/webgpu/#texture-format-caps
// Intersect guaranteed normalized formats and enabled HDR with implemented uses;
// no storage/copy/MSAA or R8/RG8 attachment path is advertised.
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    if (gDevice == nullptr || (internal::CompressedTexture(format) &&
                               wgpuDeviceHasFeature(gDevice, WGPUFeatureName_TextureCompressionBC) == false))
    {
        return {};
    }
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (wgpuDeviceGetLimits(gDevice, &limits) != WGPUStatus_Success)
    {
        return {};
    }
    FormatCapabilities result = internal::PortableFormatLimits(format, RasterLimits());
    if (result.Uses.ColorAttachment)
    {
        result.MaxAttachmentMipLevels = result.MaxMipLevels;
        result.MaxAttachmentLayers = result.MaxArrayLayers;
        result.AttachmentViews = true;
        result.DepthSampled = true;
        result.StencilAttachment = true;
        result.StencilSampled = true;
        result.SampleCounts = 5;
    }
    if (result.Uses.Sampled)
    {
        result.MaxArrayLayers = limits.maxTextureArrayLayers;
        result.MaxCubeDimension = result.MaxArrayLayers >= 6 ? result.MaxSampledDimension2D : 0U;
        result.MaxDimension3D = internal::CompressedTexture(format) ? 0U : limits.maxTextureDimension3D;
        result.PartialViews = true;
    }
    result.MaxAttachmentLayers = result.Uses.ColorAttachment ? result.MaxArrayLayers : 0;
    result.MaxAttachmentCubeDimension = result.Uses.ColorAttachment ? result.MaxCubeDimension : 0;
    return result;
}

ComputeCapabilities ComputeLimits() noexcept
{
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (gDevice == nullptr || wgpuDeviceGetLimits(gDevice, &limits) != WGPUStatus_Success ||
        limits.maxStorageBuffersPerShaderStage < 4 || limits.maxComputeInvocationsPerWorkgroup < 256)
    {
        return {};
    }
    const uint64 range = RasterLimits().MaxBufferSize;
    const auto dispatch =
        limits.maxComputeWorkgroupsPerDimension < 65535 ? limits.maxComputeWorkgroupsPerDimension : 65535;
    return {static_cast<usize>(limits.maxStorageBufferBindingSize < range ? limits.maxStorageBufferBindingSize : range),
            limits.minStorageBufferOffsetAlignment > 256 ? limits.minStorageBufferOffsetAlignment : 256,
            {limits.maxComputeWorkgroupSizeX < 256 ? limits.maxComputeWorkgroupSizeX : 256,
             limits.maxComputeWorkgroupSizeY < 256 ? limits.maxComputeWorkgroupSizeY : 256,
             limits.maxComputeWorkgroupSizeZ < 64 ? limits.maxComputeWorkgroupSizeZ : 64},
            256,
            {dispatch, dispatch, dispatch},
            limits.maxStorageBuffersPerShaderStage < internal::RASTER_BINDINGS
                ? limits.maxStorageBuffersPerShaderStage
                : static_cast<uint32>(internal::RASTER_BINDINGS)};
}
RasterStatus ComputeCreatePipeline(usize slot, const internal::ComputePipelineInfo& info, uint32 request) noexcept
{
    RasterScopes(request);
    internal::RasterExpect(request, internal::RasterCallbacks::Three);
    auto& pipeline = gComputePipelines[slot];
    pipeline.Request = request;
    WGPUPipelineLayoutDescriptor layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout.bindGroupLayoutCount = 1;
    layout.bindGroupLayouts = &gRasterLayouts[info.Layout];
    pipeline.Layout = wgpuDeviceCreatePipelineLayout(gDevice, &layout);
    WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    descriptor.layout = pipeline.Layout;
    descriptor.compute.module = gRasterShaders[info.Shader].Object;
    descriptor.compute.entryPoint = {gRasterShaders[info.Shader].Entry, WGPU_STRLEN};
    WGPUCreateComputePipelineAsyncCallbackInfo callback = WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = ComputePipelineCreated;
    callback.userdata1 = Userdata(request);
    wgpuDeviceCreateComputePipelineAsync(gDevice, &descriptor, callback);
    return RasterPop(request);
}
// Thanks to W3C, "WebGPU", compute passes and resource usage scopes: closing
// the compute scope establishes a legal storage-write to vertex/indirect handoff.
// https://www.w3.org/TR/webgpu/#compute-passes
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    auto pass = wgpuCommandEncoderBeginComputePass(gEncoder, nullptr);
    if (pass == nullptr)
    {
        return RasterStatus::OutOfMemory;
    }
    wgpuComputePassEncoderSetPipeline(pass, gComputePipelines[packet.Pipeline].Object);
    wgpuComputePassEncoderSetBindGroup(pass,
                                       0,
                                       gRasterSets[packet.Set],
                                       packet.DynamicOffsetCount,
                                       packet.DynamicOffsets);
    wgpuComputePassEncoderDispatchWorkgroups(pass, packet.Groups[0], packet.Groups[1], packet.Groups[2]);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    return RasterStatus::Ready;
}
void ComputeBufferBarrier(usize, GraphAccessMode) noexcept {}
RasterStatus
RasterCreateBuffer(usize slot, const BufferDescription& info, std::span<const uint8> bytes, uint32 request) noexcept
{
    RasterScopes(request);
    WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.size = (bytes.size() + 3) & ~usize{3};
    descriptor.mappedAtCreation = true;
    descriptor.usage = info.Role == BufferRole::Uniform           ? WGPUBufferUsage_Uniform
                       : info.Role == BufferRole::Vertex          ? WGPUBufferUsage_Vertex
                       : info.Role == BufferRole::Storage         ? WGPUBufferUsage_Storage
                       : info.Role == BufferRole::StorageVertex   ? WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex
                       : info.Role == BufferRole::StorageIndirect ? WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect
                                                                  : WGPUBufferUsage_Index;
    descriptor.usage |= WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    auto object = wgpuDeviceCreateBuffer(gDevice, &descriptor);
    gRasterBuffers[slot] = object;
    if (object != nullptr && wgpuBufferGetMapState(object) == WGPUBufferMapState_Mapped)
    {
        void* mapped = wgpuBufferGetMappedRange(object, 0, descriptor.size);
        if (mapped != nullptr)
        {
            std::memset(mapped, 0, descriptor.size);
            std::memcpy(mapped, bytes.data(), bytes.size());
        }
        else
        {
            internal::RasterFail(request, RasterStatus::OutOfMemory);
        }
        wgpuBufferUnmap(object);
    }
    else
    {
        internal::RasterFail(request, RasterStatus::OutOfMemory);
    }
    return RasterPop(request);
}
RasterStatus
RasterCreateTexture(usize slot, const TextureDescription& info, const TextureUpload& upload, uint32 request) noexcept
{
    RasterScopes(request);
    WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.size = {info.Width, info.Height, info.DepthOrLayers};
    descriptor.dimension = info.Dimension == TextureDimension::D3 ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    descriptor.mipLevelCount = info.MipLevels;
    descriptor.sampleCount = info.SampleCount;
    descriptor.format = info.Format == RasterFormat::Bc1RgbaUnorm   ? WGPUTextureFormat_BC1RGBAUnorm
                        : info.Format == RasterFormat::Bc3RgbaUnorm ? WGPUTextureFormat_BC3RGBAUnorm
                        : info.Format == RasterFormat::Bc5RgUnorm   ? WGPUTextureFormat_BC5RGUnorm
                        : info.Format == RasterFormat::Rgba16Float  ? WGPUTextureFormat_RGBA16Float
                        : info.Format == RasterFormat::Rg8Unorm     ? WGPUTextureFormat_RG8Unorm
                        : info.Format == RasterFormat::R8Unorm      ? WGPUTextureFormat_R8Unorm
                        : info.Format == RasterFormat::Rgba8Unorm   ? WGPUTextureFormat_RGBA8Unorm
                                                                    : WGPUTextureFormat_RGBA8UnormSrgb;
    descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst |
                       (info.Format == RasterFormat::Rgba8Unorm ? WGPUTextureUsage_CopySrc : WGPUTextureUsage_None) |
                       (info.Attachment ? WGPUTextureUsage_RenderAttachment : WGPUTextureUsage_None);
    if (info.SampleCount > 1)
    {
        descriptor.usage = WGPUTextureUsage_RenderAttachment;
    }
    auto texture = wgpuDeviceCreateTexture(gDevice, &descriptor);
    gRasterTextures[slot] = texture;
    gRasterTextureDescriptions[slot] = info;
    if (texture != nullptr && info.Attachment == false && info.DeferredInitialization == false)
    {
        uint32 width = info.Width, height = info.Height;
        for (uint32 level = 0; level < info.MipLevels; ++level)
        {
            const auto mip = upload.Mips.empty() ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[level];
            WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            destination.texture = texture;
            destination.mipLevel = level;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = static_cast<uint32>(mip.RowPitch);
            layout.rowsPerImage = internal::TextureBlockRows(info.Format, height);
            const auto block = internal::TextureBlockWidth(info.Format);
            const WGPUExtent3D extent{internal::TextureBlockRows(info.Format, width) * block,
                                      internal::TextureBlockRows(info.Format, height) * block,
                                      internal::TextureSlices(info, level)};
            wgpuQueueWriteTexture(gQueue,
                                  &destination,
                                  upload.Bytes.data() + mip.Offset,
                                  mip.RowPitch * layout.rowsPerImage * extent.depthOrArrayLayers,
                                  &layout,
                                  &extent);
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        }
    }
    else if (texture != nullptr && info.Attachment)
    {
        WGPUTextureViewDescriptor attachment = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        attachment.dimension = WGPUTextureViewDimension_2D;
        attachment.mipLevelCount = 1;
        attachment.arrayLayerCount = 1;
        gRasterAttachmentViews[slot] = wgpuTextureCreateView(texture, &attachment);
        descriptor.format = info.Stencil ? WGPUTextureFormat_Depth24PlusStencil8 : WGPUTextureFormat_Depth32Float;
        descriptor.usage = WGPUTextureUsage_RenderAttachment |
                           (info.SampleCount == 1 ? WGPUTextureUsage_TextureBinding : WGPUTextureUsage_None);
        gRasterDepths[slot] = wgpuDeviceCreateTexture(gDevice, &descriptor);
        if (gRasterDepths[slot] != nullptr)
        {
            gRasterDepthViews[slot] = wgpuTextureCreateView(gRasterDepths[slot], &attachment);
        }
        if (gRasterAttachmentViews[slot] == nullptr || gRasterDepthViews[slot] == nullptr)
        {
            internal::RasterFail(request, RasterStatus::OutOfMemory);
        }
    }
    else if (texture == nullptr)
    {
        internal::RasterFail(request, RasterStatus::OutOfMemory);
    }
    return RasterPop(request);
}
RasterStatus RasterCreateView(usize slot, const internal::RasterViewSource& source, uint32 request) noexcept
{
    RasterScopes(request);
    WGPUTextureViewDescriptor view = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    view.dimension = source.View.Dimension == TextureDimension::D2Array ? WGPUTextureViewDimension_2DArray
                     : source.View.Dimension == TextureDimension::Cube  ? WGPUTextureViewDimension_Cube
                     : source.View.Dimension == TextureDimension::D3    ? WGPUTextureViewDimension_3D
                                                                        : WGPUTextureViewDimension_2D;
    view.baseMipLevel = source.View.BaseMip;
    view.mipLevelCount = source.View.MipCount;
    view.baseArrayLayer = source.View.BaseLayer;
    view.arrayLayerCount = source.View.LayerCount;
    view.aspect = source.View.Aspect == TextureAspect::Depth     ? WGPUTextureAspect_DepthOnly
                  : source.View.Aspect == TextureAspect::Stencil ? WGPUTextureAspect_StencilOnly
                                                                 : WGPUTextureAspect_All;
    gRasterViews[slot] = wgpuTextureCreateView(
        source.View.Aspect == TextureAspect::Color ? gRasterTextures[source.Texture] : gRasterDepths[source.Texture],
        &view);
    if (source.View.Attachment)
    {
        gRasterViewDepths[slot] = wgpuTextureCreateView(gRasterDepths[source.Texture], &view);
    }
    return RasterPop(request);
}
RasterStatus RasterCreateSampler(usize slot, const SamplerDescription& info, uint32 request) noexcept
{
    RasterScopes(request);
    WGPUSamplerDescriptor descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
    descriptor.minFilter = descriptor.magFilter =
        info.Filter == RasterFilter::Nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    descriptor.addressModeU = info.U == RasterAddress::Clamp ? WGPUAddressMode_ClampToEdge : WGPUAddressMode_Repeat;
    descriptor.addressModeV = info.V == RasterAddress::Clamp ? WGPUAddressMode_ClampToEdge : WGPUAddressMode_Repeat;
    descriptor.addressModeW = info.W == RasterAddress::Clamp ? WGPUAddressMode_ClampToEdge : WGPUAddressMode_Repeat;
    descriptor.mipmapFilter =
        info.MipFilter == RasterMipFilter::Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    descriptor.lodMinClamp = info.MinLod;
    descriptor.lodMaxClamp = info.MipFilter == RasterMipFilter::None ? 0 : info.MaxLod;
    gRasterSamplers[slot] = wgpuDeviceCreateSampler(gDevice, &descriptor);
    return RasterPop(request);
}
RasterStatus RasterCreateShader(usize slot,
                                const ShaderDescription& info,
                                const internal::RasterShaderInfo&,
                                uint32 request) noexcept
{
    RasterScopes(request);
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = {info.Wgsl.data(), info.Wgsl.size()};
    WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &source.chain;
    auto& shader = gRasterShaders[slot];
    shader.Object = wgpuDeviceCreateShaderModule(gDevice, &descriptor);
    std::memcpy(shader.Entry, info.WgslEntry.data(), info.WgslEntry.size());
    shader.Entry[info.WgslEntry.size()] = '\0';
    return RasterPop(request);
}
RasterStatus RasterCreateLayout(usize slot, const internal::RasterLayout& info, uint32 request) noexcept
{
    RasterScopes(request);
    WGPUBindGroupLayoutEntry entries[internal::RASTER_BINDINGS]{};
    for (usize i = 0; i < info.Count; ++i)
    {
        const auto& source = info.Entries[i];
        auto& entry = entries[i];
        entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entry.binding = source.Binding;
        entry.visibility = static_cast<WGPUShaderStage>(source.Visibility);
        if (source.Kind == RasterBindingKind::UniformBuffer || source.Kind == RasterBindingKind::StorageRead ||
            source.Kind == RasterBindingKind::StorageReadWrite)
        {
            entry.buffer.type = source.Kind == RasterBindingKind::UniformBuffer ? WGPUBufferBindingType_Uniform
                                : source.Kind == RasterBindingKind::StorageRead ? WGPUBufferBindingType_ReadOnlyStorage
                                                                                : WGPUBufferBindingType_Storage;
            entry.buffer.minBindingSize = source.MinSize;
            entry.buffer.hasDynamicOffset = source.HasDynamicOffset;
        }
        else if (internal::TextureBinding(source.Kind))
        {
            entry.texture.sampleType =
                source.Kind == RasterBindingKind::TextureDepth2D     ? WGPUTextureSampleType_UnfilterableFloat
                : source.Kind == RasterBindingKind::TextureStencil2D ? WGPUTextureSampleType_Uint
                                                                     : WGPUTextureSampleType_Float;
            entry.texture.viewDimension =
                source.Kind == RasterBindingKind::Texture2DArray ? WGPUTextureViewDimension_2DArray
                : source.Kind == RasterBindingKind::TextureCube  ? WGPUTextureViewDimension_Cube
                : source.Kind == RasterBindingKind::Texture3D    ? WGPUTextureViewDimension_3D
                                                                 : WGPUTextureViewDimension_2D;
        }
        else
        {
            entry.sampler.type = WGPUSamplerBindingType_Filtering;
        }
    }
    WGPUBindGroupLayoutDescriptor descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    descriptor.entryCount = info.Count;
    descriptor.entries = entries;
    gRasterLayouts[slot] = wgpuDeviceCreateBindGroupLayout(gDevice, &descriptor);
    return RasterPop(request);
}
RasterStatus RasterCreateSet(usize slot,
                             const internal::RasterLayout& layout,
                             const internal::RasterSet& info,
                             uint32 request) noexcept
{
    RasterScopes(request);
    WGPUBindGroupEntry entries[internal::RASTER_BINDINGS]{};
    for (usize i = 0; i < layout.Count; ++i)
    {
        const auto& source = layout.Entries[i];
        auto& entry = entries[i];
        entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = source.Binding;
        if (source.Kind == RasterBindingKind::UniformBuffer || source.Kind == RasterBindingKind::StorageRead ||
            source.Kind == RasterBindingKind::StorageReadWrite)
        {
            entry.buffer = gRasterBuffers[info.Slots[i]];
            entry.offset = info.Offsets[i];
            entry.size = info.Sizes[i];
        }
        else if (internal::TextureBinding(source.Kind))
        {
            entry.textureView = gRasterViews[info.Slots[i]];
        }
        else
        {
            entry.sampler = gRasterSamplers[info.Slots[i]];
        }
    }
    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.layout = gRasterLayouts[info.Layout];
    descriptor.entryCount = layout.Count;
    descriptor.entries = entries;
    gRasterSets[slot] = wgpuDeviceCreateBindGroup(gDevice, &descriptor);
    return RasterPop(request);
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32 request) noexcept
{
    RasterScopes(request);
    auto& pipeline = gRasterPipelines[slot];
    pipeline.Info = info;
    pipeline.Request = request;
    internal::RasterExpect(request, internal::RasterCallbacks::Three);
    WGPUPipelineLayoutDescriptor layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout.bindGroupLayoutCount = 1;
    layout.bindGroupLayouts = &gRasterLayouts[info.Layout];
    pipeline.Layout = wgpuDeviceCreatePipelineLayout(gDevice, &layout);
    constexpr usize VERTEX_STREAM_COUNT = sizeof(info.Streams) / sizeof(info.Streams[0]);
    constexpr usize VERTEX_ATTRIBUTE_COUNT = sizeof(info.Attributes) / sizeof(info.Attributes[0]);
    // Only initialized attributeCount prefixes reach WebGPU. Value-initializing
    // every unused slot would also initialize its format to an invalid enum zero.
    WGPUVertexAttribute attributes[VERTEX_STREAM_COUNT][VERTEX_ATTRIBUTE_COUNT];
    WGPUVertexBufferLayout buffers[VERTEX_STREAM_COUNT]{};
    for (usize stream = 0; stream < info.StreamCount; ++stream)
    {
        auto& buffer = buffers[stream];
        buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        buffer.arrayStride = info.Streams[stream].Stride;
        buffer.stepMode = info.Streams[stream].PerInstance ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
        buffer.attributes = attributes[stream];
        for (usize i = 0; i < info.AttributeCount; ++i)
        {
            const auto& source = info.Attributes[i];
            if (source.Stream != stream)
            {
                continue;
            }
            auto& attribute = attributes[stream][buffer.attributeCount++];
            attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
            attribute.shaderLocation = source.Location;
            attribute.offset = source.Offset;
            attribute.format = source.Format == RasterVertexFormat::Float2   ? WGPUVertexFormat_Float32x2
                               : source.Format == RasterVertexFormat::Float3 ? WGPUVertexFormat_Float32x3
                                                                             : WGPUVertexFormat_Float32x4;
        }
    }
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = info.Target == RasterTarget::Rgba16Float  ? WGPUTextureFormat_RGBA16Float
                    : info.Target == RasterTarget::Surface    ? gConfiguration.format
                    : info.Target == RasterTarget::Rgba8Unorm ? WGPUTextureFormat_RGBA8Unorm
                                                              : WGPUTextureFormat_RGBA8UnormSrgb;
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.srcFactor = blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    if (info.Blend)
    {
        target.blend = &blend;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = gRasterShaders[info.Fragment].Object;
    fragment.entryPoint = {gRasterShaders[info.Fragment].Entry, WGPU_STRLEN};
    WGPUColorTargetState targets[4]{};
    for (usize i = 0; i < info.ColorCount; ++i)
    {
        targets[i] = target;
        targets[i].format = info.ColorFormats[i] == RasterFormat::Rgba16Float  ? WGPUTextureFormat_RGBA16Float
                            : info.ColorFormats[i] == RasterFormat::Rgba8Unorm ? WGPUTextureFormat_RGBA8Unorm
                                                                               : WGPUTextureFormat_RGBA8UnormSrgb;
    }
    fragment.targetCount = info.ColorCount == 0 ? 1 : info.ColorCount;
    fragment.targets = info.ColorCount == 0 ? &target : targets;
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format =
        info.Stencil ? WGPUTextureFormat_Depth24PlusStencil8
                     : (info.ColorCount == 0 && info.Target == RasterTarget::Surface ? WGPUTextureFormat_Depth24Plus
                                                                                     : WGPUTextureFormat_Depth32Float);
    depth.depthWriteEnabled = info.Depth && info.DepthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    const WGPUCompareFunction comparisons[]{WGPUCompareFunction_Less,
                                            WGPUCompareFunction_LessEqual,
                                            WGPUCompareFunction_Greater,
                                            WGPUCompareFunction_GreaterEqual,
                                            WGPUCompareFunction_Always};
    depth.depthCompare = info.Depth ? comparisons[static_cast<usize>(info.DepthCompare)] : WGPUCompareFunction_Always;
    if (info.Stencil)
    {
        const WGPUCompareFunction compare[]{WGPUCompareFunction_Never,
                                            WGPUCompareFunction_Less,
                                            WGPUCompareFunction_Equal,
                                            WGPUCompareFunction_LessEqual,
                                            WGPUCompareFunction_Greater,
                                            WGPUCompareFunction_NotEqual,
                                            WGPUCompareFunction_GreaterEqual,
                                            WGPUCompareFunction_Always};
        const WGPUStencilOperation ops[]{WGPUStencilOperation_Keep,
                                         WGPUStencilOperation_Zero,
                                         WGPUStencilOperation_Replace,
                                         WGPUStencilOperation_IncrementClamp,
                                         WGPUStencilOperation_DecrementClamp,
                                         WGPUStencilOperation_Invert,
                                         WGPUStencilOperation_IncrementWrap,
                                         WGPUStencilOperation_DecrementWrap};
        const RasterStencilFace faces[]{info.StencilFront, info.StencilBack};
        WGPUStencilFaceState* output[]{&depth.stencilFront, &depth.stencilBack};
        for (usize i = 0; i < 2; ++i)
        {
            output[i]->compare = compare[static_cast<usize>(faces[i].Compare)];
            output[i]->failOp = ops[static_cast<usize>(faces[i].Fail)];
            output[i]->depthFailOp = ops[static_cast<usize>(faces[i].DepthFail)];
            output[i]->passOp = ops[static_cast<usize>(faces[i].Pass)];
        }
        depth.stencilReadMask = info.StencilReadMask;
        depth.stencilWriteMask = info.StencilWriteMask;
    }
    WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    descriptor.multisample.count = info.SampleCount;
    descriptor.layout = pipeline.Layout;
    descriptor.vertex.module = gRasterShaders[info.Vertex].Object;
    descriptor.vertex.entryPoint = {gRasterShaders[info.Vertex].Entry, WGPU_STRLEN};
    descriptor.vertex.bufferCount = info.StreamCount;
    descriptor.vertex.buffers = buffers;
    descriptor.fragment = &fragment;
    descriptor.depthStencil = &depth;
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    WGPUCreateRenderPipelineAsyncCallbackInfo callback = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = RasterPipelineCreated;
    callback.userdata1 = Userdata(request);
    wgpuDeviceCreateRenderPipelineAsync(gDevice, &descriptor, callback);
    return RasterPop(request);
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            if (gRasterBuffers[slot])
            {
                wgpuBufferRelease(gRasterBuffers[slot]);
            }
            gRasterBuffers[slot] = nullptr;
            break;
        case internal::RasterKind::Texture:
            if (gRasterTextures[slot])
            {
                wgpuTextureRelease(gRasterTextures[slot]);
            }
            if (gRasterAttachmentViews[slot])
            {
                wgpuTextureViewRelease(gRasterAttachmentViews[slot]);
            }
            if (gRasterDepthViews[slot])
            {
                wgpuTextureViewRelease(gRasterDepthViews[slot]);
            }
            if (gRasterDepths[slot])
            {
                wgpuTextureRelease(gRasterDepths[slot]);
            }
            gRasterAttachmentViews[slot] = gRasterDepthViews[slot] = nullptr;
            gRasterDepths[slot] = nullptr;
            gRasterTextures[slot] = nullptr;
            break;
        case internal::RasterKind::View:
            if (gRasterViews[slot])
            {
                wgpuTextureViewRelease(gRasterViews[slot]);
            }
            gRasterViews[slot] = nullptr;
            if (gRasterViewDepths[slot])
            {
                wgpuTextureViewRelease(gRasterViewDepths[slot]);
            }
            gRasterViewDepths[slot] = nullptr;
            break;
        case internal::RasterKind::Sampler:
            if (gRasterSamplers[slot])
            {
                wgpuSamplerRelease(gRasterSamplers[slot]);
            }
            gRasterSamplers[slot] = nullptr;
            break;
        case internal::RasterKind::Shader:
            if (gRasterShaders[slot].Object)
            {
                wgpuShaderModuleRelease(gRasterShaders[slot].Object);
            }
            gRasterShaders[slot] = {};
            break;
        case internal::RasterKind::Layout:
            if (gRasterLayouts[slot])
            {
                wgpuBindGroupLayoutRelease(gRasterLayouts[slot]);
            }
            gRasterLayouts[slot] = nullptr;
            break;
        case internal::RasterKind::Set:
            if (gRasterSets[slot])
            {
                wgpuBindGroupRelease(gRasterSets[slot]);
            }
            gRasterSets[slot] = nullptr;
            break;
        case internal::RasterKind::Pipeline:
            if (gRasterPipelines[slot].Object)
            {
                wgpuRenderPipelineRelease(gRasterPipelines[slot].Object);
            }
            if (gRasterPipelines[slot].Layout)
            {
                wgpuPipelineLayoutRelease(gRasterPipelines[slot].Layout);
            }
            gRasterPipelines[slot] = {};
            break;
        case internal::RasterKind::ComputePipeline:
            if (gComputePipelines[slot].Object)
            {
                wgpuComputePipelineRelease(gComputePipelines[slot].Object);
            }
            if (gComputePipelines[slot].Layout)
            {
                wgpuPipelineLayoutRelease(gComputePipelines[slot].Layout);
            }
            gComputePipelines[slot] = {};
            break;
        case internal::RasterKind::RayGeometry:
        case internal::RasterKind::RayScene:
        case internal::RasterKind::RayProgram:
        case internal::RasterKind::RayBatch:
        case internal::RasterKind::Count:
            break;
    }
}
internal::RasterArea gRasterArea{};
void RasterFrameExtent() noexcept
{
    gRasterArea = {};
}
RasterStatus RasterDraw(const internal::RasterPacket& packet) noexcept
{
    if (gRasterArea.Empty)
    {
        return RasterStatus::Ready;
    }
    const auto& pipeline = gRasterPipelines[packet.Pipeline];
    wgpuRenderPassEncoderSetPipeline(gPass, pipeline.Object);
    wgpuRenderPassEncoderSetStencilReference(gPass, pipeline.Info.StencilReference);
    wgpuRenderPassEncoderSetBindGroup(gPass,
                                      0,
                                      gRasterSets[packet.Set],
                                      packet.DynamicOffsetCount,
                                      packet.DynamicOffsets);
    for (usize i = 0; i < pipeline.Info.StreamCount; ++i)
    {
        wgpuRenderPassEncoderSetVertexBuffer(gPass,
                                             static_cast<uint32>(i),
                                             gRasterBuffers[packet.Vertices[i]],
                                             packet.Offsets[i],
                                             WGPU_WHOLE_SIZE);
    }
    wgpuRenderPassEncoderSetIndexBuffer(gPass,
                                        gRasterBuffers[packet.Indices],
                                        packet.Index32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16,
                                        packet.IndexOffset,
                                        WGPU_WHOLE_SIZE);
    if (packet.Indirect != internal::RASTER_INVALID)
    {
        wgpuRenderPassEncoderDrawIndexedIndirect(gPass, gRasterBuffers[packet.Indirect], packet.IndirectOffset);
    }
    else
    {
        wgpuRenderPassEncoderDrawIndexed(gPass, packet.IndexCount, packet.InstanceCount, 0, 0, 0);
    }
    return RasterStatus::Ready;
}
void RasterEndPass() noexcept
{
    if (gPass != nullptr)
    {
        wgpuRenderPassEncoderEnd(gPass);
        wgpuRenderPassEncoderRelease(gPass);
        gPass = nullptr;
    }
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo&) noexcept
{
    return RasterStatus::Ready;
}
void RasterTextureBarrier(usize, RasterTextureUse) noexcept {}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    const auto& state = info.Description;
    const bool surface = info.Texture == internal::RASTER_INVALID;
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = surface ? gView : gRasterAttachmentViews[info.Texture];
    // Discard has no WebGPU load operation; security initialization clears, but
    // the semantic ledger deliberately keeps those contents undefined.
    color.loadOp = state.ColorLoad == RasterLoad::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
    color.storeOp = state.ColorStore == RasterStore::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    color.clearValue = {static_cast<float64>(state.Clear[0]),
                        static_cast<float64>(state.Clear[1]),
                        static_cast<float64>(state.Clear[2]),
                        static_cast<float64>(state.Clear[3])};
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = surface ? gDepthView : gRasterDepthViews[info.Texture];
    depth.depthReadOnly = state.DepthReadOnly;
    // The browser bridge forwards this value even for read-only depth. Keep
    // the ignored value finite instead of forwarding the C API's NaN default.
    depth.depthClearValue = state.ClearDepth;
    if (state.DepthReadOnly == false)
    {
        depth.depthLoadOp = state.DepthLoad == RasterLoad::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
        depth.depthStoreOp = state.DepthStore == RasterStore::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    }
    WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    WGPURenderPassColorAttachment colors[4]{};
    for (usize i = 0; i < info.ColorCount; ++i)
    {
        const auto& c = info.Colors[i];
        auto& output = colors[i];
        output = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        output.view = gRasterViews[c.View];
        if (c.ResolveView != internal::RASTER_INVALID)
        {
            output.resolveTarget = gRasterViews[c.ResolveView];
        }
        output.loadOp = c.Load == RasterLoad::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
        output.storeOp = c.Store == RasterStore::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
        output.clearValue = {static_cast<float64>(c.Clear[0]),
                             static_cast<float64>(c.Clear[1]),
                             static_cast<float64>(c.Clear[2]),
                             static_cast<float64>(c.Clear[3])};
    }
    if (info.ColorCount != 0)
    {
        depth.view = gRasterViewDepths[info.Colors[0].View];
    }
    if (info.Stencil)
    {
        depth.stencilReadOnly = state.StencilReadOnly;
        depth.stencilClearValue = state.ClearStencil;
        if (state.StencilReadOnly == false)
        {
            depth.stencilLoadOp = state.StencilLoad == RasterLoad::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
            depth.stencilStoreOp = state.StencilStore == RasterStore::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
        }
    }
    pass.colorAttachmentCount = info.ColorCount == 0 ? 1 : info.ColorCount;
    pass.colorAttachments = info.ColorCount == 0 ? &color : colors;
    pass.depthStencilAttachment = &depth;
    const auto width = surface ? gTarget.Width : info.Width;
    const auto height = surface ? gTarget.Height : info.Height;
    if (internal::RasterResolveArea(state, width, height, gRasterArea) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    gPass = wgpuCommandEncoderBeginRenderPass(gEncoder, &pass);
    if (gPass == nullptr)
    {
        return RasterStatus::Failed;
    }
    const auto& v = gRasterArea.Viewport;
    const auto& c = gRasterArea.Scissor;
    wgpuRenderPassEncoderSetViewport(gPass,
                                     static_cast<float32>(v.X),
                                     static_cast<float32>(v.Y),
                                     static_cast<float32>(v.Width),
                                     static_cast<float32>(v.Height),
                                     0,
                                     1);
    wgpuRenderPassEncoderSetScissorRect(gPass, c.X, c.Y, c.Width, c.Height);
    return RasterStatus::Ready;
}
RasterStatus RasterReserveSubmission() noexcept
{
    if (gRasterReserved != nullptr)
    {
        return RasterStatus::Ready;
    }
    for (usize i = 0; i < internal::CurrentStorageLimits().Submissions; ++i)
    {
        auto& completion = gRasterCompletions[i];
        if (completion.Busy == false)
        {
            completion = {0, gSession, true, false};
            gRasterReserved = &completion;
            return RasterStatus::Ready;
        }
    }
    return RasterStatus::CapacityExceeded;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    if (gRasterReserved == nullptr)
    {
        return;
    }
    auto* completion = gRasterReserved;
    gRasterReserved = nullptr;
    completion->Ordinal = ordinal;
    completion->Submitted = true;
    WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = RasterDone;
    callback.userdata1 = completion;
    (void)wgpuQueueOnSubmittedWorkDone(gQueue, callback);
}
uint64 RasterCompleted() noexcept
{
    return gRasterCompleted;
}
void RasterReset() noexcept
{
    if (gRasterReserved != nullptr)
    {
        *gRasterReserved = {};
        gRasterReserved = nullptr;
    }
    // Submitted callback cells stay occupied until their old-session callback drains.
    // Reusing their addresses during restart would let late callbacks retire new work.
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
            return gRasterTextures.EnsureSlots(count) && gRasterTextureDescriptions.EnsureSlots(count) &&
                           gRasterDepths.EnsureSlots(count) && gRasterAttachmentViews.EnsureSlots(count) &&
                           gRasterDepthViews.EnsureSlots(count)
                       ? RasterStatus::Ready
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
            return gComputePipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        default:
            return RasterStatus::InvalidDescription;
    }
}
RasterStatus RasterPrepareStorage() noexcept
{
    return gRasterCompletions.EnsureSlots(internal::CurrentStorageLimits().Submissions) ? RasterStatus::Ready
                                                                                        : internal::StorageFailure();
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
