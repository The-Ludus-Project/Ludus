#pragma once
#include "hit_record.slang"
#include "projected_solid_angle_light.slang"

// Resampled importance sampling (RIS) of one emissive triangle per receiver, and the independent normalization
// estimate of the BSDF-hit MIS surrogate density. Candidates come from the scene's power-weighted triangle CDF
// (SelectEmissiveTriangle, p_src) and are weighted by p-hat / p_src; the reservoir streams them in index order.
// Thanks to Ishaan Shah, Aakash KT and P. J. Narayanan, "Combining Resampled Importance and Projected Solid Angle
// Samplings for Many Area Light Rendering", SIGGRAPH Asia 2023 Technical Communications,
// https://doi.org/10.1145/3610543.3626165, section 4 and Algorithm 1: M light candidates in a weighted reservoir,
// then one projected-solid-angle/LTC sample of the selected light; their naive per-candidate variant (section 3.1)
// is not used.
// Thanks to Chris Wyman, "Weighted Reservoir Sampling: Randomly Sampling Streams", Ray Tracing Gems II, chapter 22,
// 2021, for the single-sample, with-replacement streaming update, used through FoundationMath's
// weighted_reservoir.slang.
// RIS_CANDIDATE_COUNT, RIS_NORMALIZATION_CANDIDATE_COUNT and RIS_TARGET are Graph settings (main.lushader.json); the
// builder declares them before this source and validates their ranges against the maximums in hit_record.slang.
// RIS_TARGET selects how p-hat evaluates E_0 and E_1: RIS_TARGET_EXACT clips and integrates both polygons of every
// candidate; RIS_TARGET_FORM_FACTOR uses Hill and Heitz's approximation (TryApproximateTriangleLightIntegrals). Either
// target is positive wherever the triangle has area above the horizon and positive emission, so both estimate the
// same integral; the choice changes only variance and cost. The selected light's samples, densities and MIS
// coefficients always use the exact polygons.

// The settings clamped to the dimension table's capacity; the builder already rejects larger values.
static const uint RIS_ACTIVE_CANDIDATE_COUNT =
    RIS_CANDIDATE_COUNT < RIS_MAX_CANDIDATE_COUNT ? RIS_CANDIDATE_COUNT : RIS_MAX_CANDIDATE_COUNT;
static const uint RIS_TARGET_EXACT = 0;
static const uint RIS_TARGET_FORM_FACTOR = 1;
static const uint RIS_ACTIVE_NORMALIZATION_CANDIDATE_COUNT =
    RIS_NORMALIZATION_CANDIDATE_COUNT < RIS_MAX_NORMALIZATION_CANDIDATE_COUNT ? RIS_NORMALIZATION_CANDIDATE_COUNT
                                                                              : RIS_MAX_NORMALIZATION_CANDIDATE_COUNT;

/// One emissive triangle evaluated for one receiver and prepared for both light techniques.
struct EmissiveTriangleTarget
{
    TriangleData Triangle;
    TriangleLightPolygons Polygons;
    /// (c_0, c_1) from the exact integrals: Peters' MIS coefficients.
    float2 Coefficients;
    /// p-hat as RIS_TARGET evaluates it; Coefficients.x + Coefficients.y for RIS_TARGET_EXACT. Zero for a one-sided
    /// emitter seen from behind.
    float Target;
    /// Y(L), the luminance of the triangle's constant emission factor (zero from behind).
    float EmissionLuminance;
};

/// Load a scene triangle and the luminance of its constant emission factor towards the receiver. Returns false for
/// an invalid triangle or material.
bool TryLoadEmissiveTriangle(SceneContext scene,
                             ReceiverLightingFrame frame,
                             uint triangleIndex,
                             out TriangleData outTriangle,
                             out float outEmissionLuminance)
{
    outEmissionLuminance = 0;
    outTriangle = GetTriangle(scene, triangleIndex);
    if (outTriangle.IsValid == false)
        return false;
    MaterialSample material = GetMaterial(scene, outTriangle.MaterialIndex);
    if (material.IsValid == false)
        return false;
    // A planar triangle faces the receiver from every point on it, as EvaluateEmission tests at one point.
    if (material.IsEmissionTwoSided == true ||
        dot(outTriangle.GeometricNormalWorld, frame.PositionWorld - outTriangle.Position0World) > 0)
        outEmissionLuminance = ComputeLuminance(max(material.Emission, float3(0)));
    return true;
}

