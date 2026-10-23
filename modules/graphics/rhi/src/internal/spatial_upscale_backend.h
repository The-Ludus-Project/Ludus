#pragma once

#include <ludus/graphics/rhi/spatial_upscale.h>

namespace ludus::graphics::rhi::backend
{
bool IsMetalFxSpatialSupported() noexcept;
RasterStatus SpatialUpscaleGrow(foundation::usize count) noexcept;
RasterStatus SpatialUpscaleCreate(foundation::usize slot, const SpatialUpscalerDescription& description) noexcept;
RasterStatus
SpatialUpscaleEncode(foundation::usize slot, foundation::usize source, foundation::usize destination) noexcept;
void SpatialUpscaleDestroy(foundation::usize slot) noexcept;
} // namespace ludus::graphics::rhi::backend
