#include "internal/reference_light_sampling.h"
#include "internal/ltc_table.h"
#include "internal/reference_data.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/weighted_reservoir.hpp>

// The estimator, sources and departures are described in reference_light_sampling.h. The arithmetic follows the
// ris-projected-solid-angle template's projected_solid_angle_light.slang and resampled_light.slang step for step, so
// that renderer_reference.slang can port both with the same dimensions.
namespace ludus::graphics::renderer::internal
{
namespace
{
/// BT.709 luminance weights.
constexpr math::Vector3 LUMINANCE_WEIGHTS{0.2126F, 0.7152F, 0.0722F};
/// Peters 2021, section 4.2: the clamp keeps the diffuse technique, and so the target, positive over every emitter
/// above the horizon.
constexpr float32 MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO = 0.01F;
/// Schlick's dielectric F0 of the frozen reflection model (ReferenceBsdf).
constexpr float32 DIELECTRIC_SPECULAR_REFLECTANCE = 0.04F;
/// Above this |n.z| the frame's helper axis switches from +z to +y, as the BSDF sampling frame does.
constexpr float32 FRAME_HELPER_AXIS_LIMIT = 0.999F;
constexpr usize DIFFUSE_TECHNIQUE = 0;
constexpr usize SPECULAR_TECHNIQUE = 1;
constexpr uint32 DIFFUSE_FIRST_SLOT = 1;
constexpr uint32 TRIANGLE_VERTEX_COUNT = 3;

math::Vector3 TransformWorldToTangent(const ReferenceReceiverFrame& frame, math::Vector3 vectorWorld) noexcept
{
    return {math::Dot(vectorWorld, frame.TangentWorld),
            math::Dot(vectorWorld, frame.BitangentWorld),
            math::Dot(vectorWorld, frame.NormalWorld)};
}
math::Vector3 TransformTangentToWorld(const ReferenceReceiverFrame& frame, math::Vector3 vectorTangent) noexcept
{
    return frame.TangentWorld * vectorTangent.X + frame.BitangentWorld * vectorTangent.Y +
           frame.NormalWorld * vectorTangent.Z;
}
math::Vector3 ClampBelow(math::Vector3 value, float32 minimum) noexcept
{
    return {math::Max(value.X, minimum), math::Max(value.Y, minimum), math::Max(value.Z, minimum)};
}
/// Sampler version 2 address of `slot` at scattering event `depth`.
math::RandomAddress MakeAddress(uint32 pixel, uint32 sample, uint32 depth, uint32 slot) noexcept
{
    return {pixel, sample, ReferenceBounceDimension(ReferenceLightSampling::ResampledProjectedSolidAngle, depth, slot)};
}

/// One emissive triangle evaluated for one receiver.
struct EmitterTarget final
{
    /// The evaluated scene triangle.
    uint32 TriangleIndex = 0;
    ReferenceTriangleLight Light;
    /// (c_0, c_1); their sum is Target.
    math::Vector2 Coefficients{};
    /// p-hat: Y(L) (Y(a_0) E_0 + Y(a_1) E_1), zero for a one-sided emitter seen from behind.
    float32 Target = 0;
};

/// Evaluate p-hat for an emitter triangle. The constant emission factor bounds textured emission from above, so the
/// target needs no texture lookup. Returns false for an invalid triangle or a nonfinite polygon or target.
bool TryEvaluateEmitterTarget(const ReferenceJob& job,
                              const ReferenceReceiverFrame& frame,
                              uint32 triangleIndex,
                              EmitterTarget& outTarget) noexcept
{
    if (triangleIndex >= job.Scene.Triangles.GetSize())
    {
        return false;
    }
    const ReferenceTriangle& triangle = job.Scene.Triangles[triangleIndex];
    const ReferenceSurfaceData& surface = *job.Scene.Instances[triangle.Instance].Surface;
    EmitterTarget result;
    result.TriangleIndex = triangleIndex;
    float32 emissionLuminance = ComputeReferenceLuminance(ClampBelow(surface.Emission, 0));
    // A planar triangle faces the receiver from every point on it, as the emitter-hit test sees it at one point.
    if (surface.IsEmissionTwoSided == false &&
        math::Dot(triangle.GeometricNormal, frame.PositionWorld - triangle.Positions[0]) <= 0)
    {
        emissionLuminance = 0;
    }
    if ((emissionLuminance > 0) == true)
    {
        if (TryPrepareReferenceTriangleLight(frame,
                                             triangle.Positions[0],
                                             triangle.Positions[1],
                                             triangle.Positions[2],
                                             result.Light) == false)
        {
            return false;
        }
        result.Coefficients = {emissionLuminance * frame.DiffuseAlbedoLuminance * result.Light.DiffuseIntegral,
                               emissionLuminance * frame.SpecularAlbedoLuminance * result.Light.SpecularIntegral};
        result.Target = result.Coefficients.X + result.Coefficients.Y;
        if (math::IsFinite(result.Target) == false)
        {
            return false;
        }
    }
    outTarget = result;
    return true;
}

/// Z-hat = (1/K) sum_k p-hat(t'_k) / p_src(t'_k) over K independent power-CDF draws from the normalization slots, so
/// it is independent of the reservoir and of the BSDF sample. A draw without a sample adds zero. Returns false for a
/// failed target evaluation or a nonfinite estimate.
bool TryEnsureTargetTotal(const ReferenceJob& job,
                          uint32 pixel,
                          uint32 sample,
                          uint32 depth,
                          const ReferenceReceiverFrame& frame,
                          ReferenceTargetTotal& inoutTotal) noexcept
{
    if (inoutTotal.IsReady == true)
    {
        return true;
    }
    float32 sum = 0;
    for (uint32 k = 0; k < LUDUS_REFERENCE_RIS_NORMALIZATION_CANDIDATE_COUNT; ++k)
    {
        const ReferenceEmissiveTriangleSelection candidate = SelectReferenceEmissiveTriangle(
            job.Scene,
            job.Key,
            MakeAddress(pixel, sample, depth, LUDUS_REFERENCE_RIS_NORMALIZATION_FIRST_SLOT + k));
        if (candidate.HasSample == false)
        {
            continue;
        }
        EmitterTarget target;
        if (TryEvaluateEmitterTarget(job, frame, candidate.TriangleIndex, target) == false)
        {
            return false;
        }
        sum += target.Target / candidate.SelectionPmf;
    }
    const float32 estimate = sum / static_cast<float32>(LUDUS_REFERENCE_RIS_NORMALIZATION_CANDIDATE_COUNT);
    if (math::IsFinite(estimate) == false)
    {
        return false;
    }
    inoutTotal.Value = estimate;
    inoutTotal.IsReady = true;
    return true;
}

/// P-tilde(t) for an evaluated emitter, computing Z-hat on first use. Zero when the resampled strategy cannot propose
/// the triangle. Returns false when Z-hat fails.
bool TryEvaluateSurrogate(const ReferenceJob& job,
                          uint32 pixel,
                          uint32 sample,
                          uint32 depth,
                          const ReferenceReceiverFrame& frame,
                          float32 emissiveCategoryProbability,
                          const EmitterTarget& target,
                          ReferenceTargetTotal& inoutTotal,
                          float32& outProbability) noexcept
{
    const float32 sourcePmf = EvaluateReferenceEmissiveTriangleSelectionPmf(job.Scene, target.TriangleIndex);
    if ((sourcePmf > 0) == false || (target.Target > 0) == false || (emissiveCategoryProbability > 0) == false)
    {
        outProbability = 0;
        return true;
    }
    if (TryEnsureTargetTotal(job, pixel, sample, depth, frame, inoutTotal) == false)
    {
        return false;
    }
    outProbability = EvaluateReferenceSurrogateProbability(emissiveCategoryProbability,
                                                           target.Target,
                                                           sourcePmf,
                                                           inoutTotal.Value,
                                                           LUDUS_REFERENCE_RIS_CANDIDATE_COUNT);
    return true;
}

/// The reservoir's selected triangle y with its contribution weight W = w_sum / (M p-hat(y)).
struct ResampledTriangle final
{
    uint32 TriangleIndex = 0;
    float32 ContributionWeight = 0;
    bool HasSelection = false;
};

/// Stream M power-CDF candidates through the reservoir in slot order. A zero weight sum, including a scene without an
/// emissive CDF, succeeds without a selection. Returns false for a failed target evaluation or reservoir update.
bool TryResampleEmitter(const ReferenceJob& job,
                        uint32 pixel,
                        uint32 sample,
                        uint32 depth,
                        const ReferenceReceiverFrame& frame,
                        ResampledTriangle& outSelection) noexcept
{
    math::WeightedReservoir reservoir;
    ResampledTriangle result;
    float32 selectedTarget = 0;
    for (uint32 i = 0; i < LUDUS_REFERENCE_RIS_CANDIDATE_COUNT; ++i)
    {
        const uint32 slot = LUDUS_REFERENCE_RIS_CANDIDATE_FIRST_SLOT + i * LUDUS_REFERENCE_RIS_SLOTS_PER_CANDIDATE;
        const ReferenceEmissiveTriangleSelection candidate =
            SelectReferenceEmissiveTriangle(job.Scene, job.Key, MakeAddress(pixel, sample, depth, slot));
        if (candidate.HasSample == false)
        {
            outSelection = {};
            return true;
        }
        EmitterTarget target;
        if (TryEvaluateEmitterTarget(job, frame, candidate.TriangleIndex, target) == false)
        {
            return false;
        }
        const math::ReservoirCandidate entry
        {
            .Weight = target.Target / candidate.SelectionPmf,
            .Ticket = math::SampleFloat01(job.Key, MakeAddress(pixel, sample, depth, slot + 1)),
        };
        bool isSelected = false;
        if (math::TryAddReservoirCandidate(entry, reservoir, isSelected) != math::MathStatus::Success)
        {
            return false;
        }
        if (isSelected == true)
        {
            result.TriangleIndex = candidate.TriangleIndex;
            selectedTarget = target.Target;
        }
    }
    if (reservoir.HasSelection() == true)
    {
        if (math::TryComputeReservoirContributionWeight(reservoir, selectedTarget, result.ContributionWeight) !=
            math::MathStatus::Success)
        {
            return false;
        }
        result.HasSelection = true;
    }
    outSelection = result;
    return true;
}

/// One sample of the selected emitter by `technique`: the direction, where it meets the triangle and the emission
/// found there. Densities are evaluated at the normalized world direction, as the BSDF-hit density evaluates them, so
/// the MIS weight is one function of the direction at both sites. No sample when the technique is disabled or cannot
/// sample the direction, or the direction misses the triangle in FP32.
ReferenceResampledLightSample SampleTechnique(const ReferenceJob& job,
                                              const ReferenceReceiverFrame& frame,
                                              uint32 triangleIndex,
                                              const EmitterTarget& target,
                                              usize technique,
                                              math::Vector2 randomNumbers,
                                              float32 surrogateProbability) noexcept
{
    ReferenceResampledLightSample result;
    math::Vector3 directionTangent{};
    math::MathStatus sampled = math::MathStatus::Degenerate;
    if (technique == DIFFUSE_TECHNIQUE)
    {
        sampled = math::TrySampleProjectedSolidAnglePolygon(target.Light.Diffuse, randomNumbers, directionTangent);
    }
    else if (target.Light.SpecularIntegral > 0)
    {
        sampled =
            math::TrySampleLinearlyTransformedCosinePolygon(target.Light.Specular, randomNumbers, directionTangent);
    }
    if (sampled != math::MathStatus::Success)
    {
        return result;
    }
    const math::Vector3 directionWorld = ReferenceNormalize(TransformTangentToWorld(frame, directionTangent));
    const math::Vector2 densities = EvaluateReferenceLightTechniqueDensities(frame, target.Light, directionWorld);
    const float32 techniqueDensity = technique == DIFFUSE_TECHNIQUE ? densities.X : densities.Y;
    const ReferenceTriangle& triangle = job.Scene.Triangles[triangleIndex];
    float32 distanceWorld = 0;
    math::Vector2 barycentric{};
    if ((techniqueDensity > 0) == false || TryIntersectReferenceTrianglePlane(frame.PositionWorld,
                                                                              directionWorld,
                                                                              triangle.Positions[0],
                                                                              triangle.Positions[1],
                                                                              triangle.Positions[2],
                                                                              distanceWorld,
                                                                              barycentric) == false)
    {
        return result;
    }
    const ReferenceInstance& instance = job.Scene.Instances[triangle.Instance];
    const math::Vector2 uv = InterpolateReferenceUv(triangle, barycentric.X, barycentric.Y);
    result.DirectionWorld = directionWorld;
    result.PointWorld = InterpolateReferencePosition(triangle, barycentric.X, barycentric.Y);
    result.DistanceWorld = distanceWorld;
    // Sidedness, range and opacity at the sampled point, as UniformArea applies them.
    if ((instance.Surface->IsEmissionTwoSided == true || math::Dot(triangle.GeometricNormal, -directionWorld) > 0) &&
        distanceWorld <= job.Description.SecondaryDistance &&
        ReferenceOpacity(instance, uv, InterpolateReferenceColor(triangle, barycentric.X, barycentric.Y).W) == true)
    {
        result.Radiance = SampleReferenceEmission(instance, uv);
    }
    const math::Vector2 techniqueWeights =
        ComputeReferenceLightTechniqueWeights(target.Coefficients, densities, REFERENCE_LIGHT_TECHNIQUE_VISIBILITY);
    result.TechniqueDensity = techniqueDensity;
    result.TechniqueWeight = technique == DIFFUSE_TECHNIQUE ? techniqueWeights.X : techniqueWeights.Y;
    result.LightDensity = surrogateProbability * (densities.X + densities.Y);
    result.HasSample = true;
    return result;
}
} // namespace

float32 ComputeReferenceLuminance(math::Vector3 value) noexcept
{
    return math::Dot(value, LUMINANCE_WEIGHTS);
}

// Position, normal and view follow the order of the shading point that every BSDF evaluation names.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ReferenceReceiverFrame MakeReferenceReceiverFrame(math::Vector3 positionWorld,
                                                  math::Vector3 normalWorld,
                                                  math::Vector3 viewWorld,
                                                  const ReferenceBsdf& bsdf) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    ReferenceReceiverFrame frame;
    frame.PositionWorld = positionWorld;
    frame.NormalWorld = normalWorld;
    const math::Vector3 helperWorld =
        math::Abs(normalWorld.Z) < FRAME_HELPER_AXIS_LIMIT ? math::Vector3{0, 0, 1} : math::Vector3{0, 1, 0};
    frame.TangentWorld = ReferenceNormalize(math::Cross(helperWorld, normalWorld));
    frame.BitangentWorld = math::Cross(normalWorld, frame.TangentWorld);
    const math::Vector3 specularReflectance = math::Vector3{DIELECTRIC_SPECULAR_REFLECTANCE,
                                                            DIELECTRIC_SPECULAR_REFLECTANCE,
                                                            DIELECTRIC_SPECULAR_REFLECTANCE} *
                                                  (1 - bsdf.Metallic) +
                                              bsdf.BaseColor * bsdf.Metallic;
    // The table's F0 slice follows the luminance of this receiver's F0 (Peters 2021, section 4.1).
    const SpecularLtc specular =
        LookupSpecularLtc(bsdf.Roughness, TransformWorldToTangent(frame, viewWorld), specularReflectance);
    frame.HasSpecular =
        math::TryMakeLinearlyTransformedCosine(specular.InverseTransform, frame.Specular) == math::MathStatus::Success;
    frame.SpecularAlbedoLuminance =
        frame.HasSpecular == true
            ? math::Max(ComputeReferenceLuminance(ComputeSpecularLtcAlbedo(specular, specularReflectance)), 0.0F)
            : 0.0F;
    const math::Vector3 diffuseAlbedo =
        ClampBelow(math::Hadamard(bsdf.BaseColor * (1 - bsdf.Metallic), math::Vector3{1, 1, 1} - specularReflectance),
                   MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO);
    frame.DiffuseAlbedoLuminance = ComputeReferenceLuminance(diffuseAlbedo);
    return frame;
}

