#include "internal/d3d12_diagnostics.h"
#include "internal/d3d12_raster.h"
#include "internal/d3d12_transfer.h"
#include "internal/dxil_container.h"
#include "internal/raster.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <cstring>
#include <initializer_list>

// Thanks to Microsoft, "Creating Descriptor Heaps" and "Uploading texture data
// through buffers", Direct3D 12 programming guide, for immutable shader-visible
// heaps and GetCopyableFootprints/CopyTextureRegion row and slice placement:
// https://learn.microsoft.com/windows/win32/direct3d12/creating-descriptor-heaps
// https://learn.microsoft.com/windows/win32/direct3d12/upload-and-readback-of-texture-data
// The facade owns logical identities/dependencies and byte/descriptor admission.
// This owner holds native backing until its exact queue fence permits retirement.
namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace foundation;
using namespace internal;
using namespace d3d12;

constexpr usize MAX_BUFFER_BYTES = usize{128} * 1024 * 1024;
constexpr usize MAX_UNIFORM_BYTES = usize{D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT} * 16;
constexpr uint32 MAX_MIP_LEVELS = D3D12_REQ_MIP_LEVELS;
constexpr uint32 MAX_VERTEX_ATTRIBUTES = RASTER_BINDING_COUNT;
constexpr uint32 MAX_SET_DESCRIPTORS = RASTER_BINDING_COUNT;
constexpr usize CBV_ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr usize MAX_TEXTURE_SUBRESOURCES = usize{D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION} * MAX_MIP_LEVELS;
constexpr usize DESCRIPTOR_TABLE_COUNT = RASTER_BINDING_COUNT;

struct Buffer final
{
    ComOwner<ID3D12Resource> Resource;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
    bool HasStorage = false;
};
struct Texture final
{
    ComOwner<ID3D12Resource> Resource;
    ComOwner<ID3D12Resource> Upload;
    ComOwner<ID3D12CommandAllocator> Allocator;
    ComOwner<ID3D12GraphicsCommandList> Commands;
    ComOwner<ID3D12Fence> CompletionFence;
    TextureDescription Description;
    ComOwner<ID3D12Resource> Depth;
    ComOwner<ID3D12DescriptorHeap> Targets;
    ComOwner<ID3D12DescriptorHeap> Depths;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
    /// State of the private depth backing; it follows the color backing between attachment and sampled use.
    D3D12_RESOURCE_STATES DepthState = D3D12_RESOURCE_STATE_COMMON;
    uint64 Fence = 0;
    uint64 StagingBytes = 0;
    uint32 Request = 0;
    bool IsRetired = false;
};
struct View final
{
    usize TextureSlot = RASTER_INVALID;
    D3D12_SHADER_RESOURCE_VIEW_DESC Description{};
    /// Reads the texture's private depth backing instead of its color backing.
    bool IsDepth = false;
};
struct Sampler final
{
    D3D12_SAMPLER_DESC Description
    {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
    };
};
struct Shader final
{
    StorageArray<uint8> Bytes;
};
struct Layout final
{
    ComOwner<ID3D12RootSignature> Signature;
};
struct Set final
{
    ComOwner<ID3D12DescriptorHeap> Resources;
    ComOwner<ID3D12DescriptorHeap> Samplers;
    uint32 ResourceCount = 0;
    uint32 SamplerCount = 0;
    RasterLayout Description;
    /// Buffer slot per storage entry; raster draws transition these for shader reads.
    StaticArray<usize, RASTER_BINDINGS> BufferSlots{};
};
struct Submission final
{
    uint64 Ordinal = 0;
    uint64 Fence = 0;
};
StorageTable<Buffer> gBuffers;
StorageTable<Texture> gTextures;
StorageTable<View> gViews;
StorageTable<Sampler> gSamplers;
StorageTable<Shader> gRasterShaders;
StorageTable<Layout> gLayouts;
StorageTable<Set> gSets;
StorageArray<Submission> gSubmissions;
uint64 gCompletedOrdinal = 0;
usize gReservedSubmission = RASTER_INVALID;

