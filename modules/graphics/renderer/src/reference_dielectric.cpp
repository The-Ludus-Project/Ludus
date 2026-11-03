#include "internal/reference_dielectric.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>

// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering, 4th edition (2023),
// section 9.3 (Specular Reflection and Transmission) and section 9.5 (Dielectric BSDF),
// https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF. Adapted from pbrt-v4 revision
// b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f, src/pbrt/bxdfs.{h,cpp} (DielectricBxDF, ThinDielectricBxDF) and
// util/scattering.h (FrDielectric, Refract), Copyright 1998-2020 the authors, licensed under Apache-2.0 (see
// third_party/pbrt/LICENSE.txt). Specialization: smooth interfaces only, one real index for all RGB channels and
// radiance transport; shaders/dielectric.slang is the GPU twin.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace math = foundation::math;
namespace format = content::scene_format;
namespace
{
math::Vector3 NormalizeDielectricVector(math::Vector3 value) noexcept
{
    math::Vector3 result{};
    static_cast<void>(math::TryNormalize(value, result));
    return result;
}
} // namespace
bool IsReferenceDielectric(format::MaterialModel model) noexcept
{
    return model == format::MaterialModel::Dielectric || model == format::MaterialModel::ThinDielectric;
}
bool IsReferenceDielectricSupported(const format::MaterialScattering& scattering) noexcept
{
    if (IsReferenceDielectric(scattering.Model) == false)
    {
        return true;
    }
    return scattering.Eta[0] == scattering.Eta[1] && scattering.Eta[0] == scattering.Eta[2] && scattering.K[0] == 0 &&
           scattering.K[1] == 0 && scattering.K[2] == 0 &&
           math::Max(scattering.AlphaU, scattering.AlphaV) < format::LUDUS_SMOOTH_INTERFACE_ALPHA;
}
float32 ComputeDielectricFresnel(float32 cosine, float32 eta) noexcept
{
    cosine = math::Clamp(cosine, -1.0F, 1.0F);
    if (cosine < 0)
    {
        eta = 1 / eta;
        cosine = -cosine;
    }
    const float32 transmittedSineSquared = (1 - cosine * cosine) / (eta * eta);
    if (transmittedSineSquared >= 1)
    {
        return 1;
    }
    const float32 transmittedCosine = math::Sqrt(1 - transmittedSineSquared);
    const float32 parallel = (eta * cosine - transmittedCosine) / (eta * cosine + transmittedCosine);
    const float32 perpendicular = (cosine - eta * transmittedCosine) / (cosine + eta * transmittedCosine);
    return (parallel * parallel + perpendicular * perpendicular) * 0.5F;
}
ReferenceBsdfSample SampleDielectric(const ReferenceBsdf& bsdf,
                                     math::Vector3 normalWorld,
                                     math::Vector3 viewWorld,
                                     float32 lobeSample) noexcept
{
    ReferenceBsdfSample result;
    const float32 cosine = math::Dot(normalWorld, viewWorld);
    if (cosine <= 0)
    {
        return result;
    }
    const bool isThin = bsdf.Scattering.Model == format::MaterialModel::ThinDielectric;
    const float32 eta = bsdf.Scattering.Eta[0];
    // pbrt-v4 orients the interface by the shading frame; Ludus flips the normal to the viewer and keeps the side.
    const float32 relativeEta = isThin == false && bsdf.IsViewerInside == true ? 1 / eta : eta;
    float32 reflectance = ComputeDielectricFresnel(cosine, relativeEta);
    if (isThin == true && reflectance < 1)
    {
        // Sum the slab's internal reflections, a geometric series in the interface reflectance.
        const float32 transmittance = 1 - reflectance;
        reflectance += transmittance * transmittance * reflectance / (1 - reflectance * reflectance);
    }
    result.IsSpecular = true;
    result.Accepted = true;
    result.Weight = {1, 1, 1};
    if (lobeSample < reflectance)
    {
        result.Direction = NormalizeDielectricVector(normalWorld * (2 * cosine) - viewWorld);
        return result;
    }
    if (isThin == true)
    {
        result.Direction = -viewWorld;
        return result;
    }
    // Snell refraction; total internal reflection has reflectance one and never reaches this branch.
    const float32 transmittedSineSquared = (1 - cosine * cosine) / (relativeEta * relativeEta);
    const float32 transmittedCosine = math::Sqrt(math::Max(0.0F, 1 - transmittedSineSquared));
    result.Direction =
        NormalizeDielectricVector(-viewWorld / relativeEta + normalWorld * (cosine / relativeEta - transmittedCosine));
    // Radiance compresses into the narrower solid angle of the denser side (pbrt-v4 TransportMode::Radiance).
    const float32 radianceScale = 1 / (relativeEta * relativeEta);
    result.Weight = {radianceScale, radianceScale, radianceScale};
    return result;
}
} // namespace ludus::graphics::renderer::internal
