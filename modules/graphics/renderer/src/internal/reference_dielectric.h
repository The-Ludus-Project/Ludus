#pragma once

#include <ludus/graphics/renderer/reference.hpp>

namespace ludus::graphics::renderer::internal
{
/// True for the perfectly specular dielectric models, whose lobes are directional deltas.
[[nodiscard]] bool IsReferenceDielectric(content::scene_format::MaterialModel model) noexcept;
/// Other models pass; dielectrics need one real index (equal Eta channels, zero K) and a smooth interface.
[[nodiscard]] bool IsReferenceDielectricSupported(const content::scene_format::MaterialScattering& scattering) noexcept;
/// Unpolarized Fresnel reflectance of a dielectric interface with relative index eta; cosine is signed.
[[nodiscard]] foundation::float32 ComputeDielectricFresnel(foundation::float32 cosine,
                                                           foundation::float32 eta) noexcept;
/// Choose reflection or transmission with lobeSample in [0,1). normalWorld faces viewWorld.
[[nodiscard]] ReferenceBsdfSample SampleDielectric(const ReferenceBsdf& bsdf,
                                                   foundation::math::Vector3 normalWorld,
                                                   foundation::math::Vector3 viewWorld,
                                                   foundation::float32 lobeSample) noexcept;
} // namespace ludus::graphics::renderer::internal
