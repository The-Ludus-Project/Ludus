#pragma once

// Scene-independent pieces of RT1's ReferenceLightSampling::ResampledProjectedSolidAngle estimator (sampler version 2):
// the receiver frame and LTC, the triangle's two light techniques, Peters' clamped optimal MIS weights, the surrogate
// light density and the triangle-plane intersection. Each function ports its C++ counterpart in
// src/reference_light_sampling.cpp step for step; renderer_reference.slang adds the scene-dependent target, Z-hat and
// reservoir. The includer includes FoundationMath's projected_solid_angle.slang and linearly_transformed_cosine.slang
// and the renderer's ltc_table.slang (with its table binding) first. The estimator is
// docs/architecture/many-area-light-sampling-reference-review.md; the ris-projected-solid-angle template's
// projected_solid_angle_light.slang implements the same pieces for its one bounce.
//
// Thanks to Ishaan Shah, Aakash KT and P. J. Narayanan, "Combining Resampled Importance and Projected Solid Angle
// Samplings for Many Area Light Rendering", SIGGRAPH Asia 2023 Technical Communications,
// https://doi.org/10.1145/3610543.3626165, sections 3 and 4, and to Christoph Peters, "BRDF Importance Sampling for
// Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021, https://doi.org/10.1145/3450626.3459672,
// sections 4.1-4.3. See src/internal/reference_light_sampling.h for the adopted ideas and departures.

static const float3 REFERENCE_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
// Peters 2021, section 4.2: the clamp keeps the diffuse technique, and so the target, positive above the horizon.
static const float REFERENCE_MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO = 0.01;
// Schlick's dielectric F0 of the frozen reflection model.
static const float REFERENCE_DIELECTRIC_SPECULAR_REFLECTANCE = 0.04;
// Above this |n.z| the frame's helper axis switches from +z to +y, as SampleBsdfDirection's frame does.
static const float REFERENCE_FRAME_HELPER_AXIS_LIMIT = 0.999;
// Peters' visibility estimate v; a frozen constant of sampler version 2.
static const float REFERENCE_LIGHT_TECHNIQUE_VISIBILITY = 0.5;
static const uint REFERENCE_DIFFUSE_TECHNIQUE = 0;
static const uint REFERENCE_SPECULAR_TECHNIQUE = 1;

float ComputeReferenceLuminance(float3 value)
{
    return dot(value, REFERENCE_LUMINANCE_WEIGHTS);
}

// The receiver's shading frame (+z the unit shading normal) and its two light techniques: ReferenceReceiverFrame.
struct ReferenceReceiverFrame
{
    float3 PositionWorld;
    float3 TangentWorld;
    float3 BitangentWorld;
    float3 NormalWorld;
    // M_1: the specular LTC; unused when HasSpecular is false.
    LinearlyTransformedCosine Specular;
    bool HasSpecular;
    // Y(a_0) and Y(a_1).
    float DiffuseAlbedoLuminance;
    float SpecularAlbedoLuminance;
};

float3 TransformReferenceWorldToTangent(ReferenceReceiverFrame frame, float3 vectorWorld)
{
    return float3(dot(vectorWorld, frame.TangentWorld),
                  dot(vectorWorld, frame.BitangentWorld),
                  dot(vectorWorld, frame.NormalWorld));
}

float3 TransformReferenceTangentToWorld(ReferenceReceiverFrame frame, float3 vectorTangent)
{
    return frame.TangentWorld * vectorTangent.x + frame.BitangentWorld * vectorTangent.y +
           frame.NormalWorld * vectorTangent.z;
}

// MakeReferenceReceiverFrame: unit normalWorld and viewWorld; the frozen BSDF's base color, metallic and perceptual
// roughness.
ReferenceReceiverFrame MakeReferenceReceiverFrame(float3 positionWorld,
                                                  float3 normalWorld,
                                                  float3 viewWorld,
                                                  float3 baseColor,
                                                  float metallic,
                                                  float roughness)
{
    ReferenceReceiverFrame frame;
    frame.PositionWorld = positionWorld;
    frame.NormalWorld = normalWorld;
    float3 helperWorld = abs(normalWorld.z) < REFERENCE_FRAME_HELPER_AXIS_LIMIT ? float3(0, 0, 1) : float3(0, 1, 0);
    frame.TangentWorld = NormalizeOrZero(cross(helperWorld, normalWorld));
    frame.BitangentWorld = cross(normalWorld, frame.TangentWorld);
    float3 specularReflectance =
        float3(REFERENCE_DIELECTRIC_SPECULAR_REFLECTANCE) * (1 - metallic) + baseColor * metallic;
    // The table's F0 slice follows the luminance of this receiver's F0 (Peters 2021, section 4.1).
    SpecularLtc specular =
        LookupSpecularLtc(roughness, TransformReferenceWorldToTangent(frame, viewWorld), specularReflectance);
    frame.HasSpecular = TryMakeLinearlyTransformedCosine(specular.InverseTransform, frame.Specular);
    frame.SpecularAlbedoLuminance =
        frame.HasSpecular == true
            ? max(ComputeReferenceLuminance(ComputeSpecularLtcAlbedo(specular, specularReflectance)), 0)
            : 0;
    float3 diffuseAlbedo = max(baseColor * (1 - metallic) * (float3(1) - specularReflectance),
                               float3(REFERENCE_MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO));
    frame.DiffuseAlbedoLuminance = ComputeReferenceLuminance(diffuseAlbedo);
    return frame;
}

