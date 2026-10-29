#pragma once
// Scene-independent pieces of the polygonal-light-sampling template: the receiver's two sampling techniques, one
// emissive triangle prepared for both, one sample of either technique and the four estimators' weights. shade.slang
// adds the scene (emitters, emission at the sampled point and shadow rays); the CPU oracle in
// tests/polygonal_light_sampling_oracle.cpp implements the same estimators, and
// tests/polygonal_light_sampling_estimators.slang runs this file against it.
// The graph wrapper includes FoundationMath's projected_solid_angle.slang and linearly_transformed_cosine.slang and
// the renderer's ltc_table.slang before this source, because the technique declares
// scene_interface.polygonal_light_sampling; EvaluateStandardBsdf comes from scene_shader.slang.
//
// Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4),
// Article 140, 2021, https://doi.org/10.1145/3450626.3459672. Implemented from the paper's text; only FoundationMath's
// projected solid angle sampler is ported from his BSD-licensed polygon_sampling.glsl. By section:
// - 3 and supplement A (projected solid angle sampling, technique 0): FoundationMath's
//   TrySampleProjectedSolidAnglePolygon, with density p_0 = cos(theta) / (pi E_0).
// - 4.1 (LTC sampling, technique 1): the polygon transformed by M_1^-1 and clipped only in that space; an empty clip
//   disables the technique. Density p_1 = p_M1 / E_1 (Equations 10 and 12).
// - 4.2 (weighted balance heuristic): c_j = L_e a_j E_j per color channel, a_0 clamped to 0.01 per channel so the
//   diffuse technique covers the whole polygon above the horizon, and a_1 the LTC table's Fresnel-weighted albedo.
// - 4.3 (clamped optimal MIS): v times the weighted balance heuristic plus (1 - v) times the balance heuristic. With
//   v = 1 it is section 4.2's weighted balance heuristic, and with v = 0 Veach's balance heuristic (Fig. 10b).
// Departures, all named in docs/development/render-graph-authoring.md:
// - Polygons are the scene's emissive triangles; a quad light is two triangles, each with its own two samples.
// - The diffuse albedo a_0 approximates Ludus's Fresnel-weighted Lambert lobe with Schlick's reflectance at normal
//   incidence, baseColor (1 - metallic) (1 - F0), where Peters uses the Frostbite BRDF's diffuse albedo (section 6.1).
// - Shading normals are not clipped to the hemisphere of the outgoing direction (section 6.1); the BSDF is zero there.
// - A sample whose direction misses the triangle in FP32 contributes zero (compare section 3.7).

// Peters 2021, section 4.2: the diffuse technique alone covers the whole polygon above the horizon.
static const float MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO = 0.01;
// The dielectric Schlick F0 of the standard BSDF (Evaluate in renderer_gpu_preview.slang).
static const float DIELECTRIC_SPECULAR_REFLECTANCE = 0.04;
// SampleStandardBsdf's switch between helper axes for its tangent frame.
static const float FRAME_HELPER_AXIS_LIMIT = 0.999;
static const uint COLOR_CHANNEL_COUNT = 3;

// The two sampling techniques of section 4.2: index j of p_j, a_j and c_j.
static const uint PROJECTED_SOLID_ANGLE_TECHNIQUE = 0;
static const uint LTC_TECHNIQUE = 1;
static const uint POLYGONAL_LIGHT_TECHNIQUE_COUNT = 2;

// The template's four estimators, one per output and in output order. Both MIS estimators take one sample per
// technique (section 4.2); the single-technique estimators take only their own.
static const uint CLAMPED_OPTIMAL_MIS_ESTIMATOR = 0;
static const uint BALANCE_HEURISTIC_ESTIMATOR = 1;
static const uint PROJECTED_SOLID_ANGLE_ESTIMATOR = 2;
static const uint LTC_ESTIMATOR = 3;
static const uint POLYGONAL_LIGHT_ESTIMATOR_COUNT = 4;

/// The receiver's shading frame (+z the unit shading normal) and its two techniques' LTCs and albedos. These guide
/// sampling and weights only; the BSDF is always evaluated exactly.
struct PolygonalLightReceiver
{
    float3 PositionWorld;
    float3 TangentWorld;
    float3 BitangentWorld;
    float3 NormalWorld;
    /// M_1: the specular LTC; the identity when HasSpecular is false. M_0 is the identity (section 4.2).
    LinearlyTransformedCosine Specular;
    bool HasSpecular;
    /// a_0 per channel, clamped to MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO.
    float3 DiffuseAlbedo;
    /// a_1 per channel: the LTC table's albedo with Schlick's Fresnel; zero without a specular LTC.
    float3 SpecularAlbedo;
};