// The vertices follow the triangle's stored order, which fixes its winding.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool TryPrepareReferenceTriangleLight(const ReferenceReceiverFrame& frame,
                                      math::Vector3 position0World,
                                      math::Vector3 position1World,
                                      math::Vector3 position2World,
                                      ReferenceTriangleLight& outLight) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    math::ProjectedPolygonInput polygon;
    polygon.VertexCount = TRIANGLE_VERTEX_COUNT;
    polygon.PositionsTangent[0] = TransformWorldToTangent(frame, position0World - frame.PositionWorld);
    polygon.PositionsTangent[1] = TransformWorldToTangent(frame, position1World - frame.PositionWorld);
    polygon.PositionsTangent[2] = TransformWorldToTangent(frame, position2World - frame.PositionWorld);
    ReferenceTriangleLight result;
    if (math::TryPrepareProjectedSolidAnglePolygon(polygon, result.Diffuse) != math::MathStatus::Success)
    {
        return false;
    }
    result.DiffuseIntegral = result.Diffuse.ProjectedSolidAngle / math::kPiF;
    if (frame.HasSpecular == true)
    {
        if (math::TryPrepareLinearlyTransformedCosinePolygon(polygon, frame.Specular, result.Specular) !=
            math::MathStatus::Success)
        {
            return false;
        }
        result.SpecularIntegral = math::GetLinearlyTransformedCosinePolygonIntegral(result.Specular);
    }
    outLight = result;
    return true;
}