/// p-hat from RIS_TARGET_FORM_FACTOR's approximate integrals; the exact target is the coefficients' sum.
bool TryApproximateEmissiveTriangleTarget(ReceiverLightingFrame frame,
                                          TriangleData triangle,
                                          float emissionLuminance,
                                          out float outTarget)
{
    outTarget = 0;
    float2 integrals;
    if (TryApproximateTriangleLightIntegrals(frame,
                                             triangle.Position0World,
                                             triangle.Position1World,
                                             triangle.Position2World,
                                             integrals) == false)
        return false;
    float2 coefficients = ComputeLightTechniqueCoefficientsFromIntegrals(frame, integrals, emissionLuminance);
    outTarget = coefficients.x + coefficients.y;
    return IsFiniteBits(outTarget);
}

/// Evaluate p-hat for a scene triangle and prepare its exact polygons, for the selected triangle and BSDF hits. The
/// constant emission factor bounds textured emission from above, so the target needs no texture lookup. Returns false
/// for an invalid triangle or material or a nonfinite polygon or target.
// Thanks to the Slang authors, core module noinline attribute reference:
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html
// One copy of the two unrolled polygon preparations serves every site, which keeps the generated Metal artifact below
// its size limit.
[noinline] bool TryEvaluateEmissiveTriangleTarget(SceneContext scene,
                                                  ReceiverLightingFrame frame,
                                                  uint triangleIndex,
                                                  out EmissiveTriangleTarget outTarget) {
    outTarget = (EmissiveTriangleTarget)0;
    outTarget.Polygons.Diffuse = MakeEmptyProjectedSolidAnglePolygon(false);
    outTarget.Polygons.Specular.TransformedPolygon = MakeEmptyProjectedSolidAnglePolygon(false);
    outTarget.Polygons.Specular.Distribution = frame.Specular;
    if (TryLoadEmissiveTriangle(scene, frame, triangleIndex, outTarget.Triangle, outTarget.EmissionLuminance) == false)
        return false;
    if ((outTarget.EmissionLuminance > 0) == false)
        return true;
    if (TryPrepareTriangleLightPolygons(frame,
                                        outTarget.Triangle.Position0World,
                                        outTarget.Triangle.Position1World,
                                        outTarget.Triangle.Position2World,
                                        outTarget.Polygons) == false)
        return false;
    outTarget.Coefficients = ComputeLightTechniqueCoefficients(frame, outTarget.Polygons, outTarget.EmissionLuminance);
    if (RIS_TARGET == RIS_TARGET_EXACT)
    {
        outTarget.Target = outTarget.Coefficients.x + outTarget.Coefficients.y;
        return IsFiniteBits(outTarget.Target);
    }
    return IsFiniteBits(outTarget.Coefficients.x) == true && IsFiniteBits(outTarget.Coefficients.y) == true &&
           TryApproximateEmissiveTriangleTarget(frame,
                                                outTarget.Triangle,
                                                outTarget.EmissionLuminance,
                                                outTarget.Target);
}

/// Z-hat, the normalization estimate of the BSDF-hit surrogate density: an unbiased estimate of the target's total
/// sum_t p-hat(t). Computed at most once per invocation and only when a BSDF-hit or light-sample MIS weight needs it.
struct TargetTotalEstimate
{
    float Value;
    bool IsReady;
};

TargetTotalEstimate MakeTargetTotalEstimate()
{
    TargetTotalEstimate result;
    result.Value = 0;
    result.IsReady = false;
    return result;
}

