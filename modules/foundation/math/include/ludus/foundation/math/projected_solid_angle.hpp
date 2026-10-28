#pragma once

// Projected solid angle sampling of convex polygonal lights.
//
// Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights",
// ACM Transactions on Graphics 40(4), Article 140, 2021, section 3 and
// supplement sections A.1-A.7, https://doi.org/10.1145/3450626.3459672.
// The sector partition, ellipse representation, two-quad initialization, the
// two-iteration tangent-line solver and the bitonic sorting networks are ported
// from the author's BSD-licensed polygon_sampling.glsl; the notice is retained in
// src/projected_solid_angle.cpp. Horizon clipping is an independent
// Sutherland-Hodgman implementation of supplement A.1's result, not its generated
// GPL code. Departures are listed on the FoundationMath module page
// (docs/modules/foundation/math.md, "Projected solid angle sampling").
//
// The shader port with matching numerics is shaders/projected_solid_angle.slang.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math
{
using core::float32;
using core::uint32;

/// @brief Fewest vertices of a polygon, before and after clipping.
inline constexpr uint32 MIN_PROJECTED_POLYGON_VERTICES = 3;
/// @brief Largest convex polygon accepted before horizon clipping (paper supplement A.1).
inline constexpr uint32 MAX_PROJECTED_POLYGON_INPUT_VERTICES = 7;
/// @brief Largest polygon after clipping to z >= 0; clipping adds at most one vertex (supplement A.6).
inline constexpr uint32 MAX_PROJECTED_POLYGON_VERTICES = MAX_PROJECTED_POLYGON_INPUT_VERTICES + 1;
/// @brief Tangent-line iterations per decentral sample; two give backward errors near 1e-5 (paper section 3.6).
inline constexpr uint32 PROJECTED_POLYGON_ITERATION_COUNT = 2;

/// @brief A convex planar polygon in the tangent space of a shading point.
///
/// The shading point is the origin and the shading normal is +z, the frame the paper
/// shares with linearly transformed cosines (section 3.1). Coordinates need not be
/// normalized. The polygon must be convex and planar with no three
/// collinear vertices (paper section 3.1 and supplement A.2); these
/// preconditions are not detected. Either winding is accepted: a polygon seen
/// counterclockwise from the origin is mirrored internally (paper section 3.1).
struct ProjectedPolygonInput final
{
    /// Valid prefix of VertexCount positions relative to the shading point.
    Vector3 PositionsTangent[MAX_PROJECTED_POLYGON_INPUT_VERTICES]{};
    /// Number of vertices, MIN_PROJECTED_POLYGON_VERTICES through MAX_PROJECTED_POLYGON_INPUT_VERTICES.
    uint32 VertexCount = 0;
};

/// @brief How the great circle of a polygon edge bounds the projected polygon (paper section 3.7, supplement A.2).
///
/// The paper encodes inner ellipses in the sign bit of u.x and degenerate ones as an
/// infinite u.x. Ludus stores the kind explicitly because its Metal and WGSL shader
/// targets do not guarantee signed zeros or infinities.
enum class ProjectedEllipseKind : core::uint8
{
    Outer = 0,      ///< The edge runs counterclockwise around the zenith; its normal has a positive z.
    Inner = 1,      ///< The edge runs clockwise around the zenith; its normal has a negative z.
    Degenerate = 2, ///< The great circle passes through the zenith; it encloses no area and counts as outer.
};

/// @brief Per-polygon, per-shading-point state for projected solid angle sampling.
///
/// Produced by TryPrepareProjectedSolidAnglePolygon and consumed unchanged by the
/// sampling and density functions. The fields record the paper's representation
/// (supplement A.7, Algorithm 4); callers should treat them as read-only.
struct ProjectedSolidAnglePolygon final
{
    /// Clipped tangent-space vertex xy coordinates, y negated when IsMirrored; counterclockwise around the zenith
    /// in the decentral case.
    Vector2 VerticesXy[MAX_PROJECTED_POLYGON_VERTICES]{};
    /// Ellipse vector u = n_xy / n_z for the edge following each vertex counterclockwise; zero when degenerate.
    Vector2 Ellipses[MAX_PROJECTED_POLYGON_VERTICES]{};
    /// Kind of each entry of Ellipses.
    ProjectedEllipseKind EllipseKinds[MAX_PROJECTED_POLYGON_VERTICES]{};
    /// Inner ellipse adjacent to vertex 0 in the decentral case.
    Vector2 InnerEllipse0{};
    /// Whether the zenith lies inside the polygon, so every sector is bounded by one outer ellipse (paper Fig. 3a).
    bool IsCentralCase = true;
    /// Projected solid angle of the polygon within the sector starting at each vertex, in steradians.
    float32 SectorProjectedSolidAngles[MAX_PROJECTED_POLYGON_VERTICES]{};
    /// Total projected solid angle in steradians, in [0, pi]; zero means nothing can be sampled.
    float32 ProjectedSolidAngle = 0.0F;
    /// Clipped vertex count, zero when no part of the polygon lies above the horizon.
    uint32 VertexCount = 0;
    /// Whether y was negated so the polygon winds clockwise as seen from the origin.
    bool IsMirrored = false;
};

/// @brief Clip a convex polygon to z >= 0 and prepare it for projected solid angle sampling.
///
/// A polygon entirely below the horizon, or one whose plane contains the shading
/// point, succeeds with ProjectedSolidAngle == 0 so a caller can skip the light.
/// @param polygon Convex polygon in tangent space; see ProjectedPolygonInput.
/// @param outPolygon Prepared state; written only on success.
/// @return Success, NonFiniteInput for a nonfinite coordinate, or InvalidArgument for a vertex count
/// outside [MIN_PROJECTED_POLYGON_VERTICES, MAX_PROJECTED_POLYGON_INPUT_VERTICES]. outPolygon is unchanged on
/// failure.
[[nodiscard]] MathStatus TryPrepareProjectedSolidAnglePolygon(const ProjectedPolygonInput& polygon,
                                                              ProjectedSolidAnglePolygon& outPolygon) noexcept;

/// @brief Map two uniform random numbers to a direction uniformly distributed in projected solid angle.
///
/// The mapping is continuous in both random numbers, so stratification carries
/// over (paper section 3.2 and Fig. 13). Decentral samples run
/// PROJECTED_POLYGON_ITERATION_COUNT tangent-line iterations; single-precision
/// rounding dominates their error (paper section 3.6 and Fig. 6).
/// @param polygon Output of TryPrepareProjectedSolidAnglePolygon.
/// @param randomNumbers Uniform random numbers, each in [0, 1].
/// @param outDirectionTangent Unit tangent-space direction with z >= 0; written only on success.
/// @return Success; NonFiniteInput or InvalidArgument for random numbers outside [0, 1]; Degenerate when
/// ProjectedSolidAngle is zero; IllConditioned when single precision cannot represent the sample, which the
/// paper observes only for extreme tiny, distant polygons (section 3.7, Fig. 7b). outDirectionTangent is
/// unchanged on failure.
[[nodiscard]] MathStatus TrySampleProjectedSolidAnglePolygon(const ProjectedSolidAnglePolygon& polygon,
                                                             Vector2 randomNumbers,
                                                             Vector3& outDirectionTangent) noexcept;

/// @brief Solid-angle density of TrySampleProjectedSolidAnglePolygon for one direction.
///
/// The density is cos(theta) / ProjectedSolidAngle. The direction must lie in the
/// polygon's solid angle: it was sampled from this polygon (for example by another
/// technique of the same light) or a ray in this direction hits the polygon. No
/// containment test is performed.
/// @param polygon Output of TryPrepareProjectedSolidAnglePolygon.
/// @param directionTangent Unit tangent-space direction.
/// @return Density per steradian; zero below the horizon, for a nonfinite direction or an empty polygon.
[[nodiscard]] float32 EvaluateProjectedSolidAnglePolygonDensity(const ProjectedSolidAnglePolygon& polygon,
                                                                Vector3 directionTangent) noexcept;
} // namespace ludus::foundation::math
