#include "internal/gpu_scope_backend.h"
#include "internal/raster.h"

#include <ludus/foundation/base/types.h>

#include <span>

// Raster, compute, lifetime and pass-timing hooks for a backend that only clears and presents
// (the Direct3D 12 bring-up, docs/architecture/windows-direct3d12.md). Zero limits
// fail every portable raster/compute profile at startup, and every creation request
// reports Unsupported, so no record ever reaches submission or destruction.
namespace ludus::graphics::rhi::backend
{
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

namespace
{
/// Ordinals the facade accepted. Nothing is recorded for them, so each completes on submission.
uint64 gSubmitted = 0;
} // namespace

RasterCapabilities RasterLimits() noexcept
{
    return {};
}
BindingCapabilities BindingLimits() noexcept
{
    return {};
}
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage& outUsage) noexcept
{
    outUsage = {};
    return false;
}
uint32 RasterColorAttachmentLimit() noexcept
{
    return 0;
}
uint32 RasterVertexStrideLimit() noexcept
{
    return 0;
}
FormatCapabilities FormatLimits(RasterFormat) noexcept
{
    return {};
}
ComputeCapabilities ComputeLimits() noexcept
{
    return {};
}
RasterStatus ComputeCreatePipeline(usize, const internal::ComputePipelineInfo&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus ComputeEncode(const internal::ComputePacket&) noexcept
{
    return RasterStatus::Unsupported;
}
void ComputeBufferBarrier(usize, GraphAccessMode) noexcept {}
RasterStatus RasterGrow(internal::RasterKind, usize) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterPrepareStorage() noexcept
{
    // No storage exists to prepare; frames that only clear proceed.
    return RasterStatus::Ready;
}
RasterStatus RasterCreateBuffer(usize, const BufferDescription&, std::span<const uint8>, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterCreateTexture(usize, const TextureDescription&, const TextureUpload&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterCreateView(usize, const internal::RasterViewSource&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterCreateSampler(usize, const SamplerDescription&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterCreateShader(usize, const ShaderDescription&, const internal::RasterShaderInfo&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterCreateLayout(usize, const internal::RasterLayout&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterCreateSet(usize, const internal::RasterLayout&, const internal::RasterSet&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterCreatePipeline(usize, const internal::RasterPipelineInfo&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
void RasterDestroy(internal::RasterKind, usize) noexcept {}
RasterStatus RasterDraw(const internal::RasterPacket&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RasterBeginPass(const internal::RasterPassInfo&) noexcept
{
    return RasterStatus::Unsupported;
}
void RasterEndPass() noexcept {}
void RasterTextureBarrier(usize, RasterTextureUse) noexcept {}
RasterStatus RasterReserveSubmission() noexcept
{
    return RasterStatus::Unsupported;
}
void RasterDiscardSubmission() noexcept {}
void RasterSubmit(uint64 ordinal) noexcept
{
    gSubmitted = ordinal;
}
uint64 RasterCompleted() noexcept
{
    return gSubmitted;
}
void RasterReset() noexcept
{
    gSubmitted = 0;
}
void RasterShutdown() noexcept
{
    gSubmitted = 0;
}
bool LifetimeTransferAvailable(bool, usize) noexcept
{
    return false;
}
RasterStatus
LifetimeUpload(usize, const BufferDescription&, const internal::LifetimeUploadTarget&, const uint8*, usize) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus LifetimeReadback(usize, BufferRole, const internal::LifetimeCopyRange&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus LifetimeBufferUpdate(usize, BufferRole, const internal::LifetimeCopyRange&, const uint8*) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus LifetimeTextureTransfer(usize, const internal::TextureTransfer&, const uint8*) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus LifetimePollTransfer(bool, usize) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus LifetimeCopyReadback(usize, uint8*, usize) noexcept
{
    return RasterStatus::Unsupported;
}
void LifetimeReleaseTransfer(bool, usize) noexcept {}
void LifetimeReset() noexcept {}
RasterStatus ArmPassTimings() noexcept
{
    return RasterStatus::Unsupported;
}
GpuPassTimings PollPassTimings() noexcept
{
    return { .State = GpuTimingState::Unavailable };
}
RasterStatus BeginScope(const GpuScopeDescription&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus EndScope() noexcept
{
    return RasterStatus::Unsupported;
}
void ResetPassTimings() noexcept {}
} // namespace ludus::graphics::rhi::backend
