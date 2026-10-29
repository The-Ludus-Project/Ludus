#include "polygonal_light_sampling_oracle.h"

#include "../src/internal/ltc_table.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/shading_normal.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/graphics/renderer/reference.hpp>

// Each function follows its counterpart in render_graphs/polygonal_light_sampling/main_shaders/polygonal_light.slang
// step by step, so the GPU parity test compares one algorithm in two languages. Thanks to Christoph Peters, "BRDF
// Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021,
// https://doi.org/10.1145/3450626.3459672: section 3 (projected solid angle sampling), 4.1 (LTC sampling and the
// empty-clip rule), 4.2 (the weighted balance heuristic, per color channel, with the diffuse albedo clamped to 0.01)
// and 4.3 (clamped optimal MIS), and section 6.1's clip of the shading normal to the view's hemisphere, through
// FoundationMath's TryClipShadingNormalToOutgoingHemisphere, the C++ twin of the template's shading_normal.slang.
namespace ludus::graphics::renderer::testing
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
// The constants of polygonal_light.slang.
constexpr float32 MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO = 0.01F;
constexpr float32 DIELECTRIC_SPECULAR_REFLECTANCE = 0.04F;
constexpr float32 FRAME_HELPER_AXIS_LIMIT = 0.999F;
constexpr usize PROJECTED_SOLID_ANGLE_TECHNIQUE = 0;
constexpr usize LTC_TECHNIQUE = 1;
constexpr usize TECHNIQUE_COUNT = 2;

math::Vector3 NormalizeOrZero(math::Vector3 value) noexcept
{
    math::Vector3 result;
    return math::TryNormalize(value, result) == math::MathStatus::Success ? result : math::Vector3{};
}

math::Vector3 TransformWorldToTangent(const PolygonalLightOracle& oracle, math::Vector3 vectorWorld) noexcept
{
    return {math::Dot(vectorWorld, oracle.TangentWorld),
            math::Dot(vectorWorld, oracle.BitangentWorld),
            math::Dot(vectorWorld, oracle.NormalWorld)};
}

math::Vector3 TransformTangentToWorld(const PolygonalLightOracle& oracle, math::Vector3 vectorTangent) noexcept
{
    return oracle.TangentWorld * vectorTangent.X + oracle.BitangentWorld * vectorTangent.Y +
           oracle.NormalWorld * vectorTangent.Z;
}

math::Vector3 MaxZero(math::Vector3 value) noexcept
{
    return {math::Max(value.X, 0.0F), math::Max(value.Y, 0.0F), math::Max(value.Z, 0.0F)};
}

bool IsUsedTechnique(PolygonalLightEstimator estimator, usize technique) noexcept
{
    if (estimator == PolygonalLightEstimator::ProjectedSolidAngle)
    {
        return technique == PROJECTED_SOLID_ANGLE_TECHNIQUE;
    }
    if (estimator == PolygonalLightEstimator::Ltc)
    {
        return technique == LTC_TECHNIQUE;
    }
    return true;
}

/// TryIntersectTrianglePlane: whether a direction from the receiver meets the triangle in single precision.
bool IsTriangleHit(const PolygonalLightOracle& oracle, math::Vector3 directionWorld) noexcept
{
    const math::Vector3 origin = oracle.Case.ReceiverPositionWorld;
    const math::Vector3 position0 = oracle.Case.PositionsWorld[0];
    const math::Vector3 edge1 = oracle.Case.PositionsWorld[1] - position0;
    const math::Vector3 edge2 = oracle.Case.PositionsWorld[2] - position0;
    const math::Vector3 normal = math::Cross(edge1, edge2);
    const float32 normalLengthSquared = math::Dot(normal, normal);
    const float32 denominator = math::Dot(normal, directionWorld);
    if ((normalLengthSquared > 0.0F) == false || (denominator > 0.0F || denominator < 0.0F) == false)
    {
        return false;
    }
    const float32 distance = math::Dot(normal, position0 - origin) / denominator;
    if (math::IsFinite(distance) == false || (distance > 0.0F) == false)
    {
        return false;
    }
    const math::Vector3 offset = origin + directionWorld * distance - position0;
    const float32 u = math::Dot(normal, math::Cross(offset, edge2)) / normalLengthSquared;
    const float32 v = math::Dot(normal, math::Cross(edge1, offset)) / normalLengthSquared;
    return math::IsFinite(u) == true && math::IsFinite(v) == true && u >= 0.0F && v >= 0.0F && u + v <= 1.0F;
}

