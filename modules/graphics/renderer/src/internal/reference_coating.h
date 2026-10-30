#pragma once

#include <ludus/graphics/renderer/reference.hpp>

namespace ludus::graphics::renderer::internal
{
[[nodiscard]] bool IsReferenceCoatingSupported(const content::scene_format::MaterialScattering& scattering) noexcept;
[[nodiscard]] ReferenceBsdfEvaluation EvaluateCoatedDiffuse(const ReferenceBsdf& bsdf,
                                                            foundation::math::Vector3 normalWorld,
                                                            foundation::math::Vector3 viewWorld,
                                                            foundation::math::Vector3 lightWorld,
                                                            ReferenceBsdfRandomContext random) noexcept;
[[nodiscard]] ReferenceBsdfSample SampleCoatedDiffuse(const ReferenceBsdf& bsdf,
                                                      foundation::math::Vector3 normalWorld,
                                                      foundation::math::Vector3 viewWorld,
                                                      foundation::math::Vector3 samples,
                                                      ReferenceBsdfRandomContext random) noexcept;
[[nodiscard]] bool IsCoatingRandomContextValid(ReferenceBsdfRandomContext random) noexcept;
} // namespace ludus::graphics::renderer::internal
