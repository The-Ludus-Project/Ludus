#include "internal/gpu_scope_backend.h"
#include "internal/raster.h"

// Direct3D 12 measurement and scope operations deferred to D5b.
namespace ludus::graphics::rhi::backend
{
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