/// p-hat alone, for the reservoir's and Z-hat's candidates: the same value TryEvaluateEmissiveTriangleTarget
/// returns, without preparing polygons under RIS_TARGET_FORM_FACTOR. Returns false as
/// TryEvaluateEmissiveTriangleTarget does.
[noinline] bool
TryEvaluateResamplingTarget(SceneContext scene, ReceiverLightingFrame frame, uint triangleIndex, out float outTarget) {
    outTarget = 0;
    if (RIS_TARGET == RIS_TARGET_EXACT)
    {
        EmissiveTriangleTarget target;
        bool isEvaluated = TryEvaluateEmissiveTriangleTarget(scene, frame, triangleIndex, target);
        outTarget = target.Target;
        return isEvaluated;
    }
    TriangleData triangle;
    float emissionLuminance;
    if (TryLoadEmissiveTriangle(scene, frame, triangleIndex, triangle, emissionLuminance) == false)
        return false;
    if ((emissionLuminance > 0) == false)
        return true;
    return TryApproximateEmissiveTriangleTarget(frame, triangle, emissionLuminance, outTarget);
}

/// Z-hat = (1/K) sum_k p-hat(t'_k) / p_src(t'_k) over K independent power-CDF draws from their own dimensions, so it is
/// independent of the reservoir and of the BSDF sample. A draw without a sample adds zero. Returns false for a
/// failed target evaluation or a nonfinite estimate.
bool TryEnsureTargetTotalEstimate(SceneContext scene,
                                  RayLaunch launch,
                                  ReceiverLightingFrame frame,
                                  inout TargetTotalEstimate inoutEstimate)
{
    if (inoutEstimate.IsReady == true)
        return true;
    float sum = 0;
    for (uint k = 0; k < RIS_ACTIVE_NORMALIZATION_CANDIDATE_COUNT; ++k)
    {
        EmissiveTriangleSelection candidate =
            SelectEmissiveTriangle(scene, launch, RIS_NORMALIZATION_DIMENSION_FIRST + k);
        if (candidate.HasSample == false)
            continue;
        float target;
        if (TryEvaluateResamplingTarget(scene, frame, candidate.TriangleIndex, target) == false)
            return false;
        sum += target / candidate.SelectionPmf;
    }
    float estimate = sum / float(RIS_ACTIVE_NORMALIZATION_CANDIDATE_COUNT);
    if (IsFiniteBits(estimate) == false)
        return false;
    inoutEstimate.Value = estimate;
    inoutEstimate.IsReady = true;
    return true;
}

/// The reservoir's selected triangle y with its target, source probability and contribution weight
/// W = w_sum / (M p-hat(y)).
struct ResampledTriangle
{
    uint TriangleIndex;
    float SelectionPmf;
    float Target;
    float ContributionWeight;
    bool HasSelection;
};

/// Stream RIS_CANDIDATE_COUNT power-CDF candidates through the reservoir. A zero weight sum, including a scene
/// without an emissive CDF, succeeds without a selection, which contributes zero. Returns false for a failed target
/// evaluation or reservoir update.
bool TryResampleEmissiveTriangle(SceneContext scene,
                                 RayLaunch launch,
                                 ReceiverLightingFrame frame,
                                 out ResampledTriangle outSelection)
{
    outSelection = (ResampledTriangle)0;
    outSelection.TriangleIndex = SCENE_INVALID_INDEX;
    WeightedReservoir reservoir = MakeWeightedReservoir();
    for (uint i = 0; i < RIS_ACTIVE_CANDIDATE_COUNT; ++i)
    {
        uint dimension = RIS_CANDIDATE_DIMENSION_FIRST + i * RIS_DIMENSIONS_PER_CANDIDATE;
        EmissiveTriangleSelection candidate = SelectEmissiveTriangle(scene, launch, dimension);
        if (candidate.HasSample == false)
            return true;
        float target;
        if (TryEvaluateResamplingTarget(scene, frame, candidate.TriangleIndex, target) == false)
            return false;
        bool isSelected;
        if (TryAddReservoirCandidate(reservoir,
                                     target / candidate.SelectionPmf,
                                     SampleSceneRandom(launch, dimension + 1),
                                     isSelected) == false)
            return false;
        if (isSelected == true)
        {
            outSelection.TriangleIndex = candidate.TriangleIndex;
            outSelection.SelectionPmf = candidate.SelectionPmf;
            outSelection.Target = target;
        }
    }
    if (HasReservoirSelection(reservoir) == false)
        return true;
    if (TryComputeReservoirContributionWeight(reservoir, outSelection.Target, outSelection.ContributionWeight) == false)
        return false;
    outSelection.HasSelection = true;
    return true;
}
