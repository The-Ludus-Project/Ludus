#pragma once

// CPU oracle for the polygonal-light-sampling render graph template
// (render_graphs/polygonal_light_sampling/main_shaders/polygonal_light.slang): the same four estimators of one
// emissive triangle at one receiver, implemented with FoundationMath's C++ projected solid angle and LTC primitives,
// the renderer's CPU LTC table lookup and the RT1 reference BSDF. Visibility and emission maps are scene inputs the
// template adds in shade.slang; the oracle shades an unoccluded triangle with constant emission. The GPU parity test
// (polygonal_light_sampling_parity_tests.cpp) and the unbiasedness test (polygonal_light_sampling_oracle_tests.cpp)
// both use it. Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on
// Graphics 40(4), Article 140, 2021, https://doi.org/10.1145/3450626.3459672, sections 3, 4.1-4.3 and 6.1; the
// template's source names the departures, which the oracle shares.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::testing
{
/// The template's estimators, in output order (polygonal_light.slang).
enum class PolygonalLightEstimator : foundation::uint8
{
    ClampedOptimalMis = 0,   ///< One sample per technique, Peters 2021, section 4.3.
    BalanceHeuristic = 1,    ///< One sample per technique, Veach's balance heuristic.
    ProjectedSolidAngle = 2, ///< The projected solid angle sample alone (technique 0).
    Ltc = 3,                 ///< The LTC sample alone (technique 1); estimates the shading within the LTC's support.
};
inline constexpr foundation::usize POLYGONAL_LIGHT_ESTIMATOR_COUNT = 4;
inline constexpr foundation::usize POLYGONAL_LIGHT_VERTEX_COUNT = 3;

/// One receiver and one emissive triangle.
struct PolygonalLightCase final
{
    foundation::math::Vector3 ReceiverPositionWorld{};
    /// Unit shading normal before section 6.1's clip to the hemisphere of ViewDirectionWorld.
    foundation::math::Vector3 NormalWorld{0, 0, 1};
    /// Unit direction towards the viewer.
    foundation::math::Vector3 ViewDirectionWorld{0, 0, 1};
    /// Standard BSDF parameters; see ReferenceBsdf.
    foundation::math::Vector3 BaseColor{1, 1, 1};
    foundation::float32 Metallic = 0.0F;
    foundation::float32 Roughness = 0.5F;
    foundation::StaticArray<foundation::math::Vector3, POLYGONAL_LIGHT_VERTEX_COUNT> PositionsWorld{};
    /// Constant emitted radiance L_e per channel.
    foundation::math::Vector3 Emission{};
    bool IsEmissionTwoSided = false;
    /// Peters' visibility estimate v in [0, 1] (section 4.3).
    foundation::float32 Visibility = 0.5F;
};

/// A case prepared for both techniques, as TryPreparePolygonalLight prepares it in the template.
struct PolygonalLightOracle final
{
    PolygonalLightCase Case{};
    /// The shading frame: Case.NormalWorld clipped to the view's hemisphere (Peters 2021, section 6.1), as
    /// ClipShadingNormalToOutgoingHemisphere clips it. The BSDF and both techniques use this normal.
    foundation::math::Vector3 NormalWorld{};
    foundation::math::Vector3 TangentWorld{};
    foundation::math::Vector3 BitangentWorld{};
    foundation::math::LinearlyTransformedCosine Specular{};
    bool HasSpecular = false;
    /// a_0 and a_1 per channel (section 4.2).
    foundation::math::Vector3 DiffuseAlbedo{};
    foundation::math::Vector3 SpecularAlbedo{};
    foundation::math::ProjectedSolidAnglePolygon Diffuse{};
    foundation::math::LinearlyTransformedCosinePolygon SpecularPolygon{};
    /// E_0 and E_1.
    foundation::float32 DiffuseIntegral = 0.0F;
    foundation::float32 SpecularIntegral = 0.0F;
    /// c_0 and c_1 per channel.
    foundation::math::Vector3 DiffuseCoefficient{};
    foundation::math::Vector3 SpecularCoefficient{};
};

/// Prepare a case. Returns false, leaving outOracle unspecified, for a nonfinite normal, view, polygon or coefficient.
[[nodiscard]] bool TryPreparePolygonalLightOracle(const PolygonalLightCase& lightCase,
                                                  PolygonalLightOracle& outOracle) noexcept;

/// The four estimators' radiance for one projected solid angle sample and one LTC sample, each two uniform numbers in
/// [0, 1). Both MIS estimators use both samples; each single-technique estimator uses its own. Returns false for a
/// reference BSDF evaluation that rejects its input; a sample that misses the triangle contributes zero.
[[nodiscard]] bool TryEstimatePolygonalLight(
    const PolygonalLightOracle& oracle,
    foundation::math::Vector2 projectedSolidAngleSample,
    foundation::math::Vector2 ltcSample,
    foundation::StaticArray<foundation::math::Vector3, POLYGONAL_LIGHT_ESTIMATOR_COUNT>& outRadiance) noexcept;

/// The unshadowed integrand f(omega) cos(theta) L_e at a unit direction that meets the triangle, zero when the triangle
/// does not emit towards the receiver. Returns false for a rejected BSDF evaluation.
[[nodiscard]] bool TryEvaluatePolygonalLightIntegrand(const PolygonalLightOracle& oracle,
                                                      foundation::math::Vector3 directionWorld,
                                                      foundation::math::Vector3& outIntegrand) noexcept;

/// True where the LTC technique can sample a unit direction: the receiver has a specular LTC whose clip is not empty
/// and whose density is positive there. The LTC estimator's expectation is the integrand restricted to this set.
[[nodiscard]] bool IsInLtcSupport(const PolygonalLightOracle& oracle,
                                  foundation::math::Vector3 directionWorld) noexcept;
} // namespace ludus::graphics::renderer::testing
