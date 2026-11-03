#pragma once
#include "material.h"

// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering, 4th edition (2023),
// section 9.3 (Specular Reflection and Transmission) and section 9.5 (Dielectric BSDF),
// https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF. Adapted from pbrt-v4 revision
// b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f, src/pbrt/bxdfs.{h,cpp} (DielectricBxDF, ThinDielectricBxDF) and
// util/scattering.h (FrDielectric, Refract), Copyright 1998-2020 the authors, licensed under Apache-2.0 (see
// third_party/pbrt/LICENSE.txt). GPU twin of reference_dielectric.cpp: smooth interfaces only, one real index for
// all RGB channels and radiance transport.

/// One specular dielectric proposal: Weight is the complete throughput multiplier.
struct DielectricSample
{
    float3 DirectionWorld;
    float3 Weight;
    bool HasSample;
};
bool IsDielectricModel(MaterialModel model)
{
    return model == MaterialModel::Dielectric || model == MaterialModel::ThinDielectric;
}
/// Unpolarized Fresnel reflectance of a dielectric interface with relative index eta; cosine is signed.
float ComputeDielectricFresnel(float cosine, float eta)
{
    cosine = clamp(cosine, -1, 1);
    if (cosine < 0)
    {
        eta = 1 / eta;
        cosine = -cosine;
    }
    float transmittedSineSquared = (1 - cosine * cosine) / (eta * eta);
    if (transmittedSineSquared >= 1)
        return 1;
    float transmittedCosine = sqrt(1 - transmittedSineSquared);
    float parallel = (eta * cosine - transmittedCosine) / (eta * cosine + transmittedCosine);
    float perpendicular = (cosine - eta * transmittedCosine) / (cosine + eta * transmittedCosine);
    return (parallel * parallel + perpendicular * perpendicular) * 0.5;
}
/// Reflect with probability equal to the reflectance, otherwise refract (or pass a thin slab).
/// normalWorld faces viewWorld; isViewerInside selects the inverse relative index of a solid dielectric.
DielectricSample SampleDielectric(MaterialModel model,
                                  float eta,
                                  bool isViewerInside,
                                  float3 normalWorld,
                                  float3 viewWorld,
                                  float lobeSample)
{
    DielectricSample result = (DielectricSample)0;
    float cosine = dot(normalWorld, viewWorld);
    if (cosine <= 0)
        return result;
    bool isThin = model == MaterialModel::ThinDielectric;
    float relativeEta = isThin == false && isViewerInside == true ? 1 / eta : eta;
    float reflectance = ComputeDielectricFresnel(cosine, relativeEta);
    if (isThin == true && reflectance < 1)
    {
        // Sum the slab's internal reflections, a geometric series in the interface reflectance.
        float transmittance = 1 - reflectance;
        reflectance += transmittance * transmittance * reflectance / (1 - reflectance * reflectance);
    }
    result.HasSample = true;
    result.Weight = 1;
    if (lobeSample < reflectance)
    {
        result.DirectionWorld = normalize(normalWorld * (2 * cosine) - viewWorld);
        return result;
    }
    if (isThin == true)
    {
        result.DirectionWorld = -viewWorld;
        return result;
    }
    // Snell refraction; total internal reflection has reflectance one and never reaches this branch.
    float transmittedSineSquared = (1 - cosine * cosine) / (relativeEta * relativeEta);
    float transmittedCosine = sqrt(max(0, 1 - transmittedSineSquared));
    result.DirectionWorld =
        normalize(-viewWorld / relativeEta + normalWorld * (cosine / relativeEta - transmittedCosine));
    // Radiance compresses into the narrower solid angle of the denser side (pbrt-v4 TransportMode::Radiance).
    result.Weight = 1 / (relativeEta * relativeEta);
    return result;
}