math::Vector2 EvaluateReferenceLightTechniqueDensities(const ReferenceReceiverFrame& frame,
                                                       const ReferenceTriangleLight& light,
                                                       math::Vector3 directionWorld) noexcept
{
    const math::Vector3 directionTangent = TransformWorldToTangent(frame, directionWorld);
    return {math::EvaluateProjectedSolidAnglePolygonDensity(light.Diffuse, directionTangent),
            math::EvaluateLinearlyTransformedCosinePolygonDensity(light.Specular, directionTangent)};
}

math::Vector2
ComputeReferenceLightTechniqueWeights(math::Vector2 coefficients, math::Vector2 densities, float32 visibility) noexcept
{
    const float32 densitySum = densities.X + densities.Y;
    if ((densitySum > 0) == false)
    {
        return {};
    }
    const math::Vector2 balance{densities.X / densitySum, densities.Y / densitySum};
    const math::Vector2 weightedDensities = math::Hadamard(coefficients, densities);
    const float32 weightedSum = weightedDensities.X + weightedDensities.Y;
    const math::Vector2 weighted =
        weightedSum > 0 ? math::Vector2{weightedDensities.X / weightedSum, weightedDensities.Y / weightedSum} : balance;
    return weighted * visibility + balance * (1 - visibility);
}

