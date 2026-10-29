#pragma once

// Linearly transformed cosines (LTCs) integrated over and sampled within convex
// polygons, built on projected solid angle sampling.
//
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", ACM
// Transactions on Graphics 35(4), Article 41, 2016 (SIGGRAPH 2016),
// https://doi.org/10.1145/2897824.2925895: the LTC density (section 3.1,
// Equation 1, with the clamped cosine of Equation 4), its scale invariance
// (section 3.1), and the polygon integral of Equation 3, which equals the
// clamped-cosine integral over the polygon transformed by M^-1.
// Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights",
// ACM Transactions on Graphics 40(4), Article 140, 2021, section 4.1,
// https://doi.org/10.1145/3450626.3459672: sample an LTC within a polygon by
// clipping M^-1 P to z >= 0 only, sampling it in projected solid angle and
// mapping the sample back with M; an empty clip disables the technique.
// This is an independent implementation from the papers' text; no code from
// either paper's GPL-licensed utilities is used. Departures are listed on the
// FoundationMath module page (docs/modules/foundation/math.md, "Linearly
// transformed cosines over polygons").
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

/// @brief Smallest accepted determinant of the scaled inverse transform (2^-32).
///
/// TryMakeLinearlyTransformedCosine scales M^-1 so its largest-magnitude entry lies in [1, 2). A smaller determinant
/// means the distribution is nearly a delta or a great circle, and its density would leave single precision.
inline constexpr float32 MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT = 2.3283064365386963e-10F;

/// @brief A linearly transformed cosine distribution (Heitz et al. 2016, Equation 1; Peters 2021, section 4.1).
///
/// The density at a unit direction w is p(w) = max(0, (M^-1 w).z) * |M^-1| / (pi * |M^-1 w|^4): the clamped cosine
/// (1/pi) max(0, z) of the normalized direction M^-1 w / |M^-1 w|, times the Jacobian |M^-1| / |M^-1 w|^3. It
/// integrates to one over the sphere. The distribution is invariant to a positive scale of M, so both matrices are
/// stored scaled: InverseTransform by a power of two, and Transform as the adjugate of InverseTransform, a positive
/// multiple of M. Produced by TryMakeLinearlyTransformedCosine; treat the fields as read-only.
struct LinearlyTransformedCosine final
{
    /// M^-1 scaled by a power of two so that its largest-magnitude entry lies in [1, 2).
    Matrix3 InverseTransform = Matrix3::Identity();
    /// The adjugate of InverseTransform, |M^-1| times the inverse of InverseTransform; maps cosine directions to w.
    Matrix3 Transform = Matrix3::Identity();
    /// Determinant of InverseTransform, at least MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT.
    float32 InverseDeterminant = 1.0F;
};

/// @brief An LTC restricted to a convex polygon, prepared for integration and sampling at one shading point.
///
/// Produced by TryPrepareLinearlyTransformedCosinePolygon; treat the fields as read-only.
struct LinearlyTransformedCosinePolygon final
{
    /// The polygon transformed by M^-1, clipped to z >= 0 and prepared for projected solid angle sampling.
    /// Its ProjectedSolidAngle is pi times the LTC's integral over the polygon (Heitz et al. 2016, Equation 3); zero
    /// means the clip is empty and the technique must be disabled (Peters 2021, section 4.1).
    ProjectedSolidAnglePolygon TransformedPolygon{};
    /// The distribution the polygon was prepared with.
    LinearlyTransformedCosine Distribution{};
};

/// @brief Make an LTC from its inverse transform M^-1, the matrix that maps the LTC back to a clamped cosine.
///
/// The matrix acts on column vectors in the tangent frame of ProjectedPolygonInput: the shading point at the origin
/// and the shading normal along +z. Tabulated isotropic BRDF fits are usually stated in a frame whose x-z plane
/// contains the view direction; the caller composes that rotation into M^-1. Only the direction of M^-1 w matters,
/// so any positive multiple of M^-1 describes the same distribution.
/// @param inverseTransform M^-1, with a positive determinant (Peters 2021, section 4.1, requires |M| > 0).
/// @param outDistribution The scaled representation; written only on success.
/// @return Success; NonFiniteInput for a nonfinite entry; Degenerate for a zero matrix, a largest-magnitude entry of
/// at least 2^127 or a determinant that is not positive (a singular matrix or one that mirrors the hemisphere);
/// IllConditioned when the scaled determinant is below MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT. outDistribution is
/// unchanged on failure.
[[nodiscard]] MathStatus TryMakeLinearlyTransformedCosine(const Matrix3& inverseTransform,
                                                          LinearlyTransformedCosine& outDistribution) noexcept;