[[nodiscard]] RasterStatus GetFailure(HRESULT result) noexcept
{
    return result == E_OUTOFMEMORY                               ? RasterStatus::OutOfMemory
           : GetDeviceOrNull()->GetDeviceRemovedReason() != S_OK ? RasterStatus::DeviceLost
                                                                 : RasterStatus::Failed;
}
[[nodiscard]] DXGI_FORMAT GetFormat(RasterFormat format) noexcept
{
    switch (format)
    {
        case RasterFormat::Rgba8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RasterFormat::Rgba8Srgb:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case RasterFormat::R8Unorm:
            return DXGI_FORMAT_R8_UNORM;
        case RasterFormat::Rg8Unorm:
            return DXGI_FORMAT_R8G8_UNORM;
        case RasterFormat::Rgba16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RasterFormat::Bc1RgbaUnorm:
            return DXGI_FORMAT_BC1_UNORM;
        case RasterFormat::Bc3RgbaUnorm:
            return DXGI_FORMAT_BC3_UNORM;
        case RasterFormat::Bc5RgUnorm:
            return DXGI_FORMAT_BC5_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}
// A sampled depth aspect needs a typeless backing, so that the depth-stencil view and the
// shader resource view read the same bits as D32_FLOAT and R32_FLOAT (or their stencil
// variants). Thanks to Microsoft, "Configuring Depth-Stencil Functionality", section
// "Reading the Depth-Stencil Buffer as a Texture", Direct3D 11 programming guide:
// https://learn.microsoft.com/windows/win32/direct3d11/d3d10-graphics-programming-guide-depth-stencil
struct DepthFormats final
{
    DXGI_FORMAT Backing = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT Attachment = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT Sampled = DXGI_FORMAT_UNKNOWN;
};
[[nodiscard]] DepthFormats GetDepthFormats(bool hasStencil) noexcept
{
    return hasStencil == true ? DepthFormats{DXGI_FORMAT_R32G8X24_TYPELESS,
                                             DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                                             DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS}
                              : DepthFormats{DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT};
}
[[nodiscard]] bool HasDepthFormat(DXGI_FORMAT format) noexcept
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    return SUCCEEDED(GetDeviceOrNull()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ==
               true &&
           (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0;
}
[[nodiscard]] D3D12_RESOURCE_DESC MakeBufferDescription(usize size) noexcept
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}
[[nodiscard]] RasterStatus CreateResource(const D3D12_RESOURCE_DESC& description,
                                          D3D12_HEAP_TYPE heapType,
                                          D3D12_RESOURCE_STATES state,
                                          ComOwner<ID3D12Resource>& outResource) noexcept
{
    D3D12_HEAP_PROPERTIES heap{ .Type = heapType };
    ID3D12Resource* resourceOrNull = nullptr;
    const HRESULT result = GetDeviceOrNull()->CreateCommittedResource(&heap,
                                                                      D3D12_HEAP_FLAG_NONE,
                                                                      &description,
                                                                      state,
                                                                      nullptr,
                                                                      GetInterfaceId(resourceOrNull),
                                                                      GetInterfaceOutput(resourceOrNull));
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    outResource = ComOwner<ID3D12Resource>(resourceOrNull);
    return RasterStatus::Ready;
}
void ReleaseUpload(Texture& inoutTexture) noexcept
{
    inoutTexture.Commands.Reset();
    inoutTexture.Allocator.Reset();
    inoutTexture.Upload.Reset();
    inoutTexture.CompletionFence.Reset();
    ReleaseResourceStorage(inoutTexture.StagingBytes);
    inoutTexture.StagingBytes = 0;
    inoutTexture.Fence = 0;
}
void PollTextures() noexcept
{
    for (Texture& texture : gTextures)
    {
        if (texture.Fence == 0)
        {
            continue;
        }
        const uint64 completed = texture.CompletionFence->GetCompletedValue();
        const bool isLost = completed == ~uint64{0};
        if (completed < texture.Fence && isLost == false)
        {
            continue;
        }
        ReleaseUpload(texture);
        if (texture.IsRetired == true)
        {
            texture = {};
        }
        else
        {
            const uint32 request = texture.Request;
            texture.Request = 0;
            RasterComplete(request, isLost == true ? RasterStatus::DeviceLost : RasterStatus::Ready);
        }
    }
}
[[nodiscard]] RasterStatus
CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 count, ComOwner<ID3D12DescriptorHeap>& outHeap) noexcept
{
    if (count == 0)
    {
        return RasterStatus::Ready;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC description{type, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ID3D12DescriptorHeap* heapOrNull = nullptr;
    const HRESULT result = GetDeviceOrNull()->CreateDescriptorHeap(&description,
                                                                   GetInterfaceId(heapOrNull),
                                                                   GetInterfaceOutput(heapOrNull));
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    outHeap = ComOwner<ID3D12DescriptorHeap>(heapOrNull);
    return RasterStatus::Ready;
}
[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE
GetDescriptor(ID3D12DescriptorHeap* heap, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 index) noexcept
{
    LUDUS_ASSERT(heap != nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE result = heap->GetCPUDescriptorHandleForHeapStart();
    result.ptr += usize{index} * GetDeviceOrNull()->GetDescriptorHandleIncrementSize(type);
    return result;
}
void TransitionWholeResource(ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES& inoutState,
                             D3D12_RESOURCE_STATES next) noexcept
{
    if (next == inoutState)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = inoutState;
    barrier.Transition.StateAfter = next;
    GetCommandsOrNull()->ResourceBarrier(1, &barrier);
    inoutState = next;
}
} // namespace

RasterCapabilities RasterLimits() noexcept
{
    if (d3d12::GetDeviceOrNull() == nullptr)
    {
        return {};
    }
    return
    {
        .MaxBufferSize = MAX_BUFFER_BYTES,
        .MaxTextureDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        .UniformOffsetAlignment = CBV_ALIGNMENT,
        .MaxUniformRange = MAX_UNIFORM_BYTES,
        .ResourcesPerKind = CurrentStorageLimits().ResourcesPerKind,
        .DrawsPerFrame = CurrentStorageLimits().DrawsPerFrame,
        .MaxVertexAttributes = MAX_VERTEX_ATTRIBUTES,
        .MaxTextureUploadBytes = MAX_BUFFER_BYTES,
        .HasLineFill = true,
    };
}
BindingCapabilities BindingLimits() noexcept
{
    const uint32 descriptorLimit = CurrentStorageLimits().DescriptorsPerSet;
    if (d3d12::GetDeviceOrNull() == nullptr)
    {
        return {};
    }
    return
    {
        .MaxTextureArrayElements = 1,
        .MaxDescriptorsPerSet = descriptorLimit < MAX_SET_DESCRIPTORS ? descriptorLimit : MAX_SET_DESCRIPTORS,
        .MaxBindings = RASTER_BINDING_COUNT,
        .MaxUniformBuffersPerStage = D3D12_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT,
        .MaxSampledTexturesPerStage = RASTER_BINDING_COUNT,
        .MaxSamplersPerStage = D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT,
        .MaxUniformBuffersPerSet = D3D12_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT,
        .MaxSamplersPerSet = D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT,
    };
}
uint32 RasterColorAttachmentLimit() noexcept
{
    return static_cast<uint32>(MAX_COLOR_ATTACHMENTS);
}
uint32 RasterVertexStrideLimit() noexcept
{
    return MAX_VERTEX_STRIDE_BYTES;
}
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    ID3D12Device* deviceOrNull = d3d12::GetDeviceOrNull();
    if (deviceOrNull == nullptr)
    {
        return {};
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{GetFormat(format),
                                              D3D12_FORMAT_SUPPORT1_NONE,
                                              D3D12_FORMAT_SUPPORT2_NONE};
    if (FAILED(deviceOrNull->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) == true ||
        (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) == 0 ||
        (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) == 0)
    {
        return {};
    }
    const bool hasAttachment = HasDepthFormat(DXGI_FORMAT_D32_FLOAT) == true &&
                               (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0 &&
                               (format == RasterFormat::Rgba8Unorm || format == RasterFormat::Rgba8Srgb ||
                                format == RasterFormat::Rgba16Float);
    const bool hasBlend = hasAttachment == true && (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
    const bool hasCube = (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURECUBE) != 0;
    const bool hasVolume = (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE3D) != 0;
    return
    {
        .Uses =
        {
            .Sampled = true,
            .Filterable = true,
            .ColorAttachment = hasAttachment,
            .Blendable = hasBlend,
            .CopySource = format == RasterFormat::Rgba8Unorm,
            .CopyDestination = format == RasterFormat::Rgba8Unorm,
        },
        .MaxSampledDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        .MaxAttachmentDimension2D = hasAttachment == true ? D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION : 0U,
        .MaxMipLevels = MAX_MIP_LEVELS,
        .SampleCounts = hasAttachment == true ? 1U : 0U,
        .MaxArrayLayers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION,
        .MaxCubeDimension = hasCube == true ? D3D12_REQ_TEXTURECUBE_DIMENSION : 0U,
        .MaxDimension3D = hasVolume == true ? D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : 0U,
        .PartialViews = true,
        .MaxAttachmentMipLevels = hasAttachment == true ? MAX_MIP_LEVELS : 0U,
        .MaxAttachmentCubeDimension = hasAttachment == true && hasCube == true ? D3D12_REQ_TEXTURECUBE_DIMENSION : 0U,
        .MaxAttachmentLayers = hasAttachment == true ? D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION : 0U,
        .AttachmentViews = hasAttachment,
        .DepthSampled = hasAttachment,
        .StencilAttachment = hasAttachment == true && HasDepthFormat(DXGI_FORMAT_D32_FLOAT_S8X24_UINT) == true,
        .HasUnalignedBlockExtents = false,
    };
}
bool GetDescriptorArenaUsage(DescriptorArenaUsage& outUsage) noexcept
{
    outUsage = {};
    for (Set& set : gSets)
    {
        outUsage.Pools += (set.Resources.Get() != nullptr ? 1U : 0U) + (set.Samplers.Get() != nullptr ? 1U : 0U);
        outUsage.ReservedDescriptors += uint64{set.ResourceCount} + set.SamplerCount;
    }
    return true;
}
RasterStatus RasterGrow(RasterKind kind, usize count) noexcept
{
    bool isReady = false;
    switch (kind)
    {
        case RasterKind::Buffer:
            isReady = gBuffers.EnsureSlots(count);
            break;
        case RasterKind::Texture:
            isReady = gTextures.EnsureSlots(count);
            break;
        case RasterKind::View:
            isReady = gViews.EnsureSlots(count);
            break;
        case RasterKind::Sampler:
            isReady = gSamplers.EnsureSlots(count);
            break;
        case RasterKind::Shader:
            isReady = gRasterShaders.EnsureSlots(count);
            break;
        case RasterKind::Layout:
            isReady = gLayouts.EnsureSlots(count);
            break;
        case RasterKind::Set:
            isReady = gSets.EnsureSlots(count);
            break;
        case RasterKind::Pipeline:
            return GrowPipelines(count);
        case RasterKind::ComputePipeline:
            return GrowComputePipelines(count);
        default:
            return RasterStatus::Unsupported;
    }
    return isReady == true ? RasterStatus::Ready : StorageFailure();
}
RasterStatus RasterPrepareStorage() noexcept
{
    PollTextures();
    PollInitialBuffers();
    return gSubmissions.TryResize(CurrentStorageLimits().Submissions) == true ? RasterStatus::Ready : StorageFailure();
}
RasterStatus RasterCreateBuffer(usize slot,
                                const BufferDescription& description,
                                ludus::foundation::BorrowedView<const uint8> bytes,
                                uint32 request) noexcept
{
    return CreateInitialBuffer({slot, request}, description, {bytes.GetDataOrNull(), bytes.GetSize()});
}
RasterStatus RasterCreateTexture(usize slot,
                                 const TextureDescription& description,
                                 const TextureUpload& upload,
                                 uint32 request) noexcept
{
    PollTextures();
    Texture& texture = gTextures[slot];
    if (texture.Fence != 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    const uint32 layers = description.Dimension == TextureDimension::D3 ? 1 : description.DepthOrLayers;
    const uint32 count = layers * description.MipLevels;
    if (count > MAX_TEXTURE_SUBRESOURCES)
    {
        return RasterStatus::InvalidDescription;
    }
    D3D12_RESOURCE_DESC native{};
    native.Dimension = description.Dimension == TextureDimension::D3 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                     : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    native.Width = description.Width;
    native.Height = description.Height;
    native.DepthOrArraySize = static_cast<uint16>(description.DepthOrLayers);
    native.MipLevels = static_cast<uint16>(description.MipLevels);
    native.Format = GetFormat(description.Format);
    native.SampleDesc.Count = description.SampleCount;
    native.Flags = description.Attachment == true ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    const D3D12_RESOURCE_STATES initialState =
        description.Attachment == true ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST;
    RasterStatus status = CreateResource(native, D3D12_HEAP_TYPE_DEFAULT, initialState, texture.Resource);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    texture.Description = description;
    texture.State = initialState;
    if (description.Attachment == true)
    {
        const DepthFormats formats = GetDepthFormats(description.Stencil);
        D3D12_RESOURCE_DESC depth = native;
        depth.Format = formats.Backing;
        depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        status = CreateResource(depth, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE, texture.Depth);
        if (status != RasterStatus::Ready)
        {
            return status;
        }
        texture.DepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        for (const D3D12_DESCRIPTOR_HEAP_TYPE type : {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_TYPE_DSV})
        {
            const D3D12_DESCRIPTOR_HEAP_DESC heap{type, count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            ID3D12DescriptorHeap* heapOrNull = nullptr;
            const HRESULT result = GetDeviceOrNull()->CreateDescriptorHeap(&heap,
                                                                           GetInterfaceId(heapOrNull),
                                                                           GetInterfaceOutput(heapOrNull));
            if (FAILED(result) == true)
            {
                return GetFailure(result);
            }
            if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
            {
                texture.Targets = ComOwner<ID3D12DescriptorHeap>(heapOrNull);
            }
            else
            {
                texture.Depths = ComOwner<ID3D12DescriptorHeap>(heapOrNull);
            }
        }
        for (uint32 layer = 0; layer < layers; ++layer)
        {
            for (uint32 mip = 0; mip < description.MipLevels; ++mip)
            {
                const uint32 subresource = mip + layer * description.MipLevels;
                D3D12_RENDER_TARGET_VIEW_DESC target{};
                target.Format = native.Format;
                target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                target.Texture2DArray.MipSlice = mip;
                target.Texture2DArray.FirstArraySlice = layer;
                target.Texture2DArray.ArraySize = 1;
                GetDeviceOrNull()->CreateRenderTargetView(
                    texture.Resource.Get(),
                    &target,
                    GetDescriptor(texture.Targets.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, subresource));
                D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
                depthView.Format = formats.Attachment;
                depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                depthView.Texture2DArray.MipSlice = mip;
                depthView.Texture2DArray.FirstArraySlice = layer;
                depthView.Texture2DArray.ArraySize = 1;
                GetDeviceOrNull()->CreateDepthStencilView(
                    texture.Depth.Get(),
                    &depthView,
                    GetDescriptor(texture.Depths.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, subresource));
            }
        }
        return RasterStatus::Ready;
    }
    if (description.DeferredInitialization == true)
    {
        return RasterStatus::Ready;
    }
    StorageArray<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    if (footprints.TryResize(count) == false)
    {
        return StorageFailure();
    }
    uint64 stagingBytes = 0;
    GetDeviceOrNull()
        ->GetCopyableFootprints(&native, 0, count, 0, footprints.GetData(), nullptr, nullptr, &stagingBytes);
    if (stagingBytes == ~uint64{0} || stagingBytes > ~usize{0})
    {
        return RasterStatus::CapacityExceeded;
    }
    if (ReserveResourceStorage(stagingBytes) == false)
    {
        return StorageFailure();
    }
    texture.StagingBytes = stagingBytes;
    status = CreateResource(MakeBufferDescription(static_cast<usize>(stagingBytes)),
                            D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ,
                            texture.Upload);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    void* mappedOrNull = nullptr;
    const D3D12_RANGE readRange{};
    HRESULT result = texture.Upload->Map(0, &readRange, &mappedOrNull);
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    uint8* mapped = static_cast<uint8*>(mappedOrNull);
    usize sourceMipOffset = 0;
    for (uint32 mip = 0; mip < description.MipLevels; ++mip)
    {
        const uint32 height = TextureMipExtent(description.Height, mip);
        const usize rows = TextureBlockRows(description.Format, height);
        const usize slices = TextureSlices(description, mip);
        const usize pitch = upload.Mips.IsEmpty() == true ? upload.RowPitch : upload.Mips[mip].RowPitch;
        const usize offset = upload.Mips.IsEmpty() == true ? sourceMipOffset : upload.Mips[mip].Offset;
        const usize rowBytes = usize{TextureBlockRows(description.Format, TextureMipExtent(description.Width, mip))} *
                               TextureBlockBytes(description.Format);
        for (usize slice = 0; slice < slices; ++slice)
        {
            const usize subresource =
                description.Dimension == TextureDimension::D3 ? mip : mip + slice * description.MipLevels;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[subresource];
            const usize destinationSlice = description.Dimension == TextureDimension::D3 ? slice : 0;
            for (usize row = 0; row < rows; ++row)
            {
                std::memcpy(mapped + footprint.Offset + (destinationSlice * rows + row) * footprint.Footprint.RowPitch,
                            upload.Bytes.GetDataOrNull() + offset + (slice * rows + row) * pitch,
                            rowBytes);
            }
        }
        sourceMipOffset += pitch * rows * slices;
    }
    texture.Upload->Unmap(0, nullptr);
    ID3D12CommandAllocator* allocatorOrNull = nullptr;
    result = GetDeviceOrNull()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       GetInterfaceId(allocatorOrNull),
                                                       GetInterfaceOutput(allocatorOrNull));
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    texture.Allocator = ComOwner<ID3D12CommandAllocator>(allocatorOrNull);
    ID3D12GraphicsCommandList* commandsOrNull = nullptr;
    result = GetDeviceOrNull()->CreateCommandList(0,
                                                  D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  texture.Allocator.Get(),
                                                  nullptr,
                                                  GetInterfaceId(commandsOrNull),
                                                  GetInterfaceOutput(commandsOrNull));
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    texture.Commands = ComOwner<ID3D12GraphicsCommandList>(commandsOrNull);
    for (uint32 subresource = 0; subresource < count; ++subresource)
    {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = texture.Upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[subresource];
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.Resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresource;
        texture.Commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    texture.Commands->ResourceBarrier(1, &barrier);
    result = texture.Commands->Close();
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    texture.Description = description;
    texture.Request = request;
    ID3D12Fence* fence = GetFenceOrNull();
    LUDUS_ASSERT(fence != nullptr);
    fence->AddRef();
    texture.CompletionFence = ComOwner<ID3D12Fence>(fence);
    if (SubmitCreation(texture.Commands.Get(), texture.Fence) == false)
    {
        return RasterStatus::DeviceLost;
    }
    // Return Pending even if the driver finished immediately. Completion is delivered
    // by the next facade collection, after the request identity has been published.
    texture.State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    RasterExpect(request, RasterCallbacks::One);
    return RasterStatus::Pending;
}
RasterStatus RasterCreateView(usize slot, const RasterViewSource& source, uint32) noexcept
{
    // The facade admits a depth view only as one D2 subresource of an attachment texture.
    // Stencil loads would need plane slice 1 and a remapped component; they stay unsupported.
    const bool isDepth = source.View.Aspect == TextureAspect::Depth;
    if (source.View.Aspect == TextureAspect::Stencil)
    {
        return RasterStatus::Unsupported;
    }
    const TextureDescription& texture = gTextures[source.Texture].Description;
    const TextureViewDescription& view = source.View;
    D3D12_SHADER_RESOURCE_VIEW_DESC native{};
    native.Format = isDepth == true ? GetDepthFormats(texture.Stencil).Sampled : GetFormat(texture.Format);
    native.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (view.Dimension)
    {
        case TextureDimension::D2:
            if (texture.DepthOrLayers == 1)
            {
                native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                native.Texture2D.MostDetailedMip = view.BaseMip;
                native.Texture2D.MipLevels = view.MipCount;
            }
            else
            {
                native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                native.Texture2DArray.MostDetailedMip = view.BaseMip;
                native.Texture2DArray.MipLevels = view.MipCount;
                native.Texture2DArray.FirstArraySlice = view.BaseLayer;
                native.Texture2DArray.ArraySize = 1;
            }
            break;
        case TextureDimension::D2Array:
            native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            native.Texture2DArray.MostDetailedMip = view.BaseMip;
            native.Texture2DArray.MipLevels = view.MipCount;
            native.Texture2DArray.FirstArraySlice = view.BaseLayer;
            native.Texture2DArray.ArraySize = view.LayerCount;
            break;
        case TextureDimension::Cube:
            native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            native.TextureCube.MostDetailedMip = view.BaseMip;
            native.TextureCube.MipLevels = view.MipCount;
            break;
        case TextureDimension::D3:
            native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            native.Texture3D.MostDetailedMip = view.BaseMip;
            native.Texture3D.MipLevels = view.MipCount;
            break;
    }
    if (view.Attachment == true)
    {
        gViews[slot] = { .TextureSlot = source.Texture, .Description = native };
        return RasterStatus::Ready;
    }
    // D3D12 cannot bind a one-layer array SRV as Texture2D. Do not reinterpret it.
    if (view.Dimension == TextureDimension::D2 && texture.Dimension != TextureDimension::D2)
    {
        return RasterStatus::Unsupported;
    }
    gViews[slot] = { .TextureSlot = source.Texture, .Description = native, .IsDepth = isDepth };
    return RasterStatus::Ready;
}
RasterStatus RasterCreateSampler(usize slot, const SamplerDescription& description, uint32) noexcept
{
    D3D12_SAMPLER_DESC native = Sampler{}.Description;
    const bool isLinear = description.Filter == RasterFilter::Linear;
    const bool hasLinearMips = description.MipFilter == RasterMipFilter::Linear;
    native.Filter =
        isLinear == true
            ? (hasLinearMips == true ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT)
            : (hasLinearMips == true ? D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT);
    native.AddressU =
        description.U == RasterAddress::Clamp ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    native.AddressV =
        description.V == RasterAddress::Clamp ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    native.AddressW =
        description.W == RasterAddress::Clamp ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    native.MaxAnisotropy = 1;
    native.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    native.MinLOD = description.MinLod;
    native.MaxLOD = description.MipFilter == RasterMipFilter::None ? 0 : description.MaxLod;
    gSamplers[slot].Description = native;
    return RasterStatus::Ready;
}
RasterStatus
RasterCreateShader(usize slot, const ShaderDescription& description, const RasterShaderInfo& info, uint32) noexcept
{
    if ((info.Stage == ShaderStage::Compute &&
         HasDxilWorkgroupSize({description.Dxil.GetDataOrNull(), description.Dxil.GetSize()}, info.WorkgroupSize) ==
             false) ||
        HasDxilEntry({description.Dxil.GetDataOrNull(), description.Dxil.GetSize()},
                     info.Stage,
                     description.DxilEntry) == false ||
        HasDxilBindings({description.Dxil.GetDataOrNull(), description.Dxil.GetSize()}, info.Layout) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    Shader& shader = gRasterShaders[slot];
    if (shader.Bytes.TryResize(description.Dxil.GetSize()) == false)
    {
        return StorageFailure();
    }
    std::memcpy(shader.Bytes.GetData(), description.Dxil.GetDataOrNull(), description.Dxil.GetSize());
    return RasterStatus::Ready;
}
RasterStatus RasterCreateLayout(usize slot, const RasterLayout& layout, uint32) noexcept
{
    StaticArray<D3D12_DESCRIPTOR_RANGE, DESCRIPTOR_TABLE_COUNT> ranges{};
    StaticArray<D3D12_ROOT_PARAMETER, DESCRIPTOR_TABLE_COUNT> parameters{};
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        const RasterBinding& binding = layout.Entries[entry];
        const bool isStorage =
            binding.Kind == RasterBindingKind::StorageRead || binding.Kind == RasterBindingKind::StorageReadWrite;
        // Structured reads also serve fragment stages as SRVs (the deferred lighting resolve reads
        // its light and LTC tables there); structured writes stay in compute, as the portable
        // contract requires (D6c).
        const bool isStorageVisible =
            binding.Visibility == RasterVisibility::Compute ||
            (binding.Kind == RasterBindingKind::StorageRead && binding.Visibility == RasterVisibility::Fragment);
        constexpr usize MAX_PORTABLE_STORAGE_STRIDE = sizeof(uint32) * 4;
        if (binding.Count != 1 || binding.HasDynamicOffset == true ||
            (isStorage == true && (isStorageVisible == false || binding.MinSize > MAX_PORTABLE_STORAGE_STRIDE ||
                                   (MAX_PORTABLE_STORAGE_STRIDE % binding.MinSize) != 0)) ||
            (binding.Kind != RasterBindingKind::UniformBuffer && binding.Kind != RasterBindingKind::Sampler &&
             TextureBinding(binding.Kind) == false && isStorage == false))
        {
            return RasterStatus::Unsupported;
        }
        D3D12_DESCRIPTOR_RANGE& range = ranges[entry];
        range.RangeType = binding.Kind == RasterBindingKind::UniformBuffer      ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                          : binding.Kind == RasterBindingKind::Sampler          ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                          : binding.Kind == RasterBindingKind::StorageReadWrite ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                                                                : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = binding.Binding;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER& parameter = parameters[entry];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility = binding.Visibility == RasterVisibility::Vertex     ? D3D12_SHADER_VISIBILITY_VERTEX
                                     : binding.Visibility == RasterVisibility::Fragment ? D3D12_SHADER_VISIBILITY_PIXEL
                                                                                        : D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(layout.Count);
    description.pParameters = parameters.GetData();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ID3DBlob* blobOrNull = nullptr;
    ID3DBlob* errorsOrNull = nullptr;
    HRESULT result =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blobOrNull, &errorsOrNull);
    ComOwner<ID3DBlob> blob(blobOrNull);
    ComOwner<ID3DBlob> errors(errorsOrNull);
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    ID3D12RootSignature* signatureOrNull = nullptr;
    result = GetDeviceOrNull()->CreateRootSignature(0,
                                                    blob->GetBufferPointer(),
                                                    blob->GetBufferSize(),
                                                    GetInterfaceId(signatureOrNull),
                                                    GetInterfaceOutput(signatureOrNull));
    if (FAILED(result) == true)
    {
        return GetFailure(result);
    }
    gLayouts[slot].Signature = ComOwner<ID3D12RootSignature>(signatureOrNull);
    return RasterStatus::Ready;
}
RasterStatus RasterCreateSet(usize slot, const RasterLayout& layout, const RasterSet& snapshot, uint32) noexcept
{
    Set candidate;
    candidate.Description = layout;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        if (layout.Entries[entry].Kind == RasterBindingKind::Sampler)
        {
            ++candidate.SamplerCount;
        }
        else
        {
            ++candidate.ResourceCount;
        }
    }
    RasterStatus status =
        CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, candidate.ResourceCount, candidate.Resources);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    status = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, candidate.SamplerCount, candidate.Samplers);
    if (status != RasterStatus::Ready)
    {
        return status;
    }
    uint32 resourceIndex = 0;
    uint32 samplerIndex = 0;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        const RasterBinding& binding = layout.Entries[entry];
        const usize resourceSlot = snapshot.Slots[entry];
        if (binding.Kind == RasterBindingKind::Sampler)
        {
            GetDeviceOrNull()->CreateSampler(
                &gSamplers[resourceSlot].Description,
                GetDescriptor(candidate.Samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, samplerIndex++));
            continue;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE descriptor =
            GetDescriptor(candidate.Resources.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resourceIndex++);
        if (binding.Kind == RasterBindingKind::UniformBuffer)
        {
            const usize size = snapshot.Sizes[entry];
            const usize roundedSize = (size + CBV_ALIGNMENT - 1) / CBV_ALIGNMENT * CBV_ALIGNMENT;
            const D3D12_CONSTANT_BUFFER_VIEW_DESC uniform{gBuffers[resourceSlot].Resource->GetGPUVirtualAddress() +
                                                              snapshot.Offsets[entry],
                                                          static_cast<UINT>(roundedSize)};
            GetDeviceOrNull()->CreateConstantBufferView(&uniform, descriptor);
        }
        else if (binding.Kind == RasterBindingKind::StorageRead || binding.Kind == RasterBindingKind::StorageReadWrite)
        {
            const usize stride = binding.MinSize;
            const usize offset = snapshot.Offsets[entry];
            const usize size = snapshot.Sizes[entry];
            if (offset % stride != 0 || size % stride != 0)
            {
                return RasterStatus::InvalidDescription;
            }
            ID3D12Resource* resource = gBuffers[resourceSlot].Resource.Get();
            candidate.BufferSlots[entry] = resourceSlot;
            const uint64 firstElement = offset / stride;
            const uint32 elementCount = static_cast<uint32>(size / stride);
            const uint32 elementStride = static_cast<uint32>(stride);
            if (binding.Kind == RasterBindingKind::StorageReadWrite)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC storage{};
                storage.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                storage.Buffer.FirstElement = firstElement;
                storage.Buffer.NumElements = elementCount;
                storage.Buffer.StructureByteStride = elementStride;
                GetDeviceOrNull()->CreateUnorderedAccessView(resource, nullptr, &storage, descriptor);
            }
            else
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC storage{};
                storage.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                storage.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                storage.Buffer.FirstElement = firstElement;
                storage.Buffer.NumElements = elementCount;
                storage.Buffer.StructureByteStride = elementStride;
                GetDeviceOrNull()->CreateShaderResourceView(resource, &storage, descriptor);
            }
        }
        else
        {
            const View& view = gViews[resourceSlot];
            const Texture& texture = gTextures[view.TextureSlot];
            GetDeviceOrNull()->CreateShaderResourceView(view.IsDepth == true ? texture.Depth.Get()
                                                                             : texture.Resource.Get(),
                                                        &view.Description,
                                                        descriptor);
        }
    }
    gSets[slot] = Move(candidate);
    return RasterStatus::Ready;
}
void RasterDestroy(RasterKind kind, usize slot) noexcept
{
    switch (kind)
    {
        case RasterKind::Buffer:
            gBuffers[slot] = {};
            break;
        case RasterKind::Texture: {
            Texture& texture = gTextures[slot];
            if (texture.Fence != 0)
            {
                texture.IsRetired = true;
                texture.Request = 0;
            }
            else
            {
                ReleaseUpload(texture);
                texture = {};
            }
            break;
        }
        case RasterKind::View:
            gViews[slot] = {};
            break;
        case RasterKind::Sampler:
            gSamplers[slot] = {};
            break;
        case RasterKind::Shader:
            gRasterShaders[slot].Bytes.Clear();
            break;
        case RasterKind::Layout:
            gLayouts[slot] = {};
            break;
        case RasterKind::Pipeline:
            DestroyPipeline(slot);
            break;
        case RasterKind::ComputePipeline:
            DestroyComputePipeline(slot);
            break;
        case RasterKind::Set:
            gSets[slot] = {};
            break;
        default:
            break;
    }
}
RasterStatus RasterReserveSubmission() noexcept
{
    if (gReservedSubmission != RASTER_INVALID)
    {
        return RasterStatus::Ready;
    }
    for (usize index = 0; index < gSubmissions.GetSize(); ++index)
    {
        if (gSubmissions[index].Ordinal == 0)
        {
            gReservedSubmission = index;
            return RasterStatus::Ready;
        }
    }
    return RasterStatus::CapacityExceeded;
}
void RasterDiscardSubmission() noexcept
{
    gReservedSubmission = RASTER_INVALID;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    LUDUS_ASSERT(gReservedSubmission != RASTER_INVALID);
    gSubmissions[gReservedSubmission] = { .Ordinal = ordinal, .Fence = GetLastFence() };
    gReservedSubmission = RASTER_INVALID;
}
uint64 RasterCompleted() noexcept
{
    PollTextures();
    PollInitialBuffers();
    const uint64 completed = GetCompletedFence();
    for (Submission& submission : gSubmissions)
    {
        if (submission.Ordinal != 0 && submission.Fence <= completed)
        {
            if (submission.Ordinal > gCompletedOrdinal)
            {
                gCompletedOrdinal = submission.Ordinal;
            }
            submission = {};
        }
    }
    return gCompletedOrdinal;
}
void RasterReset() noexcept
{
    LogDeviceRemoval(GetDeviceOrNull());
    DiscardRecording();
    const bool isDrained = WaitForQueue();
    ResetRasterPasses();
    PollTextures();
    if (isDrained == true)
    {
        for (Texture& texture : gTextures)
        {
            if (texture.Fence != 0 && texture.CompletionFence.Get() == GetFenceOrNull())
            {
                const uint32 request = texture.Request;
                ReleaseUpload(texture);
                texture.Request = 0;
                RasterComplete(request, RasterStatus::Ready);
            }
        }
    }
    for (Submission& submission : gSubmissions)
    {
        submission = {};
    }
    gReservedSubmission = RASTER_INVALID;
    gCompletedOrdinal = 0;
}
void RasterShutdown() noexcept
{
    // Reset/drain precedes facade physical destruction, including rejected uploads
    // whose signal failed. A removed device has no remaining usable GPU work.
    for (Texture& texture : gTextures)
    {
        if (texture.Fence == 0)
        {
            ReleaseUpload(texture);
            texture = {};
        }
        else
        {
            // A timeout is not completion. Keep backing and its original fence
            // across device restart; this native slot cannot be reused early.
            texture.IsRetired = true;
            texture.Request = 0;
        }
    }
    gSubmissions.Clear();
}
void RasterTextureBarrier(usize slot, RasterTextureUse use) noexcept
{
    Texture& texture = gTextures[slot];
    constexpr D3D12_RESOURCE_STATES SAMPLED =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool isAttachment = use == RasterTextureUse::ColorAttachment;
    TransitionWholeResource(texture.Resource.Get(),
                            texture.State,
                            isAttachment == true ? D3D12_RESOURCE_STATE_RENDER_TARGET : SAMPLED);
    // As in the Vulkan backend, the private depth backing follows the texture: depth testing
    // in an attachment pass, shader reads through a depth view elsewhere.
    if (texture.Depth.Get() != nullptr)
    {
        TransitionWholeResource(texture.Depth.Get(),
                                texture.DepthState,
                                isAttachment == true ? D3D12_RESOURCE_STATE_DEPTH_WRITE : SAMPLED);
    }
}
// Thanks to Microsoft, "Using Resource Barriers to Synchronize Resource States
// in Direct3D 12", for buffer decay after ExecuteCommandLists and UAV ordering.
// https://learn.microsoft.com/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
void ComputeBufferBarrier(usize slot, GraphAccessMode use) noexcept
{
    Buffer& buffer = gBuffers[slot];
    if (buffer.HasStorage == false)
    {
        return;
    }
    const D3D12_RESOURCE_STATES next = use == GraphAccessMode::StorageReadWrite ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                                                : D3D12_RESOURCE_STATE_GENERIC_READ;
    D3D12_RESOURCE_BARRIER barrier{};
    if (buffer.State != next)
    {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {buffer.Resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, buffer.State, next};
        buffer.State = next;
    }
    else if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = buffer.Resource.Get();
    }
    else
    {
        return;
    }
    GetCommandsOrNull()->ResourceBarrier(1, &barrier);
}
namespace d3d12
{
DXGI_FORMAT GetRasterFormat(RasterFormat format) noexcept
{
    return GetFormat(format);
}
void SetBuffer(usize slot, bool hasStorage, ComOwner<ID3D12Resource>&& resource) noexcept
{
    gBuffers[slot] = { .Resource = Move(resource), .HasStorage = hasStorage };
}
void ResetBufferStates() noexcept
{
    // Every transfer and frame is a separate ExecuteCommandLists call on the
    // same queue. Begin tracking again when a fresh frame list starts.
    for (Buffer& buffer : gBuffers)
    {
        buffer.State = D3D12_RESOURCE_STATE_COMMON;
    }
}
ID3D12Resource* GetTexture(usize slot) noexcept
{
    return gTextures[slot].Resource.Get();
}
D3D12_RESOURCE_STATES GetTextureState(usize slot) noexcept
{
    return gTextures[slot].State;
}
void SetTextureState(usize slot, D3D12_RESOURCE_STATES state) noexcept
{
    gTextures[slot].State = state;
}
ID3D12Resource* GetBuffer(usize slot) noexcept
{
    return gBuffers[slot].Resource.Get();
}
D3D12_SHADER_BYTECODE GetShader(usize slot) noexcept
{
    const Shader& shader = gRasterShaders[slot];
    return {shader.Bytes.GetData(), shader.Bytes.GetSize()};
}
ID3D12RootSignature* GetSignature(usize slot) noexcept
{
    return gLayouts[slot].Signature.Get();
}
Attachment GetAttachment(usize slot, uint32 mip, uint32 layer) noexcept
{
    Texture& texture = gTextures[slot];
    const uint32 index = mip + layer * texture.Description.MipLevels;
    return {texture.Resource.Get(),
            texture.Depth.Get(),
            GetDescriptor(texture.Targets.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, index),
            GetDescriptor(texture.Depths.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, index)};
}
void BindSet(ID3D12GraphicsCommandList* commands, usize slot, ShaderStage stage) noexcept
{
    const Set& set = gSets[slot];
    if (stage != ShaderStage::Compute)
    {
        // The ordered graph transitions the buffers a dispatch declares. A raster draw binds its
        // storage reads through the set alone, so make each readable by the pixel shader here.
        for (usize entry = 0; entry < set.Description.Count; ++entry)
        {
            if (set.Description.Entries[entry].Kind == RasterBindingKind::StorageRead)
            {
                ComputeBufferBarrier(set.BufferSlots[entry], GraphAccessMode::StorageRead);
            }
        }
    }
    constexpr usize MAX_BOUND_HEAPS = 2;
    StaticArray<ID3D12DescriptorHeap*, MAX_BOUND_HEAPS> heaps{};
    uint32 count = 0;
    if (set.Resources.Get() != nullptr)
    {
        heaps[count++] = set.Resources.Get();
    }
    if (set.Samplers.Get() != nullptr)
    {
        heaps[count++] = set.Samplers.Get();
    }
    if (count != 0)
    {
        commands->SetDescriptorHeaps(count, heaps.GetData());
    }
    uint32 resourceIndex = 0;
    uint32 samplerIndex = 0;
    for (usize entry = 0; entry < set.Description.Count; ++entry)
    {
        const bool isSampler = set.Description.Entries[entry].Kind == RasterBindingKind::Sampler;
        ID3D12DescriptorHeap* heap = isSampler == true ? set.Samplers.Get() : set.Resources.Get();
        const D3D12_DESCRIPTOR_HEAP_TYPE type =
            isSampler == true ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        const uint32 index = isSampler == true ? samplerIndex++ : resourceIndex++;
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += uint64{index} * GetDeviceOrNull()->GetDescriptorHandleIncrementSize(type);
        if (stage == ShaderStage::Compute)
        {
            commands->SetComputeRootDescriptorTable(static_cast<UINT>(entry), handle);
        }
        else
        {
            commands->SetGraphicsRootDescriptorTable(static_cast<UINT>(entry), handle);
        }
    }
}
} // namespace d3d12
} // namespace ludus::graphics::rhi::backend