float3 TransformWorldToTangent(PolygonalLightReceiver receiver, float3 vectorWorld)
{
    return float3(dot(vectorWorld, receiver.TangentWorld),
                  dot(vectorWorld, receiver.BitangentWorld),
                  dot(vectorWorld, receiver.NormalWorld));
}

float3 TransformTangentToWorld(PolygonalLightReceiver receiver, float3 vectorTangent)
{
    return receiver.TangentWorld * vectorTangent.x + receiver.BitangentWorld * vectorTangent.y +
           receiver.NormalWorld * vectorTangent.z;
}

/// Build the frame and both techniques once per receiver. normalWorld and viewDirectionWorld are unit vectors;
/// roughness is the standard BSDF's perceptual roughness. The specular LTC comes from the table slice of this
/// receiver's F0 (section 4.1); a failed LTC construction disables the specular technique.
PolygonalLightReceiver MakePolygonalLightReceiver(float3 positionWorld,
                                                  float3 normalWorld,
                                                  float3 viewDirectionWorld,
                                                  float3 baseColor,
                                                  float metallic,
                                                  float roughness)
{
    PolygonalLightReceiver receiver;
    receiver.PositionWorld = positionWorld;
    receiver.NormalWorld = normalWorld;
    float3 helperWorld = abs(normalWorld.z) < FRAME_HELPER_AXIS_LIMIT ? float3(0, 0, 1) : float3(0, 1, 0);
    receiver.TangentWorld = NormalizeOrZero(cross(helperWorld, normalWorld));
    receiver.BitangentWorld = cross(normalWorld, receiver.TangentWorld);
    float3 specularReflectance = DIELECTRIC_SPECULAR_REFLECTANCE * (1 - metallic) + baseColor * metallic;
    SpecularLtc specular =
        LookupSpecularLtc(roughness, TransformWorldToTangent(receiver, viewDirectionWorld), specularReflectance);
    receiver.HasSpecular = TryMakeLinearlyTransformedCosine(specular.InverseTransform, receiver.Specular);
    receiver.SpecularAlbedo = receiver.HasSpecular == true
                                  ? max(ComputeSpecularLtcAlbedo(specular, specularReflectance), float3(0))
                                  : float3(0);
    receiver.DiffuseAlbedo =
        max(baseColor * (1 - metallic) * (1 - specularReflectance), float3(MINIMUM_DIFFUSE_TECHNIQUE_ALBEDO));
    return receiver;
}

// TryPreparePolygonalLight and TrySamplePolygonalLight are not inlined, so every call site shares one copy of
// FoundationMath's unrolled polygon preparation and sampling.

/// One emissive triangle prepared for both techniques at one receiver.
struct PolygonalLight
{
    float3 Position0World;
    float3 Position1World;
    float3 Position2World;
    /// The triangle in the shading frame, clipped to z >= 0 (technique 0).
    ProjectedSolidAnglePolygon Diffuse;
    /// The triangle transformed by M_1^-1 and clipped there (technique 1).
    LinearlyTransformedCosinePolygon Specular;
    /// E_0: the clipped projected solid angle over pi, the integral of p_M0 over the polygon.
    float DiffuseIntegral;
    /// E_1: the LTC integral; zero when the LTC-space clip is empty or the receiver has no specular LTC.
    float SpecularIntegral;
    /// c_0 = L_e a_0 E_0 and c_1 = L_e a_1 E_1 per channel (section 4.2).
    float3 DiffuseCoefficient;
    float3 SpecularCoefficient;
};