/// @brief Density of an LTC at one direction, per steradian over the whole sphere.
/// @param distribution Output of TryMakeLinearlyTransformedCosine.
/// @param directionTangent Unit tangent-space direction.
/// @return p(w) as defined on LinearlyTransformedCosine; zero where (M^-1 w).z <= 0, for a nonfinite or zero
/// direction, or where single precision cannot represent the density.
[[nodiscard]] float32 EvaluateLinearlyTransformedCosine(const LinearlyTransformedCosine& distribution,
                                                        Vector3 directionTangent) noexcept;

/// @brief Transform a convex polygon by M^-1, clip it to z >= 0 and prepare it for integration and sampling.
///
/// The original polygon is not clipped to its own horizon (Peters 2021, section 4.1): the LTC may be positive below
/// the shading horizon and zero in parts above it, so a sample can point below the horizon, where the caller's BRDF
/// evaluates to zero. A transformed polygon below z = 0, or one whose plane contains the shading point, succeeds with
/// an empty TransformedPolygon.
/// @param polygon Convex polygon in tangent space; see ProjectedPolygonInput.
/// @param distribution Output of TryMakeLinearlyTransformedCosine.
/// @param outPolygon Prepared state; written only on success.
/// @return The status of TryPrepareProjectedSolidAnglePolygon for the transformed vertices: Success,
/// NonFiniteInput for a nonfinite coordinate (before or after the transform) or InvalidArgument for a vertex count
/// outside [MIN_PROJECTED_POLYGON_VERTICES, MAX_PROJECTED_POLYGON_INPUT_VERTICES]. outPolygon is unchanged on
/// failure.
[[nodiscard]] MathStatus
TryPrepareLinearlyTransformedCosinePolygon(const ProjectedPolygonInput& polygon,
                                           const LinearlyTransformedCosine& distribution,
                                           LinearlyTransformedCosinePolygon& outPolygon) noexcept;

/// @brief The LTC's integral over the polygon, in [0, 1] (Heitz et al. 2016, Equations 3 and 10).
///
/// It equals the projected solid angle of the transformed, clipped polygon divided by pi. Heitz et al. multiply it by
/// the BRDF's norm to approximate the polygon's unshadowed shading.
/// @param polygon Output of TryPrepareLinearlyTransformedCosinePolygon.
/// @return The integral; zero when the clip is empty.
[[nodiscard]] float32
GetLinearlyTransformedCosinePolygonIntegral(const LinearlyTransformedCosinePolygon& polygon) noexcept;

/// @brief Hill and Heitz's approximation of a polygon's horizon-clipped cosine integral, without clipping.
///
/// Thanks to Stephen Hill and Eric Heitz, "Real-Time Area Lighting: a Journey from Research to Production",
/// SIGGRAPH 2016 Advances in Real-Time Rendering course, https://advances.realtimerendering.com/s2016/: the vector form
/// factor F of the unclipped polygon, whose norm is the form factor in the direction of F (slides 90-93), and the
/// clipped form factor of a proxy sphere with the same F, max((|F|^2 + F.z) / (|F| + 1), 0) (slides 94-97 and 102).
/// Edges use their quadratic fit of theta / sin(theta) (slide 77, at most 0.73% from the exact ratio). Departure: a
/// polygon entirely above the horizon uses F.z, the unclipped form factor, which the course's tabulated variant also
/// reproduces there, and a polygon with no vertex above the horizon has a zero integral.
struct ApproximatePolygonIntegral final
{
    /// F: the polygon's vector form factor, oriented towards the polygon; zero when the polygon's plane contains the
    /// origin.
    Vector3 VectorFormFactor{};
    /// The approximate clipped integral in [0, 1]. Zero when no vertex lies above the horizon. It may also be zero for
    /// a polygon that crosses the horizon, where the proxy sphere lies below it.
    float32 Integral = 0.0F;
    /// |d0 . (d1 x d2)| for the first three unit vertex directions d_i. It is positive exactly when the integral is
    /// computed, so a caller can add a positive defensive term wherever the polygon has area above the horizon.
    float32 UnitTripleProduct = 0.0F;
};

