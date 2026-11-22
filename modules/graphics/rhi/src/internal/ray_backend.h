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
    /// R16b: build with refit support, or refit when the backend receives a source scene.
    bool ShouldAllowUpdate = false;
};
/// R16b compacted copy of a Ready compaction-enabled geometry slot.
struct NativeRayCompaction final
{
    foundation::usize Source = 0;
    /// CapacityExceeded when the native compacted size exceeds this budget.
    foundation::usize ByteBudget = 0;
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
/// R16b: refit the Ready update-enabled source BLAS into a new incarnation at slot. The
/// description keeps the source topology; services validated it before the call.
RasterStatus RayUpdateGeometry(foundation::usize slot,
                               foundation::usize source,
                               const RayGeometryDescription& description,
                               RayAllocation& outAllocation) noexcept;
/// R16b: refit the Ready update-enabled source TLAS into a new incarnation at slot with
/// the same instance count and per-instance geometry and opacity flags.
RasterStatus RayUpdateScene(foundation::usize slot,
                            foundation::usize source,
                            const internal::NativeRaySceneDescription& description,
                            RayAllocation& outAllocation) noexcept;
/// R16b: copy the Ready compaction-enabled source BLAS into a compacted incarnation at slot.
RasterStatus RayCompactGeometry(foundation::usize slot,
                                const internal::NativeRayCompaction& compaction,
                                RayAllocation& outAllocation) noexcept;
RasterStatus RaySubmit(foundation::usize, const internal::NativeRaySubmission&, RayAllocation&) noexcept;
RasterStatus RayPoll(internal::RasterKind, foundation::usize) noexcept;
RasterStatus RayRead(foundation::usize, RayResult*, foundation::usize) noexcept;
void RayDestroy(internal::RasterKind, foundation::usize) noexcept;
void RayDrain() noexcept;
} // namespace ludus::graphics::rhi::backend
