#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/graph.h>

#include "storage.h"
#include "texture.h"

namespace ludus::graphics::rhi::internal
{
inline constexpr foundation::usize RASTER_CAPACITY = 64;
inline constexpr foundation::usize RASTER_INVALID = foundation::usize{0xffffffffU};
inline constexpr foundation::usize RASTER_BINDINGS = RASTER_BINDING_COUNT;
inline constexpr foundation::usize RASTER_DRAWS = 1024;
inline constexpr foundation::usize LIFETIME_BATCHES = 4;
inline constexpr foundation::usize LIFETIME_BATCH_DRAWS = 256;
inline constexpr foundation::usize LIFETIME_TRANSFERS = 4;
inline constexpr foundation::usize LIFETIME_TRANSFER_BYTES = foundation::usize{256} * 1024;
// Shared by callback requests and submission ordinals; zero stays exhausted.
template <typename T>
struct RasterSequence final
{
    T Next = 1;
    [[nodiscard]] T Take() noexcept
    {
        const T value = Next;
        Next = value == 0 || value == static_cast<T>(~T{0}) ? 0 : static_cast<T>(value + 1);
        return value;
    }
};
[[nodiscard]] inline foundation::uint64 RasterNextGeneration(foundation::uint64 value) noexcept
{
    return value == 0 || value == ~foundation::uint64{0} ? 0 : value + 1;
}
enum class RasterKind : foundation::uint8
{
    Buffer,
    Texture,
    View,
    Sampler,
    Shader,
    Layout,
    Set,
    Pipeline,
    ComputePipeline,
    RayGeometry,
    RayScene,
    RayProgram,
    RayBatch,
    Count
};
struct DescriptorArenaUsage final
{
    foundation::uint64 Pools = 0;
    foundation::uint64 ReservedDescriptors = 0;
};
struct RasterLayout final
{
    // Elements use their default member initializers; the check misreports them as zero enums.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    RasterBinding Entries[RASTER_BINDINGS]{};
    foundation::usize Count = 0;
    foundation::usize DescriptorCount = 0;
    [[nodiscard]] const RasterBinding& GetDescriptorBinding(foundation::usize descriptor) const noexcept
    {
        for (foundation::usize entry = 0; entry < Count; ++entry)
        {
            if (descriptor < Entries[entry].Count)
            {
                return Entries[entry];
            }
            descriptor -= Entries[entry].Count;
        }
        LUDUS_UNREACHABLE();
    }
    [[nodiscard]] foundation::uint32 GetArrayElement(foundation::usize descriptor) const noexcept
    {
        for (foundation::usize entry = 0; entry < Count; ++entry)
        {
            if (descriptor < Entries[entry].Count)
            {
                return static_cast<foundation::uint32>(descriptor);
            }
            descriptor -= Entries[entry].Count;
        }
        LUDUS_UNREACHABLE();
    }
};
// Vulkan and WebGPU consume dynamic offsets in increasing binding-number order.
// Thanks to Khronos Group, Vulkan Specification, vkCmdBindDescriptorSets,
// https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindDescriptorSets.html
// for the ordering contract; snapshots remain immutable across every backend.
inline foundation::uint32 CollectDynamicOffsets(const RasterLayout& layout,
                                                const RasterBindingOffsets& offsets,
                                                foundation::uint32 (&outBytes)[RASTER_BINDINGS]) noexcept
{
    foundation::uint32 count = 0;
    for (foundation::usize binding = 0; binding < RASTER_BINDINGS; ++binding)
    {
        for (foundation::usize entry = 0; entry < layout.Count; ++entry)
        {
            if (layout.Entries[entry].Binding == binding && layout.Entries[entry].HasDynamicOffset == true)
            {
                outBytes[count++] = offsets.Bytes[binding];
            }
        }
    }
    return count;
}
struct RasterShaderInfo final
{
    ShaderStage Stage = ShaderStage::Vertex;
    RasterLayout Layout;
    RasterShaderInput Inputs[16]{};
    foundation::usize InputCount = 0;
    foundation::uint32 WorkgroupSize[3]{1, 1, 1};
    char UniformBlocks[RASTER_BINDINGS][64]{};
    char TextureNames[RASTER_BINDINGS][64]{};
    foundation::uint32 TextureSamplers[RASTER_BINDINGS]{};
};
struct RasterViewSource final
{
    foundation::usize Texture;
    TextureViewDescription View;
};
struct RasterSet final
{
    foundation::usize Layout = 0;
    RasterLayout Description;
    StorageArray<foundation::usize> Slots;
    StorageArray<foundation::usize> Offsets;
    StorageArray<foundation::usize> Sizes;
    StorageArray<bool> IsPopulated;
    [[nodiscard]] bool TryResize(foundation::usize count) noexcept
    {
        return Slots.TryResize(count) == true && Offsets.TryResize(count) == true && Sizes.TryResize(count) == true &&
               IsPopulated.TryResize(count) == true;
    }
    [[nodiscard]] bool TryCopyFrom(const RasterSet& source) noexcept
    {
        if (TryResize(source.Slots.GetSize()) == false)
        {
            return false;
        }
        Layout = source.Layout;
        Description = source.Description;
        for (foundation::usize descriptor = 0; descriptor < Slots.GetSize(); ++descriptor)
        {
            Slots[descriptor] = source.Slots[descriptor];
            Offsets[descriptor] = source.Offsets[descriptor];
            Sizes[descriptor] = source.Sizes[descriptor];
            IsPopulated[descriptor] = source.IsPopulated[descriptor];
        }
        return true;
    }
};
struct RasterPipelineInfo final
{
    foundation::usize Vertex = 0;
    foundation::usize Fragment = 0;
    foundation::usize Layout = 0;
    RasterVertexStream Streams[2]{};
    RasterVertexAttribute Attributes[16]{};
    foundation::usize StreamCount = 0;
    foundation::usize AttributeCount = 0;
    bool Depth = false;
    bool Blend = false;
    RasterTarget Target = RasterTarget::Surface;
    bool DepthWrite = true;
    RasterDepthCompare DepthCompare = RasterDepthCompare::Less;
    RasterFormat ColorFormats[4]{};
    foundation::usize ColorCount = 0;
    foundation::uint32 SampleCount = 1;
    bool Stencil = false;
    RasterStencilFace StencilFront{}, StencilBack{};
    foundation::uint32 StencilReadMask = 255, StencilWriteMask = 255, StencilReference = 0;
};
struct ComputePipelineInfo final
{
    foundation::usize Shader = 0;
    foundation::usize Layout = 0;
    foundation::uint32 WorkgroupSize[3]{1, 1, 1};
};
struct ComputePacket final
{
    foundation::usize Pipeline = 0;
    foundation::usize Set = 0;
    foundation::uint32 Groups[3]{1, 1, 1};
    RasterBindingOffsets BindingOffsets{};
    foundation::uint32 DynamicOffsets[RASTER_BINDINGS]{};
    foundation::uint32 DynamicOffsetCount = 0;
};
struct RasterPacket final
{
    foundation::usize Pipeline = 0;
    foundation::usize Set = 0;
    foundation::usize Vertices[2]{};
    foundation::usize Offsets[2]{};
    foundation::usize Sizes[2]{};
    foundation::usize Indices = 0;
    foundation::usize IndexOffset = 0;
    foundation::uint32 IndexCount = 0;
    foundation::uint32 InstanceCount = 0;
    bool Index32 = false;
    foundation::usize Indirect = RASTER_INVALID;
    foundation::usize IndirectOffset = 0;
    RasterBindingOffsets BindingOffsets{};
    foundation::uint32 DynamicOffsets[RASTER_BINDINGS]{};
    foundation::uint32 DynamicOffsetCount = 0;
};
struct RasterAttachmentInfo final
{
    foundation::usize View = RASTER_INVALID, Texture = RASTER_INVALID;
    foundation::usize ResolveView = RASTER_INVALID, ResolveTexture = RASTER_INVALID;
    foundation::uint32 Mip = 0, Layer = 0, ResolveMip = 0, ResolveLayer = 0;
    RasterFormat Format = RasterFormat::Rgba8Unorm;
    RasterLoad Load = RasterLoad::Clear;
    RasterStore Store = RasterStore::Store;
    foundation::float32 Clear[4]{0, 0, 0, 1};
};
struct RasterPassInfo final
{
    foundation::usize Texture = RASTER_INVALID;
    RasterPassDescription Description;
    RasterAttachmentInfo Colors[4]{};
    foundation::usize ColorCount = 0;
    foundation::uint32 Width = 0, Height = 0, SampleCount = 1;
    bool Stencil = false;
};
// Canonical rectangles stay top-left; API-specific Y conversion occurs only in backends.
struct RasterArea final
{
    RasterRectangle Viewport{}, Scissor{};
    bool Empty = false;
};
inline bool RasterResolveArea(const RasterPassDescription& pass,
                              foundation::uint32 width,
                              foundation::uint32 height,
                              RasterArea& area) noexcept
{
    const RasterRectangle full{0, 0, width, height};
    area.Viewport = pass.UseViewport ? pass.Viewport : full;
    const auto& v = area.Viewport;
    if (v.Width == 0 || v.Height == 0 || v.X > width || v.Y > height || v.Width > width - v.X ||
        v.Height > height - v.Y)
    {
        return false;
    }
    area.Scissor = pass.UseScissor ? pass.Scissor : full;
    auto& s = area.Scissor;
    if (s.X > width)
    {
        s.X = width;
    }
    if (s.Y > height)
    {
        s.Y = height;
    }
    if (s.Width > width - s.X)
    {
        s.Width = width - s.X;
    }
    if (s.Height > height - s.Y)
    {
        s.Height = height - s.Y;
    }
    area.Empty = s.Width == 0 || s.Height == 0;
    return true;
}
struct LifetimeUploadTarget final
{
    foundation::usize Buffer = 0;
    foundation::uint32 Request = 0;
};
struct LifetimeCopyRange final
{
    foundation::usize Buffer = 0;
    foundation::usize Offset = 0;
    foundation::usize Size = 0;
};
enum class TextureTransferOperation : foundation::uint8
{
    Update,
    Copy,
    Readback
};
struct TextureTransfer final
{
    TextureTransferOperation Operation = TextureTransferOperation::Update;
    foundation::usize Source = 0;
    foundation::usize Destination = 0;
    TextureRegion SourceRegion;
    TextureRegion DestinationRegion;
    foundation::usize RowPitch = 0;
    foundation::usize Size = 0;
};
bool IsBindlessRequested() noexcept;
bool IsBindlessRequired() noexcept;
void SetBindlessRequest(bool isRequested, bool isRequired = false) noexcept;
bool IsRuntimeTextureArrayRequested() noexcept;
bool IsRuntimeTextureArrayRequired() noexcept;
void SetRuntimeTextureArrayRequest(bool isRequested, bool isRequired = false) noexcept;
// These hooks join the existing frame/loss owner, not a second session.
bool FrameOpen() noexcept;
bool ClaimRasterFrame() noexcept;
bool PrepareRasterFrame() noexcept;
void RasterBeginFrame() noexcept;
void RasterEndFrame(bool accepted) noexcept;
bool RasterFinishFrame() noexcept;
void ReleaseRasterResources() noexcept;
enum class RasterCallbacks : foundation::uint8
{
    One = 1,
    Two = 2,
    Three = 3
};
void RasterExpect(foundation::uint32 request, RasterCallbacks callbacks) noexcept;
void RasterComplete(foundation::uint32 request, RasterStatus status) noexcept;
void RasterFail(foundation::uint32 request, RasterStatus status) noexcept;
void ResetLifetimeRecords() noexcept;
} // namespace ludus::graphics::rhi::internal
namespace ludus::graphics::rhi::backend
{
RasterCapabilities RasterLimits() noexcept;
BindingCapabilities BindingLimits() noexcept;
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage& outUsage) noexcept;
foundation::uint32 RasterColorAttachmentLimit() noexcept;
ludus::foundation::uint32 RasterVertexStrideLimit() noexcept;
FormatCapabilities FormatLimits(RasterFormat) noexcept;
ComputeCapabilities ComputeLimits() noexcept;
RasterStatus
ComputeCreatePipeline(foundation::usize, const internal::ComputePipelineInfo&, foundation::uint32) noexcept;
RasterStatus ComputeEncode(const internal::ComputePacket&) noexcept;
void ComputeBufferBarrier(foundation::usize, GraphAccessMode) noexcept;
RasterStatus RasterGrow(internal::RasterKind, foundation::usize) noexcept;
RasterStatus RasterPrepareStorage() noexcept;
RasterStatus RasterCreateBuffer(foundation::usize,
                                const BufferDescription&,
                                std::span<const foundation::uint8>,
                                foundation::uint32) noexcept;