/// @brief Approximate the clamped-cosine (diffuse form factor) integral of a convex polygon clipped to z >= 0.
///
/// The cost is one normalization, one cross product and one fitted edge integral per vertex, with no clipping. A
/// polygon whose plane contains the origin, including one with a zero vertex, succeeds with every output zero.
/// @param polygon Convex polygon in tangent space; see ProjectedPolygonInput.
/// @param outIntegral The approximation; written only on success.
/// @return Success; NonFiniteInput for a nonfinite coordinate or InvalidArgument for a vertex count outside
/// [MIN_PROJECTED_POLYGON_VERTICES, MAX_PROJECTED_POLYGON_INPUT_VERTICES]. outIntegral is unchanged on failure.
[[nodiscard]] MathStatus TryApproximateClampedCosinePolygonIntegral(const ProjectedPolygonInput& polygon,
                                                                    ApproximatePolygonIntegral& outIntegral) noexcept;

/// @brief Approximate GetLinearlyTransformedCosinePolygonIntegral without clipping or preparing the polygon.
///
/// Applies TryApproximateClampedCosinePolygonIntegral to the polygon transformed by M^-1, as Hill and Heitz do for
/// their specular term (slide 63). Only the transformed polygon's horizon matters, as for
/// TryPrepareLinearlyTransformedCosinePolygon.
/// @param polygon Convex polygon in tangent space; see ProjectedPolygonInput.
/// @param distribution Output of TryMakeLinearlyTransformedCosine.
/// @param outIntegral The approximation in the LTC's cosine space; written only on success.
/// @return Success; NonFiniteInput for a nonfinite coordinate before or after the transform, or InvalidArgument for
/// an invalid vertex count. outIntegral is unchanged on failure.
[[nodiscard]] MathStatus
TryApproximateLinearlyTransformedCosinePolygonIntegral(const ProjectedPolygonInput& polygon,
                                                       const LinearlyTransformedCosine& distribution,
                                                       ApproximatePolygonIntegral& outIntegral) noexcept;

/// @brief Map two uniform random numbers to a direction in the polygon distributed proportionally to the LTC.
///
/// Samples the transformed polygon in projected solid angle, maps the sample back with M and normalizes it (Heitz et
/// al. 2016, Algorithm 1; Peters 2021, section 4.1). The mapping is continuous in both random numbers. The direction
/// may lie below the shading horizon; see TryPrepareLinearlyTransformedCosinePolygon.
/// @warning When the integral is negligible (the tests observe it below 1e-6), the transformed polygon is a sliver
/// whose single-precision projected solid angle sample can be inaccurate, as the paper reports for tiny, distant
/// polygons (Peters 2021, section 3.7). Such a sample may miss the polygon. A caller that finds the light from the
/// sampled direction then sees no hit and adds nothing, and its MIS weight is proportional to the negligible integral.
/// @param polygon Output of TryPrepareLinearlyTransformedCosinePolygon.
/// @param randomNumbers Uniform random numbers, each in [0, 1].
/// @param outDirectionTangent Unit tangent-space direction; written only on success.
/// @return Success; NonFiniteInput or InvalidArgument for random numbers outside [0, 1]; Degenerate when the clip is
/// empty; IllConditioned when single precision cannot represent the sample. outDirectionTangent is unchanged on
/// failure.
[[nodiscard]] MathStatus TrySampleLinearlyTransformedCosinePolygon(const LinearlyTransformedCosinePolygon& polygon,
                                                                   Vector2 randomNumbers,
                                                                   Vector3& outDirectionTangent) noexcept;

/// @brief Solid-angle density of TrySampleLinearlyTransformedCosinePolygon for one direction.
///
/// The density is p(w) divided by the polygon integral. The direction must lie in the polygon's solid angle: it was
/// sampled from this polygon, for example by another technique of the same light, or a ray in this direction hits the
/// polygon. No containment test is performed.
/// @param polygon Output of TryPrepareLinearlyTransformedCosinePolygon.
/// @param directionTangent Unit tangent-space direction.
/// @return Density per steradian; zero where (M^-1 w).z <= 0, for a nonfinite or zero direction, for an empty clip, or
/// where single precision cannot represent the density.
[[nodiscard]] float32 EvaluateLinearlyTransformedCosinePolygonDensity(const LinearlyTransformedCosinePolygon& polygon,
                                                                      Vector3 directionTangent) noexcept;
} // namespace ludus::foundation::math
