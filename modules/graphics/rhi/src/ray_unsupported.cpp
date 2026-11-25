#include "internal/ray_backend.h"

namespace ludus::graphics::rhi::backend
{
RayCapabilities RayLimits() noexcept
{
    return {};
}
RasterStatus RayCreateProgram(foundation::usize, const ShaderDescription&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RayCreateGeometry(foundation::usize, const RayGeometryDescription&, RayAllocation&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RayCreateScene(foundation::usize, const internal::NativeRaySceneDescription&, RayAllocation&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus
RayUpdateGeometry(foundation::usize, foundation::usize, const RayGeometryDescription&, RayAllocation&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RayUpdateScene(foundation::usize,
                            foundation::usize,
                            const internal::NativeRaySceneDescription&,
                            RayAllocation&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RayCompactGeometry(foundation::usize, const internal::NativeRayCompaction&, RayAllocation&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RaySubmit(foundation::usize, const internal::NativeRaySubmission&, RayAllocation&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RayPoll(internal::RasterKind, foundation::usize) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus RayRead(foundation::usize, RayResult*, foundation::usize) noexcept
{
    return RasterStatus::Unsupported;
}
void RayDestroy(internal::RasterKind, foundation::usize) noexcept {}
void RayDrain() noexcept {}
} // namespace ludus::graphics::rhi::backend
