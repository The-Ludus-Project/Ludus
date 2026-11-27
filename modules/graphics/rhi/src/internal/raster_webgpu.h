#pragma once

#include "profile.h"
#include <ludus/foundation/containers/borrowed_view.hpp>
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
struct ComputeWebPipeline final
{
    WGPUComputePipeline Object = nullptr;
    WGPUPipelineLayout Layout = nullptr;
    uint32 Request = 0;
};
/// @brief Portable raster and compute objects, completions and draw area of one device owner (R14c).
struct RasterWebGpuState final
{
    internal::StorageTable<WGPUBuffer> Buffers;
    internal::StorageTable<WGPUTexture> Textures;
    internal::StorageTable<TextureDescription> TextureDescriptions;
    internal::StorageTable<WGPUTexture> Depths;
    internal::StorageTable<WGPUTextureView> AttachmentViews;
    internal::StorageTable<WGPUTextureView> ViewDepths;
    internal::StorageTable<WGPUTextureView> DepthViews;
    internal::StorageTable<WGPUTextureView> Views;
    internal::StorageTable<WGPUSampler> Samplers;
    internal::StorageTable<RasterWebShader> Shaders;
    internal::StorageTable<WGPUBindGroupLayout> Layouts;
    internal::StorageTable<WGPUBindGroup> Sets;
    internal::StorageTable<RasterWebPipeline> Pipelines;
    internal::StorageTable<ComputeWebPipeline> ComputePipelines;
    internal::StorageTable<RasterWebCompletion> Completions;
    RasterWebCompletion* ReservedOrNull = nullptr;
    uint64 Completed = 0;
    internal::RasterArea Area{};
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<RasterWebGpuState, MAX_DEVICE_OWNERS> gRasterWebGpuStates{};
/// RasterWebGpuState of the bound owner slot.
RasterWebGpuState& GetRasterWebGpu() noexcept
{
    return gRasterWebGpuStates[internal::GetBoundOwnerSlot()];
}
void RasterValidated(WGPUPopErrorScopeStatus status,
                     WGPUErrorType error,
                     WGPUStringView message,
                     void* request,
                     void* session) noexcept
{
    // Request numbers are per owner (R14c); the session token binds the owner that issued it.
    if (internal::Current(Token(session)) == false)
    {
        return;
    }
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
                           void* session) noexcept
{
    // Request numbers are per owner (R14c); the session token binds the owner that issued it.
    if (internal::Current(Token(session)) == false)
    {
        if (object != nullptr)
        {
            wgpuRenderPipelineRelease(object);
        }
        return;
    }
    const auto request = Token(userdata);
    bool adopted = false;
    for (RasterWebPipeline& pipeline : GetRasterWebGpu().Pipelines)
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
                            void* session) noexcept
{
    // Request numbers are per owner (R14c); the session token binds the owner that issued it.
    if (internal::Current(Token(session)) == false)
    {
        if (object != nullptr)
        {
            wgpuComputePipelineRelease(object);
        }
        return;
    }
    const auto request = Token(userdata);
    bool adopted = false;
    for (ComputeWebPipeline& pipeline : GetRasterWebGpu().ComputePipelines)
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
    wgpuDevicePushErrorScope(GetWebGpu().Device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(GetWebGpu().Device, WGPUErrorFilter_Validation);
}
RasterStatus RasterPop(uint32 request) noexcept
{
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = RasterValidated;
    callback.userdata1 = Userdata(request);
    callback.userdata2 = Userdata(GetWebGpu().Session);
    (void)wgpuDevicePopErrorScope(GetWebGpu().Device, callback);
    (void)wgpuDevicePopErrorScope(GetWebGpu().Device, callback);
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
        if (ordinal > GetRasterWebGpu().Completed)
        {
            GetRasterWebGpu().Completed = ordinal;
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
    if (GetWebGpu().Device == nullptr || wgpuDeviceGetLimits(GetWebGpu().Device, &limits) != WGPUStatus_Success)
    {
        return {};
    }
    return
    {
        .MaxDynamicUniformBuffers = limits.maxDynamicUniformBuffersPerPipelineLayout < RASTER_BINDING_COUNT
                                        ? limits.maxDynamicUniformBuffersPerPipelineLayout
                                        : static_cast<uint32>(RASTER_BINDING_COUNT),
        .MaxDynamicStorageBuffers = limits.maxDynamicStorageBuffersPerPipelineLayout < RASTER_BINDING_COUNT
                                        ? limits.maxDynamicStorageBuffersPerPipelineLayout
                                        : static_cast<uint32>(RASTER_BINDING_COUNT),
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
        .MaxUniformBuffersPerSet = static_cast<uint32>(RASTER_BINDING_COUNT),
        .MaxSamplersPerSet = static_cast<uint32>(RASTER_BINDING_COUNT),
    };
}
RasterCapabilities RasterLimits() noexcept
{
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (GetWebGpu().Device == nullptr || wgpuDeviceGetLimits(GetWebGpu().Device, &limits) != WGPUStatus_Success ||
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
    if (GetWebGpu().Device == nullptr ||
        (internal::CompressedTexture(format) &&
         wgpuDeviceHasFeature(GetWebGpu().Device, WGPUFeatureName_TextureCompressionBC) == false))
    {
        return {};
    }
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (wgpuDeviceGetLimits(GetWebGpu().Device, &limits) != WGPUStatus_Success)
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

// Defined with the compute entry points below; raster storage and indirect limits require compute.
ComputeCapabilities ComputeLimits() noexcept;
// Thanks to W3C GPU for the Web Working Group, "WebGPU", Limits and "Texture Format
// Capabilities": core devices admit read-only vertex storage, fragment storage writes and
// write-only rgba8unorm/rgba16float storage textures in fragment and compute stages.
// https://www.w3.org/TR/webgpu/#limits
RasterStorageCapabilities RasterStorageLimits() noexcept
{
    const ComputeCapabilities compute = ComputeLimits();
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (compute.MaxStorageRange == 0 || wgpuDeviceGetLimits(GetWebGpu().Device, &limits) != WGPUStatus_Success)
    {
        return {};
    }
    const uint32 textures = limits.maxStorageTexturesPerShaderStage < RASTER_BINDING_COUNT
                                ? limits.maxStorageTexturesPerShaderStage
                                : static_cast<uint32>(RASTER_BINDING_COUNT);
    return
    {
        .MaxVertexStorageBuffers = compute.MaxStorageBindings,
        .MaxFragmentStorageBuffers = compute.MaxStorageBindings,
        .HasFragmentStorageWrites = true,
        .MaxFragmentStorageTextures = textures,
        .MaxComputeStorageTextures = textures,
    };
}
// Thanks to W3C GPU for the Web Working Group, "WebGPU", drawIndirect, drawIndexedIndirect,
// dispatchWorkgroupsIndirect and the "indirect-first-instance" feature: argument offsets are
// four-byte aligned, firstIndex/baseVertex/firstVertex are core and a nonzero firstInstance
// without the feature turns the command into a no-op. https://www.w3.org/TR/webgpu/#indirect-first-instance
IndirectCapabilities IndirectLimits() noexcept
{
    if (ComputeLimits().MaxStorageRange == 0)
    {
        return {};
    }
    return
    {
        .ArgumentAlignment = internal::INDIRECT_ARGUMENT_ALIGNMENT_BYTES,
        .MaxCommandsPerDraw = internal::MAX_INDIRECT_COMMANDS_PER_DRAW,
        .HasNonIndexedDraws = true,
        .HasDrawOffsets = true,
        .HasFirstInstance = wgpuDeviceHasFeature(GetWebGpu().Device, WGPUFeatureName_IndirectFirstInstance) == true,
        .HasIndirectDispatch = true,
    };
}
ComputeCapabilities ComputeLimits() noexcept
{
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (GetWebGpu().Device == nullptr || wgpuDeviceGetLimits(GetWebGpu().Device, &limits) != WGPUStatus_Success ||
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
    ComputeWebPipeline& pipeline = GetRasterWebGpu().ComputePipelines[slot];
    pipeline.Request = request;
    WGPUPipelineLayoutDescriptor layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout.bindGroupLayoutCount = 1;
    layout.bindGroupLayouts = &GetRasterWebGpu().Layouts[info.Layout];
    pipeline.Layout = wgpuDeviceCreatePipelineLayout(GetWebGpu().Device, &layout);
    WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    descriptor.layout = pipeline.Layout;
    descriptor.compute.module = GetRasterWebGpu().Shaders[info.Shader].Object;
    descriptor.compute.entryPoint = {GetRasterWebGpu().Shaders[info.Shader].Entry, WGPU_STRLEN};
    WGPUCreateComputePipelineAsyncCallbackInfo callback = WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = ComputePipelineCreated;
    callback.userdata1 = Userdata(request);
    callback.userdata2 = Userdata(GetWebGpu().Session);
    wgpuDeviceCreateComputePipelineAsync(GetWebGpu().Device, &descriptor, callback);
    return RasterPop(request);
}
// Thanks to W3C, "WebGPU", compute passes and resource usage scopes: closing
// the compute scope establishes a legal storage-write to vertex/indirect handoff.
// https://www.w3.org/TR/webgpu/#compute-passes
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(GetWebGpu().Encoder, nullptr);
    if (pass == nullptr)
    {
        return RasterStatus::OutOfMemory;
    }
    wgpuComputePassEncoderSetPipeline(pass, GetRasterWebGpu().ComputePipelines[packet.Pipeline].Object);
    wgpuComputePassEncoderSetBindGroup(pass,
                                       0,
                                       GetRasterWebGpu().Sets[packet.Set],
                                       packet.DynamicOffsetCount,
                                       packet.DynamicOffsets);
    if (packet.Indirect != internal::RASTER_INVALID)
    {
        wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass,
                                                         GetRasterWebGpu().Buffers[packet.Indirect],
                                                         packet.IndirectOffset);
    }
    else
    {
        wgpuComputePassEncoderDispatchWorkgroups(pass, packet.Groups[0], packet.Groups[1], packet.Groups[2]);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    return RasterStatus::Ready;
}
void ComputeBufferBarrier(usize, GraphAccessMode) noexcept {}
RasterStatus RasterCreateBuffer(usize slot,
                                const BufferDescription& info,
                                ludus::foundation::BorrowedView<const uint8> bytes,
                                uint32 request) noexcept
{
    RasterScopes(request);
    WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.size =
        (bytes.GetSize() + internal::BUFFER_ALIGNMENT_BYTES - 1) & ~(internal::BUFFER_ALIGNMENT_BYTES - 1);
    descriptor.mappedAtCreation = true;
    descriptor.usage = info.Role == BufferRole::Uniform           ? WGPUBufferUsage_Uniform
                       : info.Role == BufferRole::Vertex          ? WGPUBufferUsage_Vertex
                       : info.Role == BufferRole::Storage         ? WGPUBufferUsage_Storage
                       : info.Role == BufferRole::StorageVertex   ? WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex
                       : info.Role == BufferRole::StorageIndirect ? WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect
                                                                  : WGPUBufferUsage_Index;
    descriptor.usage |= WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    WGPUBuffer object = wgpuDeviceCreateBuffer(GetWebGpu().Device, &descriptor);
    GetRasterWebGpu().Buffers[slot] = object;
    if (object != nullptr && wgpuBufferGetMapState(object) == WGPUBufferMapState_Mapped)
    {
        void* mapped = wgpuBufferGetMappedRange(object, 0, descriptor.size);
        if (mapped != nullptr)
        {
            std::memset(mapped, 0, descriptor.size);
            std::memcpy(mapped, bytes.GetDataOrNull(), bytes.GetSize());
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
                       (info.Attachment ? WGPUTextureUsage_RenderAttachment : WGPUTextureUsage_None) |
                       (info.IsStorage ? WGPUTextureUsage_StorageBinding : WGPUTextureUsage_None);
    if (info.SampleCount > 1)
    {
        descriptor.usage = WGPUTextureUsage_RenderAttachment;
    }
    WGPUTexture texture = wgpuDeviceCreateTexture(GetWebGpu().Device, &descriptor);
    GetRasterWebGpu().Textures[slot] = texture;
    GetRasterWebGpu().TextureDescriptions[slot] = info;
    if (texture != nullptr && info.Attachment == false && info.DeferredInitialization == false)
    {
        uint32 width = info.Width, height = info.Height;
        for (uint32 level = 0; level < info.MipLevels; ++level)
        {
            const TextureMipUpload mip =
                upload.Mips.IsEmpty() == true ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[level];
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
            wgpuQueueWriteTexture(GetWebGpu().Queue,
                                  &destination,
                                  upload.Bytes.GetDataOrNull() + mip.Offset,
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
        GetRasterWebGpu().AttachmentViews[slot] = wgpuTextureCreateView(texture, &attachment);
        descriptor.format = info.Stencil ? WGPUTextureFormat_Depth24PlusStencil8 : WGPUTextureFormat_Depth32Float;
        descriptor.usage = WGPUTextureUsage_RenderAttachment |
                           (info.SampleCount == 1 ? WGPUTextureUsage_TextureBinding : WGPUTextureUsage_None);
        GetRasterWebGpu().Depths[slot] = wgpuDeviceCreateTexture(GetWebGpu().Device, &descriptor);
        if (GetRasterWebGpu().Depths[slot] != nullptr)
        {
            GetRasterWebGpu().DepthViews[slot] = wgpuTextureCreateView(GetRasterWebGpu().Depths[slot], &attachment);
        }
        if (GetRasterWebGpu().AttachmentViews[slot] == nullptr || GetRasterWebGpu().DepthViews[slot] == nullptr)
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
    GetRasterWebGpu().Views[slot] =
        wgpuTextureCreateView(source.View.Aspect == TextureAspect::Color ? GetRasterWebGpu().Textures[source.Texture]
                                                                         : GetRasterWebGpu().Depths[source.Texture],
                              &view);
    if (source.View.Attachment)
    {
        GetRasterWebGpu().ViewDepths[slot] = wgpuTextureCreateView(GetRasterWebGpu().Depths[source.Texture], &view);
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
    GetRasterWebGpu().Samplers[slot] = wgpuDeviceCreateSampler(GetWebGpu().Device, &descriptor);
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
    RasterWebShader& shader = GetRasterWebGpu().Shaders[slot];
    shader.Object = wgpuDeviceCreateShaderModule(GetWebGpu().Device, &descriptor);
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
        else if (internal::StorageTextureBinding(source.Kind))
        {
            // Core WebGPU write-only storage formats; read_write access needs a language feature.
            entry.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
            entry.storageTexture.format = source.StorageFormat == RasterFormat::Rgba16Float
                                              ? WGPUTextureFormat_RGBA16Float
                                              : WGPUTextureFormat_RGBA8Unorm;
            entry.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
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
    GetRasterWebGpu().Layouts[slot] = wgpuDeviceCreateBindGroupLayout(GetWebGpu().Device, &descriptor);
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
            entry.buffer = GetRasterWebGpu().Buffers[info.Slots[i]];
            entry.offset = info.Offsets[i];
            entry.size = info.Sizes[i];
        }
        else if (internal::ViewBinding(source.Kind))
        {
            entry.textureView = GetRasterWebGpu().Views[info.Slots[i]];
        }
        else
        {
            entry.sampler = GetRasterWebGpu().Samplers[info.Slots[i]];
        }
    }
    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.layout = GetRasterWebGpu().Layouts[info.Layout];
    descriptor.entryCount = layout.Count;
    descriptor.entries = entries;
    GetRasterWebGpu().Sets[slot] = wgpuDeviceCreateBindGroup(GetWebGpu().Device, &descriptor);
    return RasterPop(request);
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32 request) noexcept
{
    RasterScopes(request);
    RasterWebPipeline& pipeline = GetRasterWebGpu().Pipelines[slot];
    pipeline.Info = info;
    pipeline.Request = request;
    internal::RasterExpect(request, internal::RasterCallbacks::Three);
    WGPUPipelineLayoutDescriptor layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout.bindGroupLayoutCount = 1;
    layout.bindGroupLayouts = &GetRasterWebGpu().Layouts[info.Layout];
    pipeline.Layout = wgpuDeviceCreatePipelineLayout(GetWebGpu().Device, &layout);
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
    // Thanks to the GPU for the Web Community Group, WebGPU CTS, "pipeline_output_targets":
    // a color attachment without a fragment output requires a zero write mask.
    // https://github.com/gpuweb/cts/blob/main/src/webgpu/api/validation/render_pipeline/fragment_state.spec.ts
    target.writeMask = info.ShouldWriteColor == true ? WGPUColorWriteMask_All : WGPUColorWriteMask_None;
    target.format = info.Target == RasterTarget::Rgba16Float  ? WGPUTextureFormat_RGBA16Float
                    : info.Target == RasterTarget::Surface    ? GetSurfaceFormat()
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
    fragment.module = GetRasterWebGpu().Shaders[info.Fragment].Object;
    fragment.entryPoint = {GetRasterWebGpu().Shaders[info.Fragment].Entry, WGPU_STRLEN};
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
    descriptor.multisample.alphaToCoverageEnabled = info.ShouldUseAlphaToCoverage;
    descriptor.layout = pipeline.Layout;
    descriptor.vertex.module = GetRasterWebGpu().Shaders[info.Vertex].Object;
    descriptor.vertex.entryPoint = {GetRasterWebGpu().Shaders[info.Vertex].Entry, WGPU_STRLEN};
    descriptor.vertex.bufferCount = info.StreamCount;
    descriptor.vertex.buffers = buffers;
    descriptor.fragment = &fragment;
    descriptor.depthStencil = &depth;
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    WGPUCreateRenderPipelineAsyncCallbackInfo callback = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = RasterPipelineCreated;
    callback.userdata1 = Userdata(request);
    callback.userdata2 = Userdata(GetWebGpu().Session);
    wgpuDeviceCreateRenderPipelineAsync(GetWebGpu().Device, &descriptor, callback);
    return RasterPop(request);
}
// Thanks to the W3C GPU for the Web Working Group, WebGPU, "GPUObjectBase label":
// https://www.w3.org/TR/webgpu/#dom-gpuobjectbase-label
void RasterSetDebugName(internal::RasterKind kind, usize slot, const internal::RasterDebugNames& names) noexcept
{
    const WGPUStringView label{names.Name, std::strlen(names.Name)};
    const WGPUStringView depthLabel{names.DepthName, std::strlen(names.DepthName)};
    if (kind == internal::RasterKind::Buffer && GetRasterWebGpu().Buffers[slot] != nullptr)
    {
        wgpuBufferSetLabel(GetRasterWebGpu().Buffers[slot], label);
    }
    else if (kind == internal::RasterKind::Texture)
    {
        if (GetRasterWebGpu().Textures[slot] != nullptr)
        {
            wgpuTextureSetLabel(GetRasterWebGpu().Textures[slot], label);
        }
        if (GetRasterWebGpu().Depths[slot] != nullptr)
        {
            wgpuTextureSetLabel(GetRasterWebGpu().Depths[slot], depthLabel);
        }
    }
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            if (GetRasterWebGpu().Buffers[slot])
            {
                wgpuBufferRelease(GetRasterWebGpu().Buffers[slot]);
            }
            GetRasterWebGpu().Buffers[slot] = nullptr;
            break;
        case internal::RasterKind::Texture:
            if (GetRasterWebGpu().Textures[slot])
            {
                wgpuTextureRelease(GetRasterWebGpu().Textures[slot]);
            }
            if (GetRasterWebGpu().AttachmentViews[slot])
            {
                wgpuTextureViewRelease(GetRasterWebGpu().AttachmentViews[slot]);
            }
            if (GetRasterWebGpu().DepthViews[slot])
            {
                wgpuTextureViewRelease(GetRasterWebGpu().DepthViews[slot]);
            }
            if (GetRasterWebGpu().Depths[slot])
            {
                wgpuTextureRelease(GetRasterWebGpu().Depths[slot]);
            }
            GetRasterWebGpu().AttachmentViews[slot] = GetRasterWebGpu().DepthViews[slot] = nullptr;
            GetRasterWebGpu().Depths[slot] = nullptr;
            GetRasterWebGpu().Textures[slot] = nullptr;
            break;
        case internal::RasterKind::View:
            if (GetRasterWebGpu().Views[slot])
            {
                wgpuTextureViewRelease(GetRasterWebGpu().Views[slot]);
            }
            GetRasterWebGpu().Views[slot] = nullptr;
            if (GetRasterWebGpu().ViewDepths[slot])
            {
                wgpuTextureViewRelease(GetRasterWebGpu().ViewDepths[slot]);
            }
            GetRasterWebGpu().ViewDepths[slot] = nullptr;
            break;
        case internal::RasterKind::Sampler:
            if (GetRasterWebGpu().Samplers[slot])
            {
                wgpuSamplerRelease(GetRasterWebGpu().Samplers[slot]);
            }
            GetRasterWebGpu().Samplers[slot] = nullptr;
            break;
        case internal::RasterKind::Shader:
            if (GetRasterWebGpu().Shaders[slot].Object)
            {
                wgpuShaderModuleRelease(GetRasterWebGpu().Shaders[slot].Object);
            }
            GetRasterWebGpu().Shaders[slot] = {};
            break;
        case internal::RasterKind::Layout:
            if (GetRasterWebGpu().Layouts[slot])
            {
                wgpuBindGroupLayoutRelease(GetRasterWebGpu().Layouts[slot]);
            }
            GetRasterWebGpu().Layouts[slot] = nullptr;
            break;
        case internal::RasterKind::Set:
            if (GetRasterWebGpu().Sets[slot])
            {
                wgpuBindGroupRelease(GetRasterWebGpu().Sets[slot]);
            }
            GetRasterWebGpu().Sets[slot] = nullptr;
            break;
        case internal::RasterKind::Pipeline:
            if (GetRasterWebGpu().Pipelines[slot].Object)
            {
                wgpuRenderPipelineRelease(GetRasterWebGpu().Pipelines[slot].Object);
            }
            if (GetRasterWebGpu().Pipelines[slot].Layout)
            {
                wgpuPipelineLayoutRelease(GetRasterWebGpu().Pipelines[slot].Layout);
            }
            GetRasterWebGpu().Pipelines[slot] = {};
            break;
        case internal::RasterKind::ComputePipeline:
            if (GetRasterWebGpu().ComputePipelines[slot].Object)
            {
                wgpuComputePipelineRelease(GetRasterWebGpu().ComputePipelines[slot].Object);
            }
            if (GetRasterWebGpu().ComputePipelines[slot].Layout)
            {
                wgpuPipelineLayoutRelease(GetRasterWebGpu().ComputePipelines[slot].Layout);
            }
            GetRasterWebGpu().ComputePipelines[slot] = {};
            break;
        case internal::RasterKind::RayGeometry:
        case internal::RasterKind::RayScene:
        case internal::RasterKind::RayProgram:
        case internal::RasterKind::RayBatch:
        case internal::RasterKind::SpatialUpscaler:
        case internal::RasterKind::Count:
            break;
    }
}
void RasterFrameExtent() noexcept
{
    GetRasterWebGpu().Area = {};
}
RasterStatus RasterDraw(const internal::RasterPacket& packet) noexcept
{
    if (GetRasterWebGpu().Area.Empty)
    {
        return RasterStatus::Ready;
    }
    const RasterWebPipeline& pipeline = GetRasterWebGpu().Pipelines[packet.Pipeline];
    wgpuRenderPassEncoderSetPipeline(GetWebGpu().Pass, pipeline.Object);
    wgpuRenderPassEncoderSetStencilReference(GetWebGpu().Pass, pipeline.Info.StencilReference);
    wgpuRenderPassEncoderSetBindGroup(GetWebGpu().Pass,
                                      0,
                                      GetRasterWebGpu().Sets[packet.Set],
                                      packet.DynamicOffsetCount,
                                      packet.DynamicOffsets);
    for (usize i = 0; i < pipeline.Info.StreamCount; ++i)
    {
        wgpuRenderPassEncoderSetVertexBuffer(GetWebGpu().Pass,
                                             static_cast<uint32>(i),
                                             GetRasterWebGpu().Buffers[packet.Vertices[i]],
                                             packet.Offsets[i],
                                             WGPU_WHOLE_SIZE);
    }
    const bool isIndexed = packet.Indices != internal::RASTER_INVALID;
    if (isIndexed == true)
    {
        wgpuRenderPassEncoderSetIndexBuffer(GetWebGpu().Pass,
                                            GetRasterWebGpu().Buffers[packet.Indices],
                                            packet.Index32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16,
                                            packet.IndexOffset,
                                            WGPU_WHOLE_SIZE);
    }
    if (packet.Indirect != internal::RASTER_INVALID)
    {
        // R12a: ordered single commands; native multi-draw is outside this profile.
        for (uint32 command = 0; command < packet.IndirectCount; ++command)
        {
            const uint64 offset = packet.IndirectOffset + usize{command} * packet.IndirectStride;
            if (isIndexed == true)
            {
                wgpuRenderPassEncoderDrawIndexedIndirect(GetWebGpu().Pass,
                                                         GetRasterWebGpu().Buffers[packet.Indirect],
                                                         offset);
            }
            else
            {
                wgpuRenderPassEncoderDrawIndirect(GetWebGpu().Pass, GetRasterWebGpu().Buffers[packet.Indirect], offset);
            }
        }
    }
    else
    {
        wgpuRenderPassEncoderDrawIndexed(GetWebGpu().Pass, packet.IndexCount, packet.InstanceCount, 0, 0, 0);
    }
    return RasterStatus::Ready;
}
void RasterEndPass() noexcept
{
    if (GetWebGpu().Pass != nullptr)
    {
        wgpuRenderPassEncoderEnd(GetWebGpu().Pass);
        wgpuRenderPassEncoderRelease(GetWebGpu().Pass);
        GetWebGpu().Pass = nullptr;
    }
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo& info) noexcept
{
    if (GetActiveSurface().Target.ShouldPresentToSurface == false && info.Texture == internal::RASTER_INVALID &&
        info.ColorCount == 0)
    {
        return RasterStatus::InvalidState;
    }
    return RasterStatus::Ready;
}
void RasterTextureBarrier(usize, RasterTextureUse) noexcept {}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    const RasterStatus admitted = RasterPreparePass(info);
    if (admitted != RasterStatus::Ready)
    {
        return admitted;
    }
    const auto& state = info.Description;
    const bool surface = info.Texture == internal::RASTER_INVALID;
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = surface ? GetWebGpu().View : GetRasterWebGpu().AttachmentViews[info.Texture];
    // Discard has no WebGPU load operation; security initialization clears, but
    // the semantic ledger deliberately keeps those contents undefined.
    color.loadOp = state.ColorLoad == RasterLoad::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
    color.storeOp = state.ColorStore == RasterStore::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    color.clearValue = {static_cast<float64>(state.Clear[0]),
                        static_cast<float64>(state.Clear[1]),
                        static_cast<float64>(state.Clear[2]),
                        static_cast<float64>(state.Clear[3])};
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = surface ? GetWebGpu().DepthView : GetRasterWebGpu().DepthViews[info.Texture];
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
        output.view = GetRasterWebGpu().Views[c.View];
        if (c.ResolveView != internal::RASTER_INVALID)
        {
            output.resolveTarget = GetRasterWebGpu().Views[c.ResolveView];
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
        depth.view = GetRasterWebGpu().ViewDepths[info.Colors[0].View];
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
    const uint32 width = surface == true ? GetActiveSurface().Target.Width : info.Width;
    const uint32 height = surface == true ? GetActiveSurface().Target.Height : info.Height;
    if (internal::RasterResolveArea(state, width, height, GetRasterWebGpu().Area) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    GetWebGpu().Pass = wgpuCommandEncoderBeginRenderPass(GetWebGpu().Encoder, &pass);
    if (GetWebGpu().Pass == nullptr)
    {
        return RasterStatus::Failed;
    }
    const RasterRectangle& v = GetRasterWebGpu().Area.Viewport;
    const RasterRectangle& c = GetRasterWebGpu().Area.Scissor;
    wgpuRenderPassEncoderSetViewport(GetWebGpu().Pass,
                                     static_cast<float32>(v.X),
                                     static_cast<float32>(v.Y),
                                     static_cast<float32>(v.Width),
                                     static_cast<float32>(v.Height),
                                     0,
                                     1);
    wgpuRenderPassEncoderSetScissorRect(GetWebGpu().Pass, c.X, c.Y, c.Width, c.Height);
    return RasterStatus::Ready;
}
RasterStatus RasterReserveSubmission() noexcept
{
    if (GetRasterWebGpu().ReservedOrNull != nullptr)
    {
        return RasterStatus::Ready;
    }
    for (usize i = 0; i < internal::CurrentStorageLimits().Submissions; ++i)
    {
        RasterWebCompletion& completion = GetRasterWebGpu().Completions[i];
        if (completion.Busy == false)
        {
            completion = {0, GetWebGpu().Session, true, false};
            GetRasterWebGpu().ReservedOrNull = &completion;
            return RasterStatus::Ready;
        }
    }
    return RasterStatus::CapacityExceeded;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    if (GetRasterWebGpu().ReservedOrNull == nullptr)
    {
        return;
    }
    RasterWebCompletion* completion = GetRasterWebGpu().ReservedOrNull;
    GetRasterWebGpu().ReservedOrNull = nullptr;
    completion->Ordinal = ordinal;
    completion->Submitted = true;
    WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = RasterDone;
    callback.userdata1 = completion;
    (void)wgpuQueueOnSubmittedWorkDone(GetWebGpu().Queue, callback);
}
uint64 RasterCompleted() noexcept
{
    return GetRasterWebGpu().Completed;
}
void RasterReset() noexcept
{
    if (GetRasterWebGpu().ReservedOrNull != nullptr)
    {
        *GetRasterWebGpu().ReservedOrNull = {};
        GetRasterWebGpu().ReservedOrNull = nullptr;
    }
    // Submitted callback cells stay occupied until their old-session callback drains.
    // Reusing their addresses during restart would let late callbacks retire new work.
    GetRasterWebGpu().Completed = 0;
}
void RasterShutdown() noexcept {}

RasterStatus RasterGrow(internal::RasterKind kind, usize count) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            return GetRasterWebGpu().Buffers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Texture:
            return GetRasterWebGpu().Textures.EnsureSlots(count) &&
                           GetRasterWebGpu().TextureDescriptions.EnsureSlots(count) &&
                           GetRasterWebGpu().Depths.EnsureSlots(count) &&
                           GetRasterWebGpu().AttachmentViews.EnsureSlots(count) &&
                           GetRasterWebGpu().DepthViews.EnsureSlots(count)
                       ? RasterStatus::Ready
                       : internal::StorageFailure();
        case internal::RasterKind::View:
            return GetRasterWebGpu().Views.EnsureSlots(count) && GetRasterWebGpu().ViewDepths.EnsureSlots(count)
                       ? RasterStatus::Ready
                       : internal::StorageFailure();
        case internal::RasterKind::Sampler:
            return GetRasterWebGpu().Samplers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Shader:
            return GetRasterWebGpu().Shaders.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Layout:
            return GetRasterWebGpu().Layouts.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Set:
            return GetRasterWebGpu().Sets.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Pipeline:
            return GetRasterWebGpu().Pipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::ComputePipeline:
            return GetRasterWebGpu().ComputePipelines.EnsureSlots(count) ? RasterStatus::Ready
                                                                         : internal::StorageFailure();
        default:
            return RasterStatus::InvalidDescription;
    }
}
RasterStatus RasterPrepareStorage() noexcept
{
    return GetRasterWebGpu().Completions.EnsureSlots(internal::CurrentStorageLimits().Submissions)
               ? RasterStatus::Ready
               : internal::StorageFailure();
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
