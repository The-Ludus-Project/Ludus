#pragma once
// Scene-independent pieces of the ris-projected-solid-angle estimator: the per-receiver frame and LTC, the
// per-triangle target, the light techniques' MIS weights and the surrogate light density for BSDF-hit MIS.
// The estimator and its decisions are docs/architecture/many-area-light-sampling-reference-review.md, sections
// "Proposed estimator" and "Decision: BSDF-hit MIS".
// The graph wrapper includes FoundationMath's projected_solid_angle.slang, linearly_transformed_cosine.slang and
// shading_normal.slang and the renderer's ltc_table.slang before this source, because the technique declares
// scene_interface.polygonal_light_sampling.
//
// Thanks to Ishaan Shah, Aakash KT and P. J. Narayanan, "Combining Resampled Importance and Projected Solid Angle
// Samplings for Many Area Light Rendering", SIGGRAPH Asia 2023 Technical Communications,
// https://doi.org/10.1145/3610543.3626165, sections 3 and 4 and Algorithm 1: lights, not points, are resampled,
// with the unshadowed LTC shading of the whole light as the target. Their single-lobe limitation (section 6) is
// addressed with both of Peters' techniques below.
// Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4),
// Article 140, 2021, https://doi.org/10.1145/3450626.3459672: section 4.1 (LTC sampling, which an empty LTC-space
// clip disables), section 4.2 (the weighted balance heuristic with coefficients c_j = L_e a_j E_j, and diffuse albedo
// clamped to 0.01 per channel), section 4.3 (clamped optimal MIS, the blend with the balance heuristic by the
// visibility estimate v) and section 6.1 (the receiver's shading normal clipped to the hemisphere of the outgoing
// direction by FoundationMath's ClipShadingNormalToOutgoingHemisphere, which shade.slang applies before
// MakeReceiverLightingFrame). Implemented from the paper's text.
// Departures, all named in docs/development/render-graph-authoring.md:
// - As the review specifies, the coefficients are reduced to BT.709 luminance, one scalar weight per technique, where
//   Peters weights each color channel.
// - Section 6.1 gives no margin for the clipped shading normal. Ludus keeps it SHADING_NORMAL_MINIMUM_OUTGOING_COSINE
//   (0.01) inside the hemisphere, because the BSDF is zero for an outgoing direction on the shading horizon.
// - Section 6.1 does not discuss back faces. shade.slang clips front faces only, so a one-sided surface seen from
//   behind stays black, as in the other templates, instead of being lit through the surface.
// Thanks to Stephen Hill and Eric Heitz, "Real-Time Area Lighting: a Journey from Research to Production", SIGGRAPH
// 2016 Advances in Real-Time Rendering course, https://advances.realtimerendering.com/s2016/, slides 77 and 90-102:
// the approximate target evaluates their vector form factor and clipped proxy sphere (FoundationMath's
// TryApproximateClampedCosinePolygonIntegral) instead of clipping and integrating both polygons, as Shah et al. do for
// speed. A defensive term keeps it positive wherever the triangle has area above the horizon.

static const float3 BT709_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
// Peters 2021, section 4.2: the diffuse technique alone covers the whole polygon above the horizon.
static const float MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO = 0.01;
// The dielectric Schlick F0 of the standard BSDF (Evaluate in renderer_gpu_preview.slang).
static const float DIELECTRIC_SPECULAR_REFLECTANCE = 0.04;
// SampleStandardBsdf's switch between helper axes for its tangent frame.
static const float FRAME_HELPER_AXIS_LIMIT = 0.999;
// The approximate target's diffuse integral is at least this fraction of the unit-direction triple product, a positive
// proxy of the unclipped solid angle, whenever a vertex lies above the horizon: Hill and Heitz's proxy sphere can lie
// below the horizon while part of the triangle is above it, and the RIS support invariant needs p-hat > 0 there.
static const float APPROXIMATE_TARGET_DEFENSIVE_FRACTION = 0.015625;

float ComputeLuminance(float3 value)
{
    return dot(value, BT709_LUMINANCE_WEIGHTS);
}

/// The receiver's shading frame (+z the unit shading normal) and its two light techniques. These guide sampling and
/// weights only; the BSDF is always evaluated exactly.
struct ReceiverLightingFrame
{
    float3 PositionWorld;
    float3 TangentWorld;
    float3 BitangentWorld;
    float3 NormalWorld;
    /// M_1: the specular LTC; the identity when HasSpecular is false.
    LinearlyTransformedCosine Specular;
    bool HasSpecular;
    /// Y(a_0): the clamped diffuse albedo's luminance.
    float DiffuseAlbedoLuminance;
    /// Y(a_1): the specular LTC's Fresnel-weighted albedo luminance.
    float SpecularAlbedoLuminance;
};

