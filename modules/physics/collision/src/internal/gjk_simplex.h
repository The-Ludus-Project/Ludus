#pragma once

// The GJK simplex of the general convex cast and its closest-point reduction.
//
// Thanks to Christer Ericson, "Real-Time Collision Detection", Morgan Kaufmann,
// 2005, sections 5.1.2 (closest point on a segment), 5.1.5 (on a triangle, by
// Voronoi regions) and 5.1.6 (on a tetrahedron), for the closest-point
// routines that reduce the simplex. Ludus evaluates them for the origin,
// returns barycentric weights and handles degenerate simplices explicitly.

#include "mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::physics::collision::internal
{
inline constexpr usize MAX_SIMPLEX_VERTICES = 4;

/// One support point of the Minkowski difference C = T - A, with the triangle point it came from.
struct SimplexVertex final
{
    math::Vector3d DifferenceMesh{}; ///< `TargetMesh - shape support`, relative to the shape position.
    math::Vector3d TargetMesh{};     ///< The triangle's support point, relative to the shape position.
};

/// At most four affinely independent support points; Count of them are in use.
struct Simplex final
{
    foundation::StaticArray<SimplexVertex, MAX_SIMPLEX_VERTICES> Vertices{};
    usize Count = 0;
};

/// The point of a simplex closest to the origin.
struct SimplexClosest final
{
    math::Vector3d PointMesh{};     ///< Closest point of conv{x - p_i} to the origin.
    math::Vector3d WitnessMesh{};   ///< Matching convex combination of the vertices' triangle points.
    bool HasEnclosedOrigin = false; ///< The tetrahedron contains the origin; PointMesh is zero.
};

/// @brief Reduce `inoutSimplex` to the smallest subset that keeps the point of conv{rayPoint - p_i} closest to the
/// origin, and report that point.
///
/// Vertices with zero barycentric weight are removed; their relative order is kept so the reduction is
/// deterministic. A degenerate segment, triangle or tetrahedron falls back to its lower-dimensional faces.
/// @pre inoutSimplex.Count is in [1, MAX_SIMPLEX_VERTICES].
void ReduceToClosest(math::Vector3d rayPointMesh, Simplex& inoutSimplex, SimplexClosest& outClosest) noexcept;
} // namespace ludus::physics::collision::internal
