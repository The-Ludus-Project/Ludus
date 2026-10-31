#pragma once

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
/// Radiance below which an emitter's bounded Lambert contribution is omitted from a grid cell: 2^-12, under one
/// 8-bit sRGB step at unit exposure even next to black, where a step is 1 / (255 * 12.92), about 3.0e-4.
inline constexpr foundation::float32 MESH_LIGHT_CULL_RADIANCE = 1.0F / 4096.0F;
/// Append the world-space culling grid of the emitter polygon records that start inoutWords (deferred_layout.h,
/// "Mesh-light culling grid") and report its first word in outGridWord. Each record is listed in every cell that
/// its influence sphere reaches and that is not wholly behind a one-sided emitter's plane. The sphere bounds the
/// record's polygon plus the distance beyond which its unshadowed Lambert term, at most
/// emission * area / (pi * distance^2), stays below MESH_LIGHT_CULL_RADIANCE.
/// @param receiversWorld Bounds of every surface the resolve shades; positions outside the grid evaluate every record.
/// @return Ready with outGridWord zero when there is nothing to cull (no records or empty bounds) or the lists would
/// exceed MAX_MESH_LIGHT_GRID_ASSIGNMENTS; the resolve then evaluates every record as before. OutOfMemory or
/// InvalidDescription (malformed records) preserve both outputs.
[[nodiscard]] rhi::RasterStatus AppendRasterMeshLightGrid(foundation::Array<foundation::math::Vector4>& inoutWords,
                                                          foundation::uint32 recordCount,
                                                          const foundation::math::Aabb3& receiversWorld,
                                                          foundation::uint32& outGridWord) noexcept;
/// Records listed for the cell containing positionWorld, as word offsets of their headers in words, appended to
/// outRecordWords; false when the position lies outside the grid at gridWord (the resolve then evaluates every
/// record) or the grid is malformed. Mirrors the resolve's lookup for CPU checks.
[[nodiscard]] bool GetRasterMeshLightCell(foundation::BorrowedView<const foundation::math::Vector4> words,
                                          foundation::uint32 gridWord,
                                          foundation::math::Vector3 positionWorld,
                                          foundation::Array<foundation::uint32>& outRecordWords) noexcept;
} // namespace ludus::graphics::renderer::internal
