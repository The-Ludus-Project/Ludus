#include "internal/spatial_upscale_backend.h"

namespace ludus::graphics::rhi::backend
{
bool IsMetalFxSpatialSupported() noexcept
{
    return false;
}
RasterStatus SpatialUpscaleGrow(foundation::usize) noexcept
{
    return RasterStatus::Ready;
}
RasterStatus SpatialUpscaleCreate(foundation::usize, const SpatialUpscalerDescription&) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus SpatialUpscaleEncode(foundation::usize, foundation::usize, foundation::usize) noexcept
{
    return RasterStatus::Unsupported;
}
void SpatialUpscaleDestroy(foundation::usize) noexcept {}
} // namespace ludus::graphics::rhi::backend
