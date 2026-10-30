#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

namespace ludus::graphics::renderer::internal
{
/// One convex planar piece of a merged constant-emission region, in mesh space.
struct RasterEmitterPolygon final
{
    /// Bitwise copies of source positions, counterclockwise about NormalMesh; a valid prefix of CornerCount.
    foundation::StaticArray<foundation::math::Vector3, MAX_MESH_LIGHT_POLYGON_CORNERS> PositionsMesh{};
    foundation::usize CornerCount = 0;      ///< 3 through MAX_MESH_LIGHT_POLYGON_CORNERS; no three corners collinear.
    foundation::math::Vector3 NormalMesh{}; ///< Unit normal along the source triangles' winding cross products.
    foundation::uint32 FirstTriangle = 0;   ///< Smallest source triangle index of the region the piece belongs to.
};

/// Convex emitter pieces covering whole merged regions; unmerged triangles are left to the caller.
struct RasterEmitterPartition final
{
    foundation::Array<RasterEmitterPolygon> Polygons; ///< Grouped by region, ordered by FirstTriangle.
    foundation::Array<bool> IsMerged;                 ///< Per input triangle: whether a polygon covers it.
};

/// Merge coplanar, edge-adjacent triangles of one constant-emission mesh into convex polygons.
///
/// Triangles join across an edge whose welded endpoint positions they traverse in opposite directions, when their
/// unit normals agree within about 0.26 degrees. A joined region is replaced only when it is a topological disk
/// whose vertices lie on one plane and whose boundary, after removing collinear vertices, is a strictly convex loop
/// enclosing the region's area within a relative 1e-5. Such a region becomes one polygon, or a fan from its first
/// corner of polygons with at most MAX_MESH_LIGHT_POLYGON_CORNERS corners; any other triangle stays unmerged. Because
/// projected solid angle and LTC integrals are additive over a partition and horizon clipping distributes over it,
/// replacing the region changes the receiver integral only through these geometric tolerances (Heitz et al. 2016,
/// section 3).
/// @param positionsMesh Mesh positions, indexed by indices; borrowed for this call.
/// @param indices Triangle list; borrowed for this call.
/// @param triangles Source triangle indices to consider, ascending, each with finite in-range corners.
/// @param outPartition Written only on success; IsMerged has one entry per element of triangles.
/// @return False, leaving outPartition unchanged, when scratch or output storage cannot be allocated.
[[nodiscard]] bool TryPartitionRasterEmitterRegions(const foundation::math::Vector3* positionsMesh,
                                                    const foundation::uint32* indices,
                                                    foundation::BorrowedView<const foundation::uint32> triangles,
                                                    RasterEmitterPartition& outPartition) noexcept;
} // namespace ludus::graphics::renderer::internal
