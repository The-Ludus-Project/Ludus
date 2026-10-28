#pragma once

// Linearly transformed cosines (LTCs) over convex polygonal lights.
//
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", ACM
// Transactions on Graphics 35(4), Article 41, 2016, sections 3-4, Equations
// (1), (3) and (4), https://doi.org/10.1145/2897824.2925895, for the
// distribution, its polygon integral and its importance sampling; and to
// Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM
// Transactions on Graphics 40(4), Article 140, 2021, section 4.1,
// https://doi.org/10.1145/3450626.3459672, for sampling the polygon in proportion
// to an LTC with projected solid angle sampling. As in Peters' implementation,
// the polygon is clipped only against the horizon of the cosine space, so a
// direction can fall below the true horizon; its BRDF is then zero.
//
// The shader port with matching numerics is shaders/linearly_transformed_cosine.slang.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math
{
using core::float32;

/// @brief A convex polygon prepared for sampling in proportion to one LTC.
///
/// Produced by TryPrepareLtcPolygon. Polygon holds the polygon transformed by the
/// inverse matrix into the space of the clamped cosine; its ProjectedSolidAngle / pi
/// is the LTC's integral over the original polygon (Heitz et al. Equation 3).
/// Callers should treat the fields as read-only.
struct LtcPolygon final
{
    ProjectedSolidAnglePolygon Polygon{}; ///< The transformed polygon, prepared for projected solid angle sampling.
    Matrix3 Transform{};                  ///< M: maps cosine-space directions to tangent space.
    Matrix3 InverseTransform{};           ///< M^-1: maps tangent-space directions to cosine space.
    float32 InverseDeterminant = 0.0F;    ///< |det(M^-1)|, the Jacobian numerator of Heitz et al. Equation (1).
};

/// @brief The unnormalized LTC density D(omega) of Heitz et al. Equation (1) for a clamped cosine.
///
/// D(omega) = max(0, w_z) |det(M^-1)| / (pi |w|^4) with w = M^-1 omega. It integrates to
/// one over the sphere for any invertible M.
/// @param inverseTransform M^-1 in the caller's tangent space.
/// @param directionTangent Unit direction.
/// @return The density per steradian; zero for a nonfinite or degenerate input or below the cosine's horizon.
[[nodiscard]] float32 EvaluateLtcDensity(const Matrix3& inverseTransform, Vector3 directionTangent) noexcept;

/// @brief Transform a convex polygon by M^-1, clip it to the cosine's horizon and prepare it for sampling.
///
/// A polygon whose transformed copy lies below the horizon prepares successfully
/// with a zero integral; Peters disables the LTC technique for it.
/// @param polygon Convex polygon in tangent space; see ProjectedPolygonInput.
/// @param inverseTransform M^-1 in the same tangent space; must be finite and invertible.
/// @param outPolygon Prepared state; written only on success.
/// @return Success; NonFiniteInput for a nonfinite vertex or matrix entry; InvalidArgument for a vertex count
/// outside [MIN_PROJECTED_POLYGON_VERTICES, MAX_PROJECTED_POLYGON_INPUT_VERTICES]; Degenerate for a singular
/// matrix. outPolygon is unchanged on failure.
[[nodiscard]] MathStatus TryPrepareLtcPolygon(const ProjectedPolygonInput& polygon,
                                              const Matrix3& inverseTransform,
                                              LtcPolygon& outPolygon) noexcept;

/// @brief The integral of the LTC over the polygon, in [0, 1] (Heitz et al. Equations 3 and 10).
[[nodiscard]] float32 GetLtcPolygonIntegral(const LtcPolygon& polygon) noexcept;

/// @brief Map two uniform random numbers to a tangent-space direction in the polygon, distributed in proportion
/// to the LTC (Peters section 4.1).
/// @param polygon Output of TryPrepareLtcPolygon.
/// @param randomNumbers Uniform random numbers, each in [0, 1].
/// @param outDirectionTangent Unit tangent-space direction; it may lie below the true horizon. Written only on
/// success.
/// @return The statuses of TrySampleProjectedSolidAnglePolygon; Degenerate when the integral is zero.
/// outDirectionTangent is unchanged on failure.
[[nodiscard]] MathStatus
TrySampleLtcPolygon(const LtcPolygon& polygon, Vector2 randomNumbers, Vector3& outDirectionTangent) noexcept;

/// @brief Solid-angle density of TrySampleLtcPolygon: D(omega) divided by the polygon integral.
///
/// The direction must lie in the polygon's solid angle, for example because another
/// technique of the same light sampled it; no containment test is performed.
/// @return Density per steradian; zero for an empty polygon, a nonfinite direction or below the cosine's horizon.
[[nodiscard]] float32 EvaluateLtcPolygonDensity(const LtcPolygon& polygon, Vector3 directionTangent) noexcept;
} // namespace ludus::foundation::math
