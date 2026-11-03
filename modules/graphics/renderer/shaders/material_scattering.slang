#pragma once
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
    return model == MaterialModel::Conductor || metallic == 1 ? 1 : 0.5;
}
float ComputeMaterialAlpha(MaterialModel model, float roughness, float conductorAlpha)
{
    return model == MaterialModel::Conductor ? conductorAlpha : roughness * roughness;
}