// The factors follow P-tilde = P_E min(p-hat / Z-hat, min(1, M p_src)) and the template's signature.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
float32 EvaluateReferenceSurrogateProbability(float32 emissiveCategoryProbability,
                                              float32 target,
                                              float32 sourcePmf,
                                              float32 targetTotal,
                                              uint32 candidateCount) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if ((emissiveCategoryProbability > 0) == false || (target > 0) == false || (sourcePmf > 0) == false)
    {
        return 0;
    }
    const float32 inclusionBound = math::Min(1.0F, static_cast<float32>(candidateCount) * sourcePmf);
    // Compare before dividing, so a tiny Z-hat never overflows the ratio.
    const float32 resampledProbability =
        targetTotal > 0 && target < inclusionBound * targetTotal ? target / targetTotal : inclusionBound;
    return emissiveCategoryProbability * resampledProbability;
}

// The vertices follow the triangle's stored order, which fixes the barycentric weights.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool TryIntersectReferenceTrianglePlane(math::Vector3 originWorld,
                                        math::Vector3 directionWorld,
                                        math::Vector3 position0World,
                                        math::Vector3 position1World,
                                        math::Vector3 position2World,
                                        float32& outDistanceWorld,
                                        math::Vector2& outBarycentric) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const math::Vector3 edge1World = position1World - position0World;
    const math::Vector3 edge2World = position2World - position0World;
    const math::Vector3 normalWorld = math::Cross(edge1World, edge2World);
    const float32 normalLengthSquared = math::Dot(normalWorld, normalWorld);
    const float32 denominator = math::Dot(normalWorld, directionWorld);
    if ((normalLengthSquared > 0) == false || (denominator > 0 || denominator < 0) == false)
    {
        return false;
    }
    const float32 distanceWorld = math::Dot(normalWorld, position0World - originWorld) / denominator;
    if (math::IsFinite(distanceWorld) == false || (distanceWorld > 0) == false)
    {
        return false;
    }
    const math::Vector3 offsetWorld = originWorld + directionWorld * distanceWorld - position0World;
    const math::Vector2 barycentric{math::Dot(normalWorld, math::Cross(offsetWorld, edge2World)) / normalLengthSquared,
                                    math::Dot(normalWorld, math::Cross(edge1World, offsetWorld)) / normalLengthSquared};
    if (math::IsFinite(barycentric) == false || barycentric.X < 0 || barycentric.Y < 0 ||
        barycentric.X + barycentric.Y > 1)
    {
        return false;
    }
    outDistanceWorld = distanceWorld;
    outBarycentric = barycentric;
    return true;
}