// ReferenceTriangleLight: one emissive triangle prepared for both light techniques at one receiver.
struct ReferenceTriangleLight
{
    ProjectedSolidAnglePolygon Diffuse;
    LinearlyTransformedCosinePolygon Specular;
    float DiffuseIntegral;
    float SpecularIntegral;
};

ReferenceTriangleLight MakeEmptyReferenceTriangleLight(ReferenceReceiverFrame frame)
{
    ReferenceTriangleLight result;
    result.Diffuse = MakeEmptyProjectedSolidAnglePolygon(false);
    result.Specular.TransformedPolygon = MakeEmptyProjectedSolidAnglePolygon(false);
    result.Specular.Distribution = frame.Specular;
    result.DiffuseIntegral = 0;
    result.SpecularIntegral = 0;
    return result;
}

// TryPrepareReferenceTriangleLight: false for a nonfinite vertex in the shading frame or after the LTC transform; an
// empty or edge-on clip succeeds with a zero integral, which disables that technique.
bool TryPrepareReferenceTriangleLight(ReferenceReceiverFrame frame,
                                      float3 position0World,
                                      float3 position1World,
                                      float3 position2World,
                                      out ReferenceTriangleLight outLight)
{
    outLight = MakeEmptyReferenceTriangleLight(frame);
    ProjectedPolygonInput polygon;
    polygon.VertexCount = 3;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        polygon.PositionsTangent[i] = float3(0);
    }
    polygon.PositionsTangent[0] = TransformReferenceWorldToTangent(frame, position0World - frame.PositionWorld);
    polygon.PositionsTangent[1] = TransformReferenceWorldToTangent(frame, position1World - frame.PositionWorld);
    polygon.PositionsTangent[2] = TransformReferenceWorldToTangent(frame, position2World - frame.PositionWorld);
    if (TryPrepareProjectedSolidAnglePolygon(polygon, outLight.Diffuse) == false)
        return false;
    outLight.DiffuseIntegral = outLight.Diffuse.ProjectedSolidAngle / PI;
    if (frame.HasSpecular == false)
        return true;
    if (TryPrepareLinearlyTransformedCosinePolygon(polygon, frame.Specular, outLight.Specular) == false)
        return false;
    outLight.SpecularIntegral = GetLinearlyTransformedCosinePolygonIntegral(outLight.Specular);
    return true;
}

// EvaluateReferenceLightTechniqueDensities: (p_0, p_1) for a unit world direction through the triangle.
float2 EvaluateReferenceLightTechniqueDensities(ReferenceReceiverFrame frame,
                                                ReferenceTriangleLight light,
                                                float3 directionWorld)
{
    float3 directionTangent = TransformReferenceWorldToTangent(frame, directionWorld);
    return float2(EvaluateProjectedSolidAnglePolygonDensity(light.Diffuse, directionTangent),
                  EvaluateLinearlyTransformedCosinePolygonDensity(light.Specular, directionTangent));
}

// ComputeReferenceLightTechniqueWeights: Peters' clamped optimal MIS weights (w_0, w_1), which sum to one wherever a
// density is positive.
float2 ComputeReferenceLightTechniqueWeights(float2 coefficients, float2 densities, float visibility)
{
    float densitySum = densities.x + densities.y;
    if ((densitySum > 0) == false)
        return float2(0);
    float2 balance = float2(densities.x / densitySum, densities.y / densitySum);
    float2 weightedDensities = coefficients * densities;
    float weightedSum = weightedDensities.x + weightedDensities.y;
    float2 weighted =
        weightedSum > 0 ? float2(weightedDensities.x / weightedSum, weightedDensities.y / weightedSum) : balance;
    return weighted * visibility + balance * (1 - visibility);
}

// EvaluateReferenceSurrogateProbability: P-tilde(t) = P_E min(p-hat(t) / Z-hat, min(1, M p_src(t))), with the
// inclusion bound when Z-hat is zero.
float EvaluateReferenceSurrogateProbability(float emissiveCategoryProbability,
                                            float target,
                                            float sourcePmf,
                                            float targetTotal,
                                            uint candidateCount)
{
    if ((emissiveCategoryProbability > 0) == false || (target > 0) == false || (sourcePmf > 0) == false)
        return 0;
    float inclusionBound = min(1.0, float(candidateCount) * sourcePmf);
    // Compare before dividing, so a tiny Z-hat never overflows the ratio.
    float resampledProbability =
        targetTotal > 0 && target < inclusionBound * targetTotal ? target / targetTotal : inclusionBound;
    return emissiveCategoryProbability * resampledProbability;
}

// TryIntersectReferenceTrianglePlane: outBarycentric weights position1World (x) and position2World (y). False, with
// zero outputs, for a direction parallel to the plane, a plane behind the origin or a point outside the triangle.
bool TryIntersectReferenceTrianglePlane(float3 originWorld,
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
    float2 barycentric = float2(dot(normalWorld, cross(offsetWorld, edge2World)) / normalLengthSquared,
                                dot(normalWorld, cross(edge1World, offsetWorld)) / normalLengthSquared);
    if (IsFiniteBits(barycentric.x) == false || IsFiniteBits(barycentric.y) == false || barycentric.x < 0 ||
        barycentric.y < 0 || barycentric.x + barycentric.y > 1)
        return false;
    outDistanceWorld = distanceWorld;
    outBarycentric = barycentric;
    return true;
}