float3 TransformWorldToTangent(ReceiverLightingFrame frame, float3 vectorWorld)
{
    return float3(dot(vectorWorld, frame.TangentWorld),
                  dot(vectorWorld, frame.BitangentWorld),
                  dot(vectorWorld, frame.NormalWorld));
}

float3 TransformTangentToWorld(ReceiverLightingFrame frame, float3 vectorTangent)
{
    return frame.TangentWorld * vectorTangent.x + frame.BitangentWorld * vectorTangent.y +
           frame.NormalWorld * vectorTangent.z;
}

/// Build the frame and both techniques once per receiver. normalWorld, already clipped by FoundationMath's
/// ClipShadingNormalToOutgoingHemisphere for a front face, and viewDirectionWorld are unit vectors;
/// roughness is the standard BSDF's perceptual roughness. The diffuse albedo a_0 approximates the Fresnel-weighted
/// Lambert lobe with Schlick's reflectance at normal incidence, baseColor (1 - metallic) (1 - F0), clamped per
/// channel to MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO; a failed LTC construction disables the specular technique.
ReceiverLightingFrame MakeReceiverLightingFrame(float3 positionWorld,
                                                float3 normalWorld,
                                                float3 viewDirectionWorld,
                                                float3 baseColor,
                                                float metallic,
                                                float roughness)
{
    ReceiverLightingFrame frame;
    frame.PositionWorld = positionWorld;
    frame.NormalWorld = normalWorld;
    float3 helperWorld = abs(normalWorld.z) < FRAME_HELPER_AXIS_LIMIT ? float3(0, 0, 1) : float3(0, 1, 0);
    frame.TangentWorld = NormalizeOrZero(cross(helperWorld, normalWorld));
    frame.BitangentWorld = cross(normalWorld, frame.TangentWorld);
    float3 specularReflectance = DIELECTRIC_SPECULAR_REFLECTANCE * (1 - metallic) + baseColor * metallic;
    // The table's F0 slice follows the luminance of this receiver's F0 (Peters 2021, section 4.1).
    SpecularLtc specular =
        LookupSpecularLtc(roughness, TransformWorldToTangent(frame, viewDirectionWorld), specularReflectance);
    frame.HasSpecular = TryMakeLinearlyTransformedCosine(specular.InverseTransform, frame.Specular);
    frame.SpecularAlbedoLuminance =
        frame.HasSpecular == true ? max(ComputeLuminance(ComputeSpecularLtcAlbedo(specular, specularReflectance)), 0)
                                  : 0;
    float3 diffuseAlbedo =
        max(baseColor * (1 - metallic) * (1 - specularReflectance), float3(MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO));
    frame.DiffuseAlbedoLuminance = ComputeLuminance(diffuseAlbedo);
    return frame;
}

/// One emissive triangle prepared for both techniques at one receiver.
struct TriangleLightPolygons
{
    /// The triangle in the shading frame, clipped to z >= 0 (technique 0, density p_0 = cos(theta) / (pi E_0)).
    ProjectedSolidAnglePolygon Diffuse;
    /// The triangle transformed by M_1^-1 and clipped there (technique 1, density p_1 = p_M1 / E_1).
    LinearlyTransformedCosinePolygon Specular;
    /// E_0: the clipped projected solid angle over pi.
    float DiffuseIntegral;
    /// E_1: the LTC integral; zero when the LTC-space clip is empty or the receiver has no specular LTC.
    float SpecularIntegral;
};

/// Prepare a triangle's two polygons. Returns false for a nonfinite vertex in the shading frame or after the LTC
/// transform; an empty or edge-on clip succeeds with a zero integral, which disables that technique.
/// The triangle relative to the receiver, in its shading frame.
ProjectedPolygonInput MakeTrianglePolygonInput(ReceiverLightingFrame frame,
                                               float3 position0World,
                                               float3 position1World,
                                               float3 position2World)
{
    ProjectedPolygonInput polygon;
    polygon.VertexCount = 3;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        polygon.PositionsTangent[i] = float3(0);
    }
    polygon.PositionsTangent[0] = TransformWorldToTangent(frame, position0World - frame.PositionWorld);
    polygon.PositionsTangent[1] = TransformWorldToTangent(frame, position1World - frame.PositionWorld);
    polygon.PositionsTangent[2] = TransformWorldToTangent(frame, position2World - frame.PositionWorld);
    return polygon;
}