/// ComputePolygonalLightWeight.
math::Vector3 ComputeWeight(const PolygonalLightOracle& oracle,
                            PolygonalLightEstimator estimator,
                            usize technique,
                            math::Vector2 densities) noexcept
{
    const float32 techniqueDensity = technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? densities.X : densities.Y;
    if ((techniqueDensity > 0.0F) == false || IsUsedTechnique(estimator, technique) == false)
    {
        return {};
    }
    if (estimator == PolygonalLightEstimator::ProjectedSolidAngle || estimator == PolygonalLightEstimator::Ltc)
    {
        return {1, 1, 1};
    }
    const float32 balance = techniqueDensity / (densities.X + densities.Y);
    if (estimator == PolygonalLightEstimator::BalanceHeuristic)
    {
        return {balance, balance, balance};
    }
    const math::Vector3 techniqueCoefficient =
        technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? oracle.DiffuseCoefficient : oracle.SpecularCoefficient;
    const math::Vector3 weightedDensities = techniqueCoefficient * techniqueDensity;
    const math::Vector3 weightedSums =
        oracle.DiffuseCoefficient * densities.X + oracle.SpecularCoefficient * densities.Y;
    const float32 visibility = oracle.Case.Visibility;
    const auto blend = [balance, visibility](float32 weightedDensity, float32 weightedSum) noexcept {
        const float32 weighted = weightedSum > 0.0F ? weightedDensity / weightedSum : balance;
        return visibility * weighted + (1.0F - visibility) * balance;
    };
    return {blend(weightedDensities.X, weightedSums.X),
            blend(weightedDensities.Y, weightedSums.Y),
            blend(weightedDensities.Z, weightedSums.Z)};
}

bool TryEvaluateBsdf(const PolygonalLightOracle& oracle, math::Vector3 directionWorld, math::Vector3& outValue) noexcept
{
    const ReferenceBsdf bsdf{oracle.Case.BaseColor, oracle.Case.Metallic, oracle.Case.Roughness};
    ReferenceBsdfEvaluation evaluation;
    if (EvaluateReferenceBsdf(bsdf, oracle.NormalWorld, oracle.Case.ViewDirectionWorld, directionWorld, evaluation) !=
        rhi::RasterStatus::Ready)
    {
        return false;
    }
    outValue = evaluation.Value;
    return true;
}
} // namespace