/// Prepare a triangle for both techniques. emission is the triangle's constant emission factor L_e; a one-sided
/// emitter whose geometric normal faces away from the receiver emits nothing towards it, which zeroes the
/// coefficients. Returns false for a nonfinite vertex in the shading frame or after the LTC transform; an empty or
/// edge-on clip succeeds with a zero integral, which disables that technique.
[noinline] bool TryPreparePolygonalLight(PolygonalLightReceiver receiver,
                                         float3 position0World,
                                         float3 position1World,
                                         float3 position2World,
                                         float3 geometricNormalWorld,
                                         float3 emission,
                                         bool isEmissionTwoSided,
                                         out PolygonalLight outLight) {
    outLight.Position0World = position0World;
    outLight.Position1World = position1World;
    outLight.Position2World = position2World;
    outLight.Diffuse = MakeEmptyProjectedSolidAnglePolygon(false);
    outLight.Specular.TransformedPolygon = MakeEmptyProjectedSolidAnglePolygon(false);
    outLight.Specular.Distribution = receiver.Specular;
    outLight.DiffuseIntegral = 0;
    outLight.SpecularIntegral = 0;
    outLight.DiffuseCoefficient = float3(0);
    outLight.SpecularCoefficient = float3(0);
    // A planar triangle faces the receiver from every point on it, as EvaluateEmission tests at one point.
    float3 facingEmission =
        isEmissionTwoSided == true || dot(geometricNormalWorld, receiver.PositionWorld - position0World) > 0
            ? max(emission, float3(0))
            : float3(0);
    ProjectedPolygonInput polygon;
    polygon.VertexCount = 3;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        polygon.PositionsTangent[i] = float3(0);
    }
    polygon.PositionsTangent[0] = TransformWorldToTangent(receiver, position0World - receiver.PositionWorld);
    polygon.PositionsTangent[1] = TransformWorldToTangent(receiver, position1World - receiver.PositionWorld);
    polygon.PositionsTangent[2] = TransformWorldToTangent(receiver, position2World - receiver.PositionWorld);
    if (TryPrepareProjectedSolidAnglePolygon(polygon, outLight.Diffuse) == false)
        return false;
    outLight.DiffuseIntegral = outLight.Diffuse.ProjectedSolidAngle / PI;
    if (receiver.HasSpecular == true)
    {
        if (TryPrepareLinearlyTransformedCosinePolygon(polygon, receiver.Specular, outLight.Specular) == false)
            return false;
        outLight.SpecularIntegral = GetLinearlyTransformedCosinePolygonIntegral(outLight.Specular);
    }
    outLight.DiffuseCoefficient = facingEmission * receiver.DiffuseAlbedo * outLight.DiffuseIntegral;
    outLight.SpecularCoefficient = facingEmission * receiver.SpecularAlbedo * outLight.SpecularIntegral;
    return IsFiniteBits(outLight.DiffuseCoefficient) == true && IsFiniteBits(outLight.SpecularCoefficient) == true;
}

/// True when estimator draws a sample of technique.
bool UsesPolygonalLightTechnique(uint estimator, uint technique)
{
    if (estimator == PROJECTED_SOLID_ANGLE_ESTIMATOR)
        return technique == PROJECTED_SOLID_ANGLE_TECHNIQUE;
    if (estimator == LTC_ESTIMATOR)
        return technique == LTC_TECHNIQUE;
    return true;
}

/// One sample of a technique, mapped to the point where it meets the triangle.
struct PolygonalLightSample
{
    /// Unit direction from the receiver.
    float3 DirectionWorld;
    /// (p_0, p_1) per steradian at DirectionWorld, the same functions for the samples of both techniques.
    float2 Densities;
    /// cos(theta) against the shading normal; positive.
    float CosineSurface;
    float DistanceWorld;
    /// Weights of Position1World (x) and Position2World (y), as GetSurfaceHit takes them.
    float2 Barycentric;
};

/// Map a direction from originWorld to the point where it meets the triangle. Returns false, with zero outputs, for a
/// direction parallel to the plane, a plane behind the origin or a point outside the triangle in FP32: such a sample
/// contributes zero.
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