bool TryPrepareTriangleLightPolygons(ReceiverLightingFrame frame,
                                     float3 position0World,
                                     float3 position1World,
                                     float3 position2World,
                                     out TriangleLightPolygons outPolygons)
{
    outPolygons.Diffuse = MakeEmptyProjectedSolidAnglePolygon(false);
    outPolygons.Specular.TransformedPolygon = MakeEmptyProjectedSolidAnglePolygon(false);
    outPolygons.Specular.Distribution = frame.Specular;
    outPolygons.DiffuseIntegral = 0;
    outPolygons.SpecularIntegral = 0;
    ProjectedPolygonInput polygon = MakeTrianglePolygonInput(frame, position0World, position1World, position2World);
    if (TryPrepareProjectedSolidAnglePolygon(polygon, outPolygons.Diffuse) == false)
        return false;
    outPolygons.DiffuseIntegral = outPolygons.Diffuse.ProjectedSolidAngle / PI;
    if (frame.HasSpecular == false)
        return true;
    if (TryPrepareLinearlyTransformedCosinePolygon(polygon, frame.Specular, outPolygons.Specular) == false)
        return false;
    outPolygons.SpecularIntegral = GetLinearlyTransformedCosinePolygonIntegral(outPolygons.Specular);
    return true;
}

/// Approximate (E_0, E_1) without clipping or preparing either polygon: Hill and Heitz's clipped proxy-sphere form
/// factor of the triangle and of its LTC transform. E_0 is at least APPROXIMATE_TARGET_DEFENSIVE_FRACTION of the
/// unit-direction triple product when a vertex lies above the horizon, so it is positive wherever the exact E_0 is,
/// and zero when no vertex does. Returns false for a nonfinite vertex in the shading frame or after the LTC transform.
bool TryApproximateTriangleLightIntegrals(ReceiverLightingFrame frame,
                                          float3 position0World,
                                          float3 position1World,
                                          float3 position2World,
                                          out float2 outIntegrals)
{
    outIntegrals = float2(0);
    ProjectedPolygonInput polygon = MakeTrianglePolygonInput(frame, position0World, position1World, position2World);
    ApproximatePolygonIntegral diffuse;
    if (TryApproximateClampedCosinePolygonIntegral(polygon, diffuse) == false)
        return false;
    bool hasVertexAboveHorizon =
        polygon.PositionsTangent[0].z > 0 || polygon.PositionsTangent[1].z > 0 || polygon.PositionsTangent[2].z > 0;
    float2 integrals = float2(0);
    if (hasVertexAboveHorizon == true)
        integrals.x = max(diffuse.Integral, APPROXIMATE_TARGET_DEFENSIVE_FRACTION * diffuse.UnitTripleProduct);
    if (frame.HasSpecular == true)
    {
        ApproximatePolygonIntegral specular;
        if (TryApproximateLinearlyTransformedCosinePolygonIntegral(polygon, frame.Specular, specular) == false)
            return false;
        integrals.y = specular.Integral;
    }
    outIntegrals = integrals;
    return true;
}

/// Peters' coefficients c_t = Y(L) Y(a_t) E_t for both techniques from (E_0, E_1); their sum is the resampling target
/// p-hat. emissionLuminance is Y of the triangle's constant emission factor, zero for a one-sided emitter seen from
/// behind.
float2
ComputeLightTechniqueCoefficientsFromIntegrals(ReceiverLightingFrame frame, float2 integrals, float emissionLuminance)
{
    return emissionLuminance *
           float2(frame.DiffuseAlbedoLuminance * integrals.x, frame.SpecularAlbedoLuminance * integrals.y);
}

/// Peters' coefficients for a prepared triangle's exact integrals.
float2
ComputeLightTechniqueCoefficients(ReceiverLightingFrame frame, TriangleLightPolygons polygons, float emissionLuminance)
{
    return ComputeLightTechniqueCoefficientsFromIntegrals(frame,
                                                          float2(polygons.DiffuseIntegral, polygons.SpecularIntegral),
                                                          emissionLuminance);
}

/// The two techniques' solid-angle densities (p_0, p_1) for a unit direction in the shading frame that passes through
/// the triangle; zero for a technique that cannot sample it. No containment test is performed.
float2 EvaluateLightTechniqueDensities(TriangleLightPolygons polygons, float3 directionTangent)
{
    return float2(EvaluateProjectedSolidAnglePolygonDensity(polygons.Diffuse, directionTangent),
                  EvaluateLinearlyTransformedCosinePolygonDensity(polygons.Specular, directionTangent));
}

