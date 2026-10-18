#include "reference_raster.h"
#include "internal/backend.h"
#include "internal/lifecycle.h"
#include "internal/native_interop.h"
#include "internal/profile.h"
#include <cstring>
namespace ludus::graphics::rhi::reference
{
void Reset() noexcept
{
    NativeAvailable = true;
    NativePending = NativeLost = false;
    NativePublishResult = RasterStatus::Ready;
    NativePublications = NativeReleases = NativeRetirements = 0;
    ProfileAvailable = true;
    HdrAvailable = true;
    ComputeAvailable = true;
    FormatBlendUnavailable = false;
    OverrideRasterLimits = false;
    HasBindingLimitOverride = false;
    for (bool& unavailable : FormatUnavailable)
    {
        unavailable = false;
    }
    Dispatches = 0;
    ComputeResult = RasterStatus::Ready;
    Next = RasterStatus::Ready;
    Submission = RasterStatus::Ready;
    LastRequest = 0;
    PendingCount = 0;
    LossOnCompletion = 0;
    Submitted = 0;
    Completed = 0;
    Draws = Passes = Barriers = 0;
    PassStart = PassPrepare = RasterStatus::Ready;
    Discards = 0;
    Copies = 0;
    TransferStart = ReadbackCopy = RasterStatus::Ready;
    RetainTransfersOnReset = false;
    for (auto& direction : TransferOccupied)
    {
        for (auto& occupied : direction)
        {
            occupied = false;
        }
    }
    for (auto& direction : Transfers)
    {
        for (auto& status : direction)
        {
            status = RasterStatus::Pending;
        }
    }
    for (auto& pipeline : Pipelines)
    {
        pipeline = {};
    }
    for (auto& size : BufferSizes)
    {
        size = 0;
    }
    for (auto& count : Creates)
    {
        count = 0;
    }
    for (auto& count : Destroys)
    {
        count = 0;
    }
}
} // namespace ludus::graphics::rhi::reference
namespace ludus::graphics::rhi::backend
{
using namespace foundation;
namespace
{
RasterStatus Created(internal::RasterKind kind, uint32 request) noexcept
{
    ++reference::Creates[static_cast<usize>(kind)];
    reference::LastRequest = request;
    if (reference::Next == RasterStatus::Pending)
    {
        if (reference::PendingRequests.TryResize(reference::PendingCount + 1))
        {
            reference::PendingRequests[reference::PendingCount++] = request;
        }
        internal::RasterExpect(request, internal::RasterCallbacks::One);
    }
    return reference::Next;
}
} // namespace
RasterStatus RasterGrow(internal::RasterKind kind, usize count) noexcept
{
    if (kind == internal::RasterKind::Buffer)
    {
        return reference::BufferBytes.EnsureSlots(count) && reference::BufferSizes.EnsureSlots(count)
                   ? RasterStatus::Ready
                   : internal::StorageFailure();
    }
    if (kind == internal::RasterKind::Texture)
    {
        return reference::TextureBytes.EnsureSlots(count) && reference::TextureDescriptions.EnsureSlots(count)
                   ? RasterStatus::Ready
                   : internal::StorageFailure();
    }
    if (kind == internal::RasterKind::Pipeline)
    {
        return reference::Pipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
    }
    return RasterStatus::Ready;
}
RasterStatus RasterPrepareStorage() noexcept
{
    return RasterStatus::Ready;
}
ComputeCapabilities ComputeLimits() noexcept
{
    return reference::ProfileAvailable && reference::ComputeAvailable && Kind() != Backend::WebGL2
               ? ComputeCapabilities{134217728, 256, {256, 256, 64}, 256, {65535, 65535, 65535}, 8}
               : ComputeCapabilities{};
}
RasterStatus ComputeCreatePipeline(usize, const internal::ComputePipelineInfo&, uint32 request) noexcept
{
    return Created(internal::RasterKind::ComputePipeline, request);
}
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    reference::DispatchPacket = packet;
    ++reference::Dispatches;
    return reference::ComputeResult;
}
void ComputeBufferBarrier(usize, GraphAccessMode) noexcept {}
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    if (static_cast<uint8>(format) > static_cast<uint8>(RasterFormat::Bc5RgUnorm) ||
        reference::FormatUnavailable[static_cast<usize>(format)] == true)
    {
        return {};
    }
    FormatCapabilities result = internal::PortableFormatLimits(format, RasterLimits());
    result.HasUnalignedBlockExtents =
        internal::CompressedTexture(format) == true && (Kind() == Backend::Vulkan || Kind() == Backend::Metal);
    result.MaxArrayLayers = 2048;
    result.MaxCubeDimension = result.MaxSampledDimension2D;
    result.MaxDimension3D = internal::CompressedTexture(format) ? 0U : 2048U;
    result.PartialViews = result.Uses.Sampled;
    result.AttachmentViews = result.DepthSampled = result.StencilAttachment = result.StencilSampled =
        result.Uses.ColorAttachment;
    result.SampleCounts = result.Uses.ColorAttachment ? 5U : 0U;
    result.MaxAttachmentMipLevels = result.Uses.ColorAttachment ? result.MaxMipLevels : 0;
    result.MaxAttachmentLayers = result.Uses.ColorAttachment ? 2048 : 0;
    if (reference::FormatBlendUnavailable)
    {
        result.Uses.Blendable = false;
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
    return Kind() == Backend::WebGL2 ? 252U : 256U;
}
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage&) noexcept
{
    return false;
}
BindingCapabilities BindingLimits() noexcept
{
    if (reference::HasBindingLimitOverride == true)
    {
        return reference::BindingLimitOverride;
    }
    if (reference::ProfileAvailable == false)
    {
        return {};
    }
    const bool hasNativeArrays = Kind() == Backend::Vulkan || Kind() == Backend::Metal;
    return
    {
        .MaxDynamicUniformBuffers = 16,
        .MaxDynamicStorageBuffers = Kind() == Backend::WebGL2 ? 0U : 8U,
        .MaxTextureArrayElements = hasNativeArrays == true ? internal::CurrentStorageLimits().DescriptorsPerSet : 1U,
        .MaxDescriptorsPerSet = hasNativeArrays == true ? internal::CurrentStorageLimits().DescriptorsPerSet
                                                        : static_cast<uint32>(internal::RASTER_BINDINGS),
        .HasUniformTextureIndexing = hasNativeArrays,
        .HasBindlessTextureIndexing = hasNativeArrays == true && internal::IsBindlessRequested() == true,
        .MaxBindings = 16,
        .MaxUniformBuffersPerStage = 12,
        .MaxSampledTexturesPerStage =
            hasNativeArrays == true ? internal::CurrentStorageLimits().DescriptorsPerSet : 16U,
        .MaxSamplersPerStage = 16,
        .MaxUniformBuffersPerSet = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxSamplersPerSet = static_cast<uint32>(internal::RASTER_BINDINGS),
        .HasBufferDescriptorArrays = Kind() == Backend::Vulkan,
        .HasSamplerDescriptorArrays = hasNativeArrays,
        .HasRuntimeTextureArrays = Kind() == Backend::Vulkan && internal::IsRuntimeTextureArrayRequested() == true,
        .HasPartiallyBoundArrays = hasNativeArrays,
    };
}
RasterCapabilities RasterLimits() noexcept
{
    if (reference::ProfileAvailable == false)
    {
        return {};
    }
    if (reference::OverrideRasterLimits)
    {
        return reference::RasterLimitOverride;
    }
    return {internal::GetResourceSizeLimit(usize{512} * 1024 * 1024),
            4096,
            256,
            16384,
            internal::CurrentStorageLimits().ResourcesPerKind,
            internal::CurrentStorageLimits().DrawsPerFrame,
            16,
            reference::HdrAvailable,
            internal::GetResourceSizeLimit(usize{512} * 1024 * 1024)};
}
RasterStatus
RasterCreateBuffer(usize slot, const BufferDescription&, std::span<const uint8> bytes, uint32 request) noexcept
{
    if (bytes.size() > internal::LIFETIME_TRANSFER_BYTES)
    {
        return RasterStatus::OutOfMemory;
    }
    if (reference::BufferBytes[slot].TryResize(bytes.size()) == false)
    {
        return RasterStatus::OutOfMemory;
    }
    std::memcpy(reference::BufferBytes[slot].GetData(), bytes.data(), bytes.size());
    reference::BufferSizes[slot] = bytes.size();
    return Created(internal::RasterKind::Buffer, request);
}
RasterStatus
RasterCreateTexture(usize slot, const TextureDescription& info, const TextureUpload& upload, uint32 request) noexcept
{
    reference::TextureDescriptions[slot] = info;
    usize size = 0;
    uint32 width = info.Width, height = info.Height;
    for (uint32 mip = 0; mip < info.MipLevels; ++mip)
    {
        size += usize{width} * height * 4;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    if (info.Format == RasterFormat::Rgba8Unorm && info.Dimension == TextureDimension::D2 &&
        size <= internal::LIFETIME_TRANSFER_BYTES)
    {
        if (reference::TextureBytes[slot].TryResize(size) == false)
        {
            return RasterStatus::OutOfMemory;
        }
        std::memset(reference::TextureBytes[slot].GetData(), 0, size);
        if (info.Attachment == false && info.DeferredInitialization == false)
        {
            width = info.Width;
            height = info.Height;
            usize offset = 0;
            for (uint32 mip = 0; mip < info.MipLevels; ++mip)
            {
                const auto rows = upload.Mips.empty() ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[mip];
                for (uint32 row = 0; row < height; ++row)
                {
                    std::memcpy(reference::TextureBytes[slot].GetData() + offset + usize{row} * width * 4,
                                upload.Bytes.data() + rows.Offset + usize{row} * rows.RowPitch,
                                usize{width} * 4);
                }
                offset += usize{width} * height * 4;
                width = width > 1 ? width / 2 : 1;
                height = height > 1 ? height / 2 : 1;
            }
        }
    }
    return Created(internal::RasterKind::Texture, request);
}
RasterStatus RasterCreateView(usize, const internal::RasterViewSource&, uint32 request) noexcept
{
    return Created(internal::RasterKind::View, request);
}
RasterStatus RasterCreateSampler(usize, const SamplerDescription&, uint32 request) noexcept
{
    return Created(internal::RasterKind::Sampler, request);
}
RasterStatus
RasterCreateShader(usize, const ShaderDescription&, const internal::RasterShaderInfo&, uint32 request) noexcept
{
    return Created(internal::RasterKind::Shader, request);
}
RasterStatus RasterCreateLayout(usize, const internal::RasterLayout&, uint32 request) noexcept
{
    return Created(internal::RasterKind::Layout, request);
}
RasterStatus RasterCreateSet(usize, const internal::RasterLayout&, const internal::RasterSet&, uint32 request) noexcept
{
    return Created(internal::RasterKind::Set, request);
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32 request) noexcept
{
    reference::Pipelines[slot] = info;
    return Created(internal::RasterKind::Pipeline, request);
}
void RasterDestroy(internal::RasterKind kind, usize) noexcept
{
    ++reference::Destroys[static_cast<usize>(kind)];
}
RasterStatus RasterDraw(const internal::RasterPacket& packet) noexcept
{
    reference::Packet = packet;
    ++reference::Draws;
    return RasterStatus::Ready;
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo&) noexcept
{
    return reference::PassPrepare;
}
void RasterEndPass() noexcept {}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& pass) noexcept
{
    reference::Pass = pass;
    ++reference::Passes;
    return reference::PassStart;
}
void RasterTextureBarrier(usize, RasterTextureUse) noexcept
{
    ++reference::Barriers;
}
RasterStatus RasterReserveSubmission() noexcept
{
    return reference::Submission;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    reference::Submitted = ordinal;
}
uint64 RasterCompleted() noexcept
{
    const auto token = reference::LossOnCompletion;
    reference::LossOnCompletion = 0;
    if (token != 0)
    {
        internal::Fail(token, StartupError::DeviceLost);
    }
    return reference::Completed;
}
void RasterDiscardSubmission() noexcept
{
    ++reference::Discards;
}
bool LifetimeTransferAvailable(bool readback, usize slot) noexcept
{
    return reference::TransferOccupied[readback ? 1 : 0][slot] == false;
}
RasterStatus LifetimeUpload(usize transfer,
                            const BufferDescription&,
                            const internal::LifetimeUploadTarget& uploadTarget,
                            const uint8* bytes,
                            usize size) noexcept
{
    const auto buffer = uploadTarget.Buffer;
    const auto request = uploadTarget.Request;

    if (LifetimeTransferAvailable(false, transfer) == false)
    {
        return RasterStatus::CapacityExceeded;
    }
    if (reference::TransferStart != RasterStatus::Ready)
    {
        return reference::TransferStart;
    }
    if (reference::BufferBytes[buffer].TryResize(size) == false)
    {
        return RasterStatus::OutOfMemory;
    }
    std::memcpy(reference::BufferBytes[buffer].GetData(), bytes, size);
    reference::BufferSizes[buffer] = size;
    reference::Transfers[0][transfer] = RasterStatus::Pending;
    reference::TransferOccupied[0][transfer] = true;
    ++reference::Copies;
    return Created(internal::RasterKind::Buffer, request);
}
RasterStatus LifetimeReadback(usize transfer, BufferRole, const internal::LifetimeCopyRange& range) noexcept
{
    const auto buffer = range.Buffer;
    const auto offset = range.Offset;
    const auto size = range.Size;
    if (LifetimeTransferAvailable(true, transfer) == false)
    {
        return RasterStatus::CapacityExceeded;
    }
    if (reference::TransferStart != RasterStatus::Ready)
    {
        return reference::TransferStart;
    }
    const auto* bytes = reference::BufferBytes[buffer].GetData();
    std::memcpy(reference::ReadbackBytes[transfer], bytes + offset, size);
    reference::Transfers[1][transfer] = RasterStatus::Pending;
    reference::TransferOccupied[1][transfer] = true;
    ++reference::Copies;
    return RasterStatus::Ready;
}
RasterStatus
LifetimeBufferUpdate(usize slot, BufferRole, const internal::LifetimeCopyRange& range, const uint8* bytes) noexcept
{
    if (reference::TransferStart != RasterStatus::Ready)
    {
        return reference::TransferStart;
    }
    std::memcpy(reference::BufferBytes[range.Buffer].GetData() + range.Offset, bytes, range.Size);
    reference::Transfers[0][slot] = RasterStatus::Pending;
    reference::TransferOccupied[0][slot] = true;
    ++reference::Copies;
    return RasterStatus::Ready;
}
namespace
{
usize ReferenceTexel(usize slot, const TextureRegion& region, uint32 row) noexcept
{
    const auto& info = reference::TextureDescriptions[slot];
    usize offset = 0;
    uint32 width = info.Width, height = info.Height;
    for (uint32 mip = 0; mip < region.Mip; ++mip)
    {
        offset += usize{width} * height * 4;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    return offset + (usize{region.Y + row} * width + region.X) * 4;
}
} // namespace
RasterStatus LifetimeTextureTransfer(usize slot, const internal::TextureTransfer& info, const uint8* bytes) noexcept
{
    if (reference::TransferStart != RasterStatus::Ready)
    {
        return reference::TransferStart;
    }
    const bool readback = info.Operation == internal::TextureTransferOperation::Readback;
    const bool update = info.Operation == internal::TextureTransferOperation::Update;
    const auto& region = readback ? info.SourceRegion : info.DestinationRegion;
    for (uint32 row = 0; row < region.Height; ++row)
    {
        const auto* source = update ? bytes + usize{row} * info.RowPitch
                                    : reference::TextureBytes[info.Source].GetData() +
                                          ReferenceTexel(info.Source, info.SourceRegion, row);
        auto* destination = readback ? reference::ReadbackBytes[slot] + usize{row} * info.RowPitch
                                     : reference::TextureBytes[info.Destination].GetData() +
                                           ReferenceTexel(info.Destination, info.DestinationRegion, row);
        std::memcpy(destination, source, usize{region.Width} * 4);
    }
    reference::Transfers[readback ? 1 : 0][slot] = RasterStatus::Pending;
    reference::TransferOccupied[readback ? 1 : 0][slot] = true;
    ++reference::Copies;
    return RasterStatus::Ready;
}
RasterStatus LifetimePollTransfer(bool readback, usize slot) noexcept
{
    return reference::Transfers[readback ? 1 : 0][slot];
}
RasterStatus LifetimeCopyReadback(usize slot, uint8* output, usize size) noexcept
{
    if (reference::ReadbackCopy == RasterStatus::Ready)
    {
        std::memcpy(output, reference::ReadbackBytes[slot], size);
    }
    else
    {
        output[0] = 0;
    } // Deliberately dirty backend scratch on failure.
    return reference::ReadbackCopy;
}
void LifetimeReleaseTransfer(bool readback, usize slot) noexcept
{
    reference::Transfers[readback ? 1 : 0][slot] = RasterStatus::Failed;
    reference::TransferOccupied[readback ? 1 : 0][slot] = false;
}
void LifetimeReset() noexcept
{
    if (reference::RetainTransfersOnReset == false)
    {
        for (auto& direction : reference::TransferOccupied)
        {
            for (auto& occupied : direction)
            {
                occupied = false;
            }
        }
    }
}
bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
MeasurementAdapter TimingAdapter() noexcept
{
    return {};
}
RasterStatus TimingArm() noexcept
{
    if (reference::Timing.State == GpuTimingState::Armed || reference::Timing.State == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    if (reference::TimingAdmission != RasterStatus::Ready)
    {
        return reference::TimingAdmission;
    }
    reference::Timing = { .State = GpuTimingState::Armed };
    return RasterStatus::Ready;
}
GpuTiming TimingPoll() noexcept
{
    const auto result = reference::Timing;
    if (reference::TimingLossToken != 0)
    {
        const auto token = reference::TimingLossToken;
        reference::TimingLossToken = 0;
        internal::Fail(token, StartupError::DeviceLost);
    }
    return result;
}
void TimingReset() noexcept
{
    reference::Timing = {};
}
bool RasterAllocationBytes(internal::RasterKind, usize, uint64& output) noexcept
{
    output = reference::AllocationBytes;
    return reference::AllocationAvailable;
}
void RasterShutdown() noexcept {}
void RasterReset() noexcept
{
    reference::Submitted = 0;
    reference::Completed = 0;
}
} // namespace ludus::graphics::rhi::backend

namespace ludus::graphics::rhi::backend
{
bool NativeInteropSupported() noexcept
{
    return reference::NativeAvailable && (Kind() == Backend::Vulkan || Kind() == Backend::Metal);
}
RasterStatus NativeDeviceSnapshot(NativeDeviceInfo& output) noexcept
{
    if (NativeInteropSupported() == false)
    {
        return RasterStatus::Unsupported;
    }
    if (const auto* source = internal::NativeDeviceRequest())
    {
        output = { .Identity = source->Identity, .InstanceApiVersion = source->InstanceApiVersion };
    }
    else
    {
        output =
        {
            .Identity =
            {
                .Api = Kind(),
                .Instance = NativeInstance{&reference::NativeObjects[0]},
                .PhysicalDevice = NativePhysicalDevice{&reference::NativeObjects[1]},
                .Device = NativeLogicalDevice{&reference::NativeObjects[2]},
                .CommandQueue = NativeCommandQueue{&reference::NativeObjects[3]},
            },
            .InstanceApiVersion = (1U << 22) | (1U << 12),
        };
    }
    return RasterStatus::Ready;
}
RasterStatus NativeTexturePublish(const internal::NativeTexturePublication& publication,
                                  NativeTextureInfo& output) noexcept
{
    const auto slot = publication.Lease;
    const auto textureSlot = publication.Texture;
    if (reference::NativePublishResult != RasterStatus::Ready)
    {
        return reference::NativePublishResult;
    }
    if (reference::NativeReadReleased.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    reference::NativeReadReleased[slot] = false;
    output.VulkanImage = NativeVulkanImage{textureSlot + 1};
    output.VulkanLayout = 5;
    ++reference::NativePublications;
    return RasterStatus::Ready;
}
RasterStatus NativeTextureRelease(usize slot) noexcept
{
    if (reference::NativeReadReleased[slot] == false)
    {
        reference::NativeReadReleased[slot] = true;
        ++reference::NativeReleases;
    }
    return reference::NativeLost ? RasterStatus::DeviceLost : RasterStatus::Pending;
}
RasterStatus NativeTexturePoll(usize slot, bool wait) noexcept
{
    if (reference::NativeLost)
    {
        return RasterStatus::DeviceLost;
    }
    if (reference::NativeReadReleased[slot] == false)
    {
        return RasterStatus::Pending;
    }
    return wait || reference::NativePending == false ? RasterStatus::Ready : RasterStatus::Pending;
}
void NativeTextureDestroy(usize) noexcept
{
    ++reference::NativeRetirements;
}
void NativeInteropDetachSurface() noexcept {}
void NativeInteropReset() noexcept {}
} // namespace ludus::graphics::rhi::backend