bool TryPreparePolygonalLightOracle(const PolygonalLightCase& lightCase, PolygonalLightOracle& outOracle) noexcept
{
    PolygonalLightOracle oracle;
    oracle.Case = lightCase;
    // TryLoadShadingPoint's clip, then MakePolygonalLightReceiver.
    if (math::TryClipShadingNormalToOutgoingHemisphere(lightCase.NormalWorld,
                                                       lightCase.ViewDirectionWorld,
                                                       oracle.NormalWorld) != math::MathStatus::Success)
    {
        return false;
    }
    const math::Vector3 normal = oracle.NormalWorld;
    const math::Vector3 helper =
        math::Abs(normal.Z) < FRAME_HELPER_AXIS_LIMIT ? math::Vector3{0, 0, 1} : math::Vector3{0, 1, 0};
    oracle.TangentWorld = NormalizeOrZero(math::Cross(helper, normal));
    oracle.BitangentWorld = math::Cross(normal, oracle.TangentWorld);
    const math::Vector3 specularReflectance =
        math::Vector3{1, 1, 1} * (DIELECTRIC_SPECULAR_REFLECTANCE * (1.0F - lightCase.Metallic)) +
        lightCase.BaseColor * lightCase.Metallic;
    const internal::SpecularLtc specular =
        internal::LookupSpecularLtc(lightCase.Roughness,
                                    TransformWorldToTangent(oracle, lightCase.ViewDirectionWorld),
                                    specularReflectance);
    oracle.HasSpecular =
        math::TryMakeLinearlyTransformedCosine(specular.InverseTransform, oracle.Specular) == math::MathStatus::Success;
    oracle.SpecularAlbedo = oracle.HasSpecular == true
                                ? MaxZero(internal::ComputeSpecularLtcAlbedo(specular, specularReflectance))
                                : math::Vector3{};
    const math::Vector3 diffuse =
        math::Hadamard(lightCase.BaseColor * (1.0F - lightCase.Metallic), math::Vector3{1, 1, 1} - specularReflectance);
    oracle.DiffuseAlbedo = {math::Max(diffuse.X, MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO),
                            math::Max(diffuse.Y, MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO),
                            math::Max(diffuse.Z, MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO)};
    // TryPreparePolygonalLight.
    const math::Vector3 geometricNormal = math::Cross(lightCase.PositionsWorld[1] - lightCase.PositionsWorld[0],
                                                      lightCase.PositionsWorld[2] - lightCase.PositionsWorld[0]);
    const bool isFacing =
        lightCase.IsEmissionTwoSided == true ||
        math::Dot(geometricNormal, lightCase.ReceiverPositionWorld - lightCase.PositionsWorld[0]) > 0.0F;
    const math::Vector3 facingEmission = isFacing == true ? MaxZero(lightCase.Emission) : math::Vector3{};
    math::ProjectedPolygonInput polygon;
    polygon.VertexCount = static_cast<uint32>(POLYGONAL_LIGHT_VERTEX_COUNT);
    for (usize vertex = 0; vertex < POLYGONAL_LIGHT_VERTEX_COUNT; ++vertex)
    {
        polygon.PositionsTangent[vertex] =
            TransformWorldToTangent(oracle, lightCase.PositionsWorld[vertex] - lightCase.ReceiverPositionWorld);
    }
    if (math::TryPrepareProjectedSolidAnglePolygon(polygon, oracle.Diffuse) != math::MathStatus::Success)
    {
        return false;
    }
    oracle.DiffuseIntegral = oracle.Diffuse.ProjectedSolidAngle / math::kPiF;
    if (oracle.HasSpecular == true)
    {
        if (math::TryPrepareLinearlyTransformedCosinePolygon(polygon, oracle.Specular, oracle.SpecularPolygon) !=
            math::MathStatus::Success)
        {
            return false;
        }
        oracle.SpecularIntegral = math::GetLinearlyTransformedCosinePolygonIntegral(oracle.SpecularPolygon);
    }
    oracle.DiffuseCoefficient = math::Hadamard(facingEmission, oracle.DiffuseAlbedo) * oracle.DiffuseIntegral;
    oracle.SpecularCoefficient = math::Hadamard(facingEmission, oracle.SpecularAlbedo) * oracle.SpecularIntegral;
    if (math::IsFinite(oracle.DiffuseCoefficient) == false || math::IsFinite(oracle.SpecularCoefficient) == false)
    {
        return false;
    }
    outOracle = oracle;
    return true;
}