/// Peters' clamped optimal MIS weights (w_0, w_1) for one sample per technique: visibility times the weighted balance
/// heuristic c_j p_j / sum_k c_k p_k, plus (1 - visibility) times the balance heuristic p_j / sum_k p_k. Inputs are
/// finite and nonnegative and visibility lies in [0, 1]. The weights sum to one wherever a density is positive, and a
/// technique with zero density gets zero weight; both are zero where neither technique samples. Where every c_j p_j is
/// zero, the weighted part falls back to the balance heuristic.
float2 ComputeClampedOptimalMisWeights(float2 coefficients, float2 densities, float visibility)
{
    float densitySum = densities.x + densities.y;
    if ((densitySum > 0) == false)
        return float2(0);
    float2 balance = densities / densitySum;
    float2 weightedDensities = coefficients * densities;
    float weightedSum = weightedDensities.x + weightedDensities.y;
    float2 weighted = weightedSum > 0 ? weightedDensities / weightedSum : balance;
    return weighted * visibility + balance * (1 - visibility);
}

/// Power heuristic (exponent two) weight of the strategy that drew a sample against one other strategy. A nonfinite or
/// nonpositive sampled density returns 0; a zero other density returns 1 and a nonfinite one returns 0. For finite
/// positive densities ComputePowerHeuristicWeight(p, q) + ComputePowerHeuristicWeight(q, p) == 1.
float ComputePowerHeuristicWeight(float sampledDensity, float otherDensity)
{
    if (IsFiniteBits(sampledDensity) == false || (sampledDensity > 0) == false)
        return 0;
    if ((otherDensity > 0) == false)
        return 1;
    if (IsFiniteBits(otherDensity) == false)
        return 0;
    // Scale both by the larger density, so squares neither overflow nor flush both terms to zero.
    float largerDensity = max(sampledDensity, otherDensity);
    float sampled = sampledDensity / largerDensity;
    float other = otherDensity / largerDensity;
    return sampled * sampled / (sampled * sampled + other * other);
}

/// The surrogate probability P-tilde(t) = P_E min(p-hat(t) / Z-hat, min(1, M p_src(t))) that the resampled light
/// strategy proposes triangle t; the BSDF-hit MIS density is P-tilde(t) (p_0 + p_1). normalizationEstimate is Z-hat,
/// the mean of p-hat / p_src over independent normalization candidates. When Z-hat is zero and p-hat is positive the
/// ratio is infinite, so the inclusion bound applies; a zero target, source probability or emissive category
/// probability returns zero.
float EvaluateSurrogateTriangleProbability(float emissiveCategoryProbability,
                                           float target,
                                           float sourcePmf,
                                           float normalizationEstimate,
                                           uint candidateCount)
{
    if ((emissiveCategoryProbability > 0) == false || (target > 0) == false || (sourcePmf > 0) == false)
        return 0;
    float inclusionBound = min(1.0, float(candidateCount) * sourcePmf);
    // Compare before dividing, so a tiny Z-hat never overflows the ratio.
    float resampledProbability = normalizationEstimate > 0 && target < inclusionBound * normalizationEstimate
                                     ? target / normalizationEstimate
                                     : inclusionBound;
    return emissiveCategoryProbability * resampledProbability;
}

/// Map a direction from originWorld to the point where it meets the triangle. outBarycentric weights position1World
/// (x) and position2World (y), as GetSurfaceHit. Returns false, with zero outputs, for a direction parallel to the
/// plane, a plane behind the origin or a point outside the triangle in FP32: such a sample contributes zero.
bool TryIntersectTrianglePlane(float3 originWorld,
                               float3 directionWorld,
                               float3 position0World,
                               float3 position1World,
                               float3 position2World,
                               out float outDistanceWorld,
                               out float2 outBarycentric)
{
    outDistanceWorld = 0;
    outBarycentric = float2(0);
    float3 edge1World = position1World - position0World;
    float3 edge2World = position2World - position0World;
    float3 normalWorld = cross(edge1World, edge2World);
    float normalLengthSquared = dot(normalWorld, normalWorld);
    float denominator = dot(normalWorld, directionWorld);
    if ((normalLengthSquared > 0) == false || (denominator > 0 || denominator < 0) == false)
        return false;
    float distanceWorld = dot(normalWorld, position0World - originWorld) / denominator;
    if (IsFiniteBits(distanceWorld) == false || (distanceWorld > 0) == false)
        return false;
    float3 offsetWorld = originWorld + directionWorld * distanceWorld - position0World;
    float2 barycentric =
        float2(dot(normalWorld, cross(offsetWorld, edge2World)), dot(normalWorld, cross(edge1World, offsetWorld))) /
        normalLengthSquared;
    if (IsFiniteBits(barycentric.x) == false || IsFiniteBits(barycentric.y) == false || any(barycentric < 0) == true ||
        barycentric.x + barycentric.y > 1)
        return false;
    outDistanceWorld = distanceWorld;
    outBarycentric = barycentric;
    return true;
}