/// Draw one sample of technique from two uniform random numbers. Returns false, an ordinary zero contribution, when
/// the technique is disabled (an empty LTC-space clip, section 4.1) or cannot sample, when its density at the
/// normalized direction is not positive, when the direction lies below the shading horizon (an LTC sample may,
/// section 4.1) or when it misses the triangle in FP32.
[noinline] bool TrySamplePolygonalLight(PolygonalLightReceiver receiver,
                                        PolygonalLight light,
                                        uint technique,
                                        float2 randomSample,
                                        out PolygonalLightSample outSample) {
    outSample = (PolygonalLightSample)0;
    float3 directionTangent = float3(0);
    bool isSampled = false;
    if (technique == PROJECTED_SOLID_ANGLE_TECHNIQUE)
        isSampled = light.DiffuseIntegral > 0 &&
                    TrySampleProjectedSolidAnglePolygon(light.Diffuse, randomSample, directionTangent) == true;
    else
        isSampled = light.SpecularIntegral > 0 &&
                    TrySampleLinearlyTransformedCosinePolygon(light.Specular, randomSample, directionTangent) == true;
    if (isSampled == false)
        return false;
    // Densities are evaluated at the normalized world direction, so both techniques' samples weigh one function.
    float3 directionWorld = NormalizeOrZero(TransformTangentToWorld(receiver, directionTangent));
    float3 normalizedTangent = TransformWorldToTangent(receiver, directionWorld);
    float2 densities = float2(EvaluateProjectedSolidAnglePolygonDensity(light.Diffuse, normalizedTangent),
                              EvaluateLinearlyTransformedCosinePolygonDensity(light.Specular, normalizedTangent));
    float techniqueDensity = technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? densities.x : densities.y;
    float cosineSurface = dot(receiver.NormalWorld, directionWorld);
    if ((techniqueDensity > 0) == false || (cosineSurface > 0) == false)
        return false;
    float distanceWorld;
    float2 barycentric;
    if (TryIntersectTrianglePlane(receiver.PositionWorld,
                                  directionWorld,
                                  light.Position0World,
                                  light.Position1World,
                                  light.Position2World,
                                  distanceWorld,
                                  barycentric) == false)
        return false;
    outSample.DirectionWorld = directionWorld;
    outSample.Densities = densities;
    outSample.CosineSurface = cosineSurface;
    outSample.DistanceWorld = distanceWorld;
    outSample.Barycentric = barycentric;
    return true;
}

/// The MIS weight per channel of a sample of technique under estimator. densities are (p_0, p_1) at the sample;
/// visibility is section 4.3's v in [0, 1]. Both MIS estimators' weights sum to one over the two techniques wherever a
/// density is positive, and a technique with zero density gets zero weight. A single-technique estimator weighs its
/// own technique's samples one wherever it can sample them.
float3 ComputePolygonalLightWeight(uint estimator,
                                   uint technique,
                                   PolygonalLight light,
                                   float2 densities,
                                   float visibility)
{
    float techniqueDensity = technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? densities.x : densities.y;
    if ((techniqueDensity > 0) == false || UsesPolygonalLightTechnique(estimator, technique) == false)
        return float3(0);
    if (estimator == PROJECTED_SOLID_ANGLE_ESTIMATOR || estimator == LTC_ESTIMATOR)
        return float3(1);
    // Veach's balance heuristic p_j / sum_k p_k, section 4.3's second term.
    float balance = techniqueDensity / (densities.x + densities.y);
    if (estimator == BALANCE_HEURISTIC_ESTIMATOR)
        return float3(balance);
    // Section 4.2's weighted balance heuristic c_j p_j / sum_k c_k p_k per channel, blended with the balance heuristic
    // by v (section 4.3). Where every c_k p_k of a channel is zero, the weighted part falls back to the balance
    // heuristic, so the weights still sum to one.
    float3 techniqueCoefficient =
        technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? light.DiffuseCoefficient : light.SpecularCoefficient;
    float3 weightedDensities = techniqueCoefficient * techniqueDensity;
    float3 weightedSums = light.DiffuseCoefficient * densities.x + light.SpecularCoefficient * densities.y;
    float3 weight = float3(balance);
    [unroll] for (uint channel = 0; channel < COLOR_CHANNEL_COUNT; ++channel)
    {
        float weighted = weightedSums[channel] > 0 ? weightedDensities[channel] / weightedSums[channel] : balance;
        weight[channel] = visibility * weighted + (1 - visibility) * balance;
    }
    return weight;
}

/// The unshadowed contribution of one sample before emission: f(omega) cos(theta) w_j(omega) / p_j(omega) per
/// channel, with the BSDF evaluated exactly. The caller multiplies by the emitted radiance at the sampled point and
/// by its visibility. Zero where the weight or the BSDF is zero.
float3 EstimatePolygonalLightSample(uint estimator,
                                    uint technique,
                                    MaterialSample material,
                                    float3 viewDirectionWorld,
                                    PolygonalLight light,
                                    PolygonalLightSample lightSample,
                                    float visibility)
{
    float3 weight = ComputePolygonalLightWeight(estimator, technique, light, lightSample.Densities, visibility);
    if (any(weight > 0) == false)
        return float3(0);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(material, viewDirectionWorld, lightSample.DirectionWorld, bsdfValue, bsdfDensity);
    float techniqueDensity =
        technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? lightSample.Densities.x : lightSample.Densities.y;
    return bsdfValue * weight * (lightSample.CosineSurface / techniqueDensity);
}