rhi::RasterStatus SampleReferenceResampledLight(const ReferenceJob& job,
                                                uint32 pixel,
                                                uint32 sample,
                                                uint32 depth,
                                                const ReferenceReceiverFrame& frame,
                                                float32 emissiveCategoryProbability,
                                                ReferenceTargetTotal& inoutTargetTotal,
                                                ReferenceResampledLight& outLight) noexcept
{
    ResampledTriangle selection;
    if (TryResampleEmitter(job, pixel, sample, depth, frame, selection) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    ReferenceResampledLight result;
    if (selection.HasSelection == false || (selection.ContributionWeight > 0) == false ||
        (emissiveCategoryProbability > 0) == false)
    {
        outLight = result;
        return rhi::RasterStatus::Ready;
    }
    EmitterTarget target;
    float32 surrogateProbability = 0;
    if (TryEvaluateEmitterTarget(job, frame, selection.TriangleIndex, target) == false ||
        TryEvaluateSurrogate(job,
                             pixel,
                             sample,
                             depth,
                             frame,
                             emissiveCategoryProbability,
                             target,
                             inoutTargetTotal,
                             surrogateProbability) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    // One diffuse sample omega_0 and, unless the LTC-space clip is empty, one LTC sample omega_1.
    const math::Vector2 diffuseRandom{
        math::SampleFloat01(job.Key, MakeAddress(pixel, sample, depth, DIFFUSE_FIRST_SLOT)),
        math::SampleFloat01(job.Key, MakeAddress(pixel, sample, depth, DIFFUSE_FIRST_SLOT + 1))};
    const math::Vector2 specularRandom{
        math::SampleFloat01(job.Key, MakeAddress(pixel, sample, depth, LUDUS_REFERENCE_LTC_FIRST_SLOT)),
        math::SampleFloat01(job.Key, MakeAddress(pixel, sample, depth, LUDUS_REFERENCE_LTC_FIRST_SLOT + 1)),
    };
    result.Samples[DIFFUSE_TECHNIQUE] = SampleTechnique(job,
                                                        frame,
                                                        selection.TriangleIndex,
                                                        target,
                                                        DIFFUSE_TECHNIQUE,
                                                        diffuseRandom,
                                                        surrogateProbability);
    result.Samples[SPECULAR_TECHNIQUE] = SampleTechnique(job,
                                                         frame,
                                                         selection.TriangleIndex,
                                                         target,
                                                         SPECULAR_TECHNIQUE,
                                                         specularRandom,
                                                         surrogateProbability);
    result.TriangleIndex = selection.TriangleIndex;
    result.ContributionScale = selection.ContributionWeight / emissiveCategoryProbability;
    result.HasSelection = true;
    outLight = result;
    return rhi::RasterStatus::Ready;
}

rhi::RasterStatus EvaluateReferenceResampledLightDensity(const ReferenceJob& job,
                                                         uint32 pixel,
                                                         uint32 sample,
                                                         uint32 depth,
                                                         const ReferenceReceiverFrame& frame,
                                                         float32 emissiveCategoryProbability,
                                                         uint32 triangle,
                                                         math::Vector3 directionWorld,
                                                         ReferenceTargetTotal& inoutTargetTotal,
                                                         float32& outDensity) noexcept
{
    if ((EvaluateReferenceEmissiveTriangleSelectionPmf(job.Scene, triangle) > 0) == false)
    {
        outDensity = 0;
        return rhi::RasterStatus::Ready;
    }
    EmitterTarget target;
    float32 surrogateProbability = 0;
    if (TryEvaluateEmitterTarget(job, frame, triangle, target) == false ||
        TryEvaluateSurrogate(job,
                             pixel,
                             sample,
                             depth,
                             frame,
                             emissiveCategoryProbability,
                             target,
                             inoutTargetTotal,
                             surrogateProbability) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const math::Vector2 densities = EvaluateReferenceLightTechniqueDensities(frame, target.Light, directionWorld);
    const float32 density = surrogateProbability * (densities.X + densities.Y);
    if (math::IsFinite(density) == false || density < 0)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    outDensity = density;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer::internal
