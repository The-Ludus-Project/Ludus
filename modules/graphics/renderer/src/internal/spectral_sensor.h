#pragma once
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>

#include "../../shaders/spectral_sensor_layout.h"

// CPU twin of shaders/spectral_sensor.slang: the spectral camera of spectral reference captures, from the same
// constants (spectral_sensor_layout.h). spectral_sensor.slang documents the sensor, the uplift and their sources.
namespace ludus::graphics::renderer::internal
{
/// Shortest wavelength of the sampled range, in nanometres.
inline constexpr foundation::float32 SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS =
    static_cast<foundation::float32>(LUDUS_SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS);
/// Width of the sampled range, in nanometres; a uniform wavelength has its reciprocal as density.
inline constexpr foundation::float32 SPECTRUM_WAVELENGTH_RANGE_NANOMETERS =
    static_cast<foundation::float32>(LUDUS_SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS) -
    SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS;
/// CIE 1931 colour-matching functions (x, y, z) at one wavelength, Wyman, Sloan and Shirley's fit.
[[nodiscard]] foundation::math::Vector3 EvaluateCieXyz1931(foundation::float32 wavelengthNanometers) noexcept;
/// The sensor's linear RGB response per unit spectral radiance at one wavelength, in nanometres^-1; nonnegative.
[[nodiscard]] foundation::math::Vector3 EvaluateSensorSensitivity(foundation::float32 wavelengthNanometers) noexcept;
/// Spectral value at one wavelength of the uplifted linear RGB triple; never negative.
[[nodiscard]] foundation::float32 EvaluateUpliftedSpectrum(foundation::math::Vector3 linearRgb,
                                                           foundation::float32 wavelengthNanometers) noexcept;
/// Wavelength in nanometres, uniform over the range, for a random value in [0, 1).
[[nodiscard]] foundation::float32 SampleWavelengthNanometers(foundation::float32 randomValue) noexcept;
/// Linear RGB estimate of a spectral radiance sampled at one uniformly drawn wavelength.
[[nodiscard]] foundation::math::Vector3 ConvertSpectralRadiance(foundation::float32 spectralRadiance,
                                                                foundation::float32 wavelengthNanometers) noexcept;
} // namespace ludus::graphics::renderer::internal
