#pragma once

// Convex planar polygons from triangle meshes, the light representation for
// projected solid angle sampling.
//
// Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM
// Transactions on Graphics 40(4), Article 140, 2021, section 3.1 and supplement
// A.2 (https://doi.org/10.1145/3450626.3459672), samples convex planar polygons
// with no three collinear vertices and asks that non-convex polygons be split
// into convex ones. This partition merges the triangles of a mesh into such
// polygons, adapting the removal of inessential diagonals from Stefan Hertel and
// Kurt Mehlhorn, "Fast triangulation of simple polygons", Foundations of
// Computation Theory, LNCS 158, 1983, pp. 207-218: a shared edge between two
// pieces is removed while the union stays convex. The greedy order is a Ludus
// choice; the partition is not minimal.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry
{
/// @brief Largest polygon in a partition; owned by projected solid angle sampling (paper supplement A.1).
inline constexpr usize MAX_CONVEX_POLYGON_VERTICES = math::MAX_PROJECTED_POLYGON_INPUT_VERTICES;

/// @brief One convex planar polygon made of whole source triangles.
///
/// Positions are bitwise copies of source vertex positions, ordered by the source
/// winding: counterclockwise when viewed against NormalMesh. Consecutive edges
/// turn by more than the collinearity tolerance, so no three vertices are collinear.
/// An affine transform keeps the polygon convex and planar; transform the
/// positions, then derive a world normal from them.
struct ConvexPolygon final
{
    StaticArray<math::Vector3, MAX_CONVEX_POLYGON_VERTICES> PositionsMesh{}; ///< Valid prefix of VertexCount.
    usize VertexCount = 0;                                                   ///< 3 through MAX_CONVEX_POLYGON_VERTICES.
    math::Vector3 NormalMesh{}; ///< Unit normal from the winding, in the positions' space.
    usize FirstTriangle = 0;    ///< First entry of this polygon in ConvexPolygonPartition::Triangles.
    usize TriangleCount = 0;    ///< Number of source triangles covered by this polygon.
};

/// @brief Convex polygons covering every nondegenerate triangle of a mesh exactly once.
struct ConvexPolygonPartition final
{
    Array<ConvexPolygon> Polygons; ///< Ordered by their smallest source triangle index.
    Array<uint32> Triangles;       ///< Source triangle indices grouped by polygon, ascending within each group.
    /// Triangles with coincident or collinear corners. They enclose no measurable area, belong to no polygon and
    /// are listed nowhere.
    usize DegenerateTriangleCount = 0;
    /// Edges shared by more than two triangles or by two triangles of opposite winding; polygons never span them.
    usize UnmergeableEdgeCount = 0;
};

/// @brief Partition a triangle mesh into convex planar polygons for polygonal light sampling.
///
/// Triangles that share an edge (equal endpoint positions, opposite directions)
/// merge when the result is coplanar, convex, simple and has at most
/// MAX_CONVEX_POLYGON_VERTICES vertices after collinear vertices are removed.
/// Merging along a chain of shared edges removes the chain's interior vertices, so
/// a fan around a center vertex becomes one polygon when it fits. Every
/// nondegenerate triangle ends in exactly one polygon; a lone triangle is a polygon
/// of three vertices. Tolerances are relative to the polygon's extent and the
/// float32 coordinate magnitude. The result is deterministic for a given input.
/// @param positionsMesh Finite vertex positions in one Cartesian space; borrowed for this call.
/// @param indices Triangle list; each index must name a position.
/// @param outPartition Written only on success.
/// @return Success; InvalidArgument for invalid views or an index count that is not a multiple of three;
/// InvalidTopology for an index outside positionsMesh; NonFiniteInput for a nonfinite referenced position;
/// OutOfMemory when scratch or output storage cannot be allocated. outPartition is unchanged on failure.
[[nodiscard]] GeometryStatus TryPartitionConvexPolygons(BorrowedView<const math::Vector3> positionsMesh,
                                                        BorrowedView<const uint32> indices,
                                                        ConvexPolygonPartition& outPartition) noexcept;
} // namespace ludus::foundation::geometry
