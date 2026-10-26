#include "internal/gpu_scope_backend.h"
#include "internal/raster.h"

#include <ludus/foundation/base/types.h>

#include <span>

// Direct3D 12 operations deferred to D4/D5. Raster resources and drawing
// live in the dedicated Direct3D 12 owners.
namespace ludus::graphics::rhi::backend
{
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

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
