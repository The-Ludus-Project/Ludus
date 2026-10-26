#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/ray.h>

#include "raster.h"

namespace ludus::graphics::rhi::internal
{
struct NativeRayInstance final
{
    foundation::usize Geometry = 0;
    foundation::float32 Transform[12]{};
    foundation::uint32 Mask = 255;
    bool IsOpaque = true;
};
struct NativeRaySceneDescription final
{
    const NativeRayInstance* Instances = nullptr;
    foundation::usize Count = 0;
    const foundation::float32* Opacity = nullptr;
    foundation::usize ScalarCount = 0, ByteBudget = 0;
};
struct NativeRaySubmission final
{
    foundation::usize Program = 0, Scene = 0;
    const RayInput* Rays = nullptr;
    foundation::usize Count = 0;
};
} // namespace ludus::graphics::rhi::internal
namespace ludus::graphics::rhi::backend
{
RayCapabilities RayLimits() noexcept;
RasterStatus RayCreateProgram(foundation::usize, const ShaderDescription&) noexcept;
RasterStatus RayCreateGeometry(foundation::usize, const RayGeometryDescription&, RayAllocation&) noexcept;
RasterStatus RayCreateScene(foundation::usize, const internal::NativeRaySceneDescription&, RayAllocation&) noexcept;
RasterStatus RaySubmit(foundation::usize, const internal::NativeRaySubmission&, RayAllocation&) noexcept;
RasterStatus RayPoll(internal::RasterKind, foundation::usize) noexcept;
RasterStatus RayRead(foundation::usize, RayResult*, foundation::usize) noexcept;
void RayDestroy(internal::RasterKind, foundation::usize) noexcept;
void RayDrain() noexcept;
} // namespace ludus::graphics::rhi::backend