RasterStatus
RasterCreateTexture(foundation::usize, const TextureDescription&, const TextureUpload&, foundation::uint32) noexcept;
RasterStatus RasterCreateView(foundation::usize, const internal::RasterViewSource&, foundation::uint32) noexcept;
RasterStatus RasterCreateSampler(foundation::usize, const SamplerDescription&, foundation::uint32) noexcept;
RasterStatus RasterCreateShader(foundation::usize,
                                const ShaderDescription&,
                                const internal::RasterShaderInfo&,
                                foundation::uint32) noexcept;
RasterStatus RasterCreateLayout(foundation::usize, const internal::RasterLayout&, foundation::uint32) noexcept;
RasterStatus RasterCreateSet(foundation::usize,
                             const internal::RasterLayout&,
                             const internal::RasterSet&,
                             foundation::uint32) noexcept;
RasterStatus RasterCreatePipeline(foundation::usize, const internal::RasterPipelineInfo&, foundation::uint32) noexcept;
void RasterDestroy(internal::RasterKind, foundation::usize) noexcept;
RasterStatus RasterDraw(const internal::RasterPacket&) noexcept;
RasterStatus RasterPreparePass(const internal::RasterPassInfo&) noexcept;
RasterStatus RasterBeginPass(const internal::RasterPassInfo&) noexcept;
void RasterEndPass() noexcept;
void RasterTextureBarrier(foundation::usize, RasterTextureUse) noexcept;
// Reserve mandatory completion bookkeeping before the first draw; submission
// cannot discover that its callback storage is exhausted after GPU work starts.
RasterStatus RasterReserveSubmission() noexcept;
void RasterDiscardSubmission() noexcept;
void RasterSubmit(foundation::uint64 ordinal) noexcept;
foundation::uint64 RasterCompleted() noexcept;
void RasterReset() noexcept;
void RasterShutdown() noexcept;
// Logical records can reset while an old browser callback still owns its cell.
bool LifetimeTransferAvailable(bool, foundation::usize) noexcept;
RasterStatus LifetimeUpload(foundation::usize,
                            const BufferDescription&,
                            const internal::LifetimeUploadTarget&,
                            const foundation::uint8*,
                            foundation::usize) noexcept;
RasterStatus LifetimeReadback(foundation::usize, BufferRole, const internal::LifetimeCopyRange&) noexcept;
RasterStatus LifetimeBufferUpdate(foundation::usize,
                                  BufferRole,
                                  const internal::LifetimeCopyRange&,
                                  const foundation::uint8*) noexcept;
RasterStatus
LifetimeTextureTransfer(foundation::usize, const internal::TextureTransfer&, const foundation::uint8*) noexcept;
RasterStatus LifetimePollTransfer(bool, foundation::usize) noexcept;
RasterStatus LifetimeCopyReadback(foundation::usize, foundation::uint8*, foundation::usize) noexcept;
void LifetimeReleaseTransfer(bool, foundation::usize) noexcept;
void LifetimeReset() noexcept;
} // namespace ludus::graphics::rhi::backend
