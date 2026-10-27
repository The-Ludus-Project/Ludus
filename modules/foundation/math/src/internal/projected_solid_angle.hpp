#pragma once

// Private building blocks of projected solid angle sampling, exposed to the
// FoundationMath tests so they can repeat the paper's sector-level experiments
// (Peters 2021, section 3.6). Not installed.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math::internal
{
using core::float32;
using core::uint32;

/// Ellipse vector u = n_xy / n_z for the great circle through two vertices and its kind (supplement A.7,
/// Algorithm 1); a degenerate edge returns a zero vector.
[[nodiscard]] Vector2 MakeEllipseFromEdge(Vector3 vertex0, Vector3 vertex1, ProjectedEllipseKind& outKind) noexcept;

/// Area inside one nondegenerate ellipse within the sector from direction0 counterclockwise to direction1
/// (Equation 1).
[[nodiscard]] float32 ComputeEllipseAreaInSector(Vector2 ellipse, Vector2 direction0, Vector2 direction1) noexcept;

/// Area between two nondegenerate inner and outer ellipses within the sector from direction0 to direction1.
[[nodiscard]] float32 ComputeAreaBetweenEllipsesInSector(Vector2 innerEllipse,
                                                         Vector2 outerEllipse,
                                                         Vector2 direction0,
                                                         Vector2 direction1) noexcept;

/// Uniform sample of the area between two ellipses within one sector (supplement A.7, Algorithm 3).
/// randomNumbers.X is the target area divided by the sector area; targetArea is the absolute target.
[[nodiscard]] Vector2 SampleSectorBetweenEllipses(Vector2 randomNumbers,
                                                  float32 targetArea,
                                                  Vector2 innerEllipse,
                                                  Vector2 outerEllipse,
                                                  Vector2 direction0,
                                                  Vector2 direction1,
                                                  uint32 iterationCount) noexcept;

/// Clip a convex polygon to z >= 0 and return the output vertex count, zero when fewer than three remain.
/// Vertices with z == 0 are kept; a new vertex is created only where an edge strictly crosses the horizon.
[[nodiscard]] uint32 ClipPolygonToUpperHemisphere(const Vector3 (&positions)[MAX_PROJECTED_POLYGON_INPUT_VERTICES],
                                                  uint32 vertexCount,
                                                  Vector3 (&outPositions)[MAX_PROJECTED_POLYGON_VERTICES]) noexcept;
} // namespace ludus::foundation::math::internal