bool TryEstimatePolygonalLight(const PolygonalLightOracle& oracle,
                               math::Vector2 projectedSolidAngleSample,
                               math::Vector2 ltcSample,
                               StaticArray<math::Vector3, POLYGONAL_LIGHT_ESTIMATOR_COUNT>& outRadiance) noexcept
{
    StaticArray<math::Vector3, POLYGONAL_LIGHT_ESTIMATOR_COUNT> radiance{};
    // ShadePolygonalLights skips a triangle that emits nothing towards the receiver or lies below its horizon.
    const math::Vector3 coefficient = oracle.DiffuseCoefficient;
    if (coefficient.X > 0.0F || coefficient.Y > 0.0F || coefficient.Z > 0.0F)
    {
        const StaticArray<math::Vector2, TECHNIQUE_COUNT> samples{{projectedSolidAngleSample, ltcSample}};
        for (usize technique = 0; technique < TECHNIQUE_COUNT; ++technique)
        {
            // TrySamplePolygonalLight.
            math::Vector3 directionTangent;
            bool isSampled = false;
            if (technique == PROJECTED_SOLID_ANGLE_TECHNIQUE)
            {
                isSampled =
                    oracle.DiffuseIntegral > 0.0F &&
                    math::TrySampleProjectedSolidAnglePolygon(oracle.Diffuse, samples[technique], directionTangent) ==
                        math::MathStatus::Success;
            }
            else
            {
                isSampled = oracle.SpecularIntegral > 0.0F && math::TrySampleLinearlyTransformedCosinePolygon(
                                                                  oracle.SpecularPolygon,
                                                                  samples[technique],
                                                                  directionTangent) == math::MathStatus::Success;
            }
            if (isSampled == false)
            {
                continue;
            }
            const math::Vector3 direction = NormalizeOrZero(TransformTangentToWorld(oracle, directionTangent));
            const math::Vector3 normalizedTangent = TransformWorldToTangent(oracle, direction);
            const math::Vector2 densities{
                math::EvaluateProjectedSolidAnglePolygonDensity(oracle.Diffuse, normalizedTangent),
                math::EvaluateLinearlyTransformedCosinePolygonDensity(oracle.SpecularPolygon, normalizedTangent)};
            const float32 techniqueDensity = technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? densities.X : densities.Y;
            const float32 cosine = math::Dot(oracle.NormalWorld, direction);
            if ((techniqueDensity > 0.0F) == false || (cosine > 0.0F) == false ||
                IsTriangleHit(oracle, direction) == false)
            {
                continue;
            }
            math::Vector3 bsdfValue;
            if (TryEvaluateBsdf(oracle, direction, bsdfValue) == false)
            {
                return false;
            }
            // EstimatePolygonalLightSample, times the constant emission the scene would return.
            for (usize estimator = 0; estimator < POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
            {
                const math::Vector3 weight =
                    ComputeWeight(oracle, static_cast<PolygonalLightEstimator>(estimator), technique, densities);
                radiance[estimator] =
                    radiance[estimator] + math::Hadamard(math::Hadamard(bsdfValue, weight), oracle.Case.Emission) *
                                              (cosine / techniqueDensity);
            }
        }
    }
    outRadiance = radiance;
    return true;
}

bool TryEvaluatePolygonalLightIntegrand(const PolygonalLightOracle& oracle,
                                        math::Vector3 directionWorld,
                                        math::Vector3& outIntegrand) noexcept
{
    const float32 cosine = math::Dot(oracle.NormalWorld, directionWorld);
    const math::Vector3 coefficient = oracle.DiffuseCoefficient;
    if ((cosine > 0.0F) == false || (coefficient.X > 0.0F || coefficient.Y > 0.0F || coefficient.Z > 0.0F) == false)
    {
        outIntegrand = {};
        return true;
    }
    math::Vector3 bsdfValue;
    if (TryEvaluateBsdf(oracle, directionWorld, bsdfValue) == false)
    {
        return false;
    }
    outIntegrand = math::Hadamard(bsdfValue, oracle.Case.Emission) * cosine;
    return true;
}

bool IsInLtcSupport(const PolygonalLightOracle& oracle, math::Vector3 directionWorld) noexcept
{
    return oracle.HasSpecular == true && oracle.SpecularIntegral > 0.0F &&
           math::EvaluateLinearlyTransformedCosine(oracle.Specular, TransformWorldToTangent(oracle, directionWorld)) >
               0.0F;
}
} // namespace ludus::graphics::renderer::testing
