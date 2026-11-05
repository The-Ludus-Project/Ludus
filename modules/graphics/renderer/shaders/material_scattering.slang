#pragma once
#include "math.slang"
#include "renderer_gpu_preview_layout.h"

// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, 4th ed., section 9.3,
// https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission:
// complex-IOR unpolarized Fresnel, matching reference_bsdf.cpp's RGB proxy.
float ComputeConductorFresnel(float cosine, float eta, float extinction)
{
    if (eta == 1 && extinction == 0)
        return 0;
    float cosineSquared = cosine * cosine;
    float sineSquared = 1 - cosineSquared;
    float etaSquared = eta * eta;
    float extinctionSquared = extinction * extinction;
    float difference = etaSquared - extinctionSquared - sineSquared;
    float magnitude = sqrt(difference * difference + 4 * etaSquared * extinctionSquared);
    float realPart = sqrt(max(0, (magnitude + difference) * 0.5));
    float first = magnitude + cosineSquared;
    float second = 2 * cosine * realPart;
    float perpendicular = (first - second) / (first + second);
    float third = cosineSquared * magnitude + sineSquared * sineSquared;
    float fourth = second * sineSquared;
    float parallel = perpendicular * (third - fourth) / (third + fourth);
    return (perpendicular + parallel) * 0.5;
}
float3 ComputeConductorFresnelRgb(float cosine, float3 eta, float3 extinction)
{
    return float3(ComputeConductorFresnel(cosine, eta.x, extinction.x),
                  ComputeConductorFresnel(cosine, eta.y, extinction.y),
                  ComputeConductorFresnel(cosine, eta.z, extinction.z));
}
float ComputeMaterialSpecularProbability(MaterialModel model, float metallic)
{
    if (model == MaterialModel::Diffuse)
        return 0;
    if (model == MaterialModel::DiffuseConductorMix)
        return metallic;
    return model == MaterialModel::Conductor || metallic == 1 ? 1 : 0.5;
}
bool HasRoughConductorLobe(MaterialModel model)
{
    return model == MaterialModel::Conductor || model == MaterialModel::DiffuseConductorMix;
}

float ComputeMaterialAlpha(MaterialModel model, float roughness, float conductorAlpha)
{
    return HasRoughConductorLobe(model) == true ? conductorAlpha : roughness * roughness;
}

// Paired with reference_bsdf.cpp. The PDF sums every lobe, including after sampling.
float4
EvaluateProjectBrdf(float3 color, float roughness, float3 weights, float3 powers, float viewCosine, float lightCosine)
{
    float viewLightCosine = min(1, viewCosine * lightCosine);
    float clampedLightCosine = min(1, lightCosine);
    float3 exponents = (1 - roughness) * powers;
    float3 scales = weights * (exponents + 2) / (2 * PI);
    float3 viewLightPowers = pow(viewLightCosine, exponents);
    viewLightPowers = select(exponents == 0, float3(1), viewLightPowers);
    float total = weights.x + weights.y + weights.z;
    return float4(color * (dot(scales, viewLightPowers) / total),
                  dot(scales, pow(clampedLightCosine, exponents + 1)) / total);
}

float3 SampleProjectBrdfDirection(float roughness,
                                  float3 weights,
                                  float3 powers,
                                  float3 normalWorld,
                                  float3 viewWorld,
                                  float3 samples)
{
    if (dot(normalWorld, viewWorld) <= 0)
        return 0;
    float firstTwoWeights = weights.x + weights.y;
    float selector = samples.x * (firstTwoWeights + weights.z);
    bool shouldSelectFirst = selector < weights.x || (weights.y == 0 && weights.z == 0);
    uint lobe = shouldSelectFirst == true ? 0 : (selector < firstTwoWeights || weights.z == 0 ? 1 : 2);
    float exponent = (1 - roughness) * powers[lobe];
    float cosine = pow(1 - samples.y, 1 / (exponent + 2));
    float radius = sqrt(max(0, 1 - cosine * cosine));
    float angle = 2 * PI * samples.z;
    // Match the CPU oracle's basis selection near the poles.
    static const float PROJECT_BRDF_BASIS_POLE_COSINE = .999;
    float3 poleWorld = abs(normalWorld.z) < PROJECT_BRDF_BASIS_POLE_COSINE ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 axisXWorld = NormalizeOrZero(cross(poleWorld, normalWorld));
    float3 axisYWorld = cross(normalWorld, axisXWorld);
    return NormalizeOrZero(radius * (cos(angle) * axisXWorld + sin(angle) * axisYWorld) + cosine * normalWorld);
}
