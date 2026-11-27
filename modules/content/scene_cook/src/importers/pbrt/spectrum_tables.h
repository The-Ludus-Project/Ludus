#pragma once

// Fixed RGB approximations of pbrt's named spectra and blackbody emitters.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/borrowed_view.hpp>

#include <string_view>

namespace ludus::content::scene_cook::importers
{
using foundation::core::float64;

/// Normal-incidence reflectance (linear RGB) of a pbrt named conductor
/// spectrum such as "metal-Cu-eta" or "metal-Au-k"; false when unknown.
[[nodiscard]] bool LookUpConductorColour(std::string_view spectrumName, float64 (&outColour)[3]) noexcept;
/// RGB for a named illuminant/reflectance spectrum (stdillum-*, metal-*); false when unknown.
[[nodiscard]] bool LookUpNamedSpectrum(std::string_view spectrumName, float64 (&outColour)[3]) noexcept;
/// Normalised linear RGB (max channel 1) of a blackbody at kelvin.
void ConvertBlackbodyToRgb(float64 kelvin, float64 (&outColour)[3]) noexcept;
/// Declared index of a pbrt dielectric's eta spectrum, as scene_format::DielectricDispersion stores it.
struct DielectricIndex final
{
    float64 IndexD = 0;     ///< Index at the Fraunhofer d line.
    float64 AbbeNumber = 0; ///< (n_d - 1) / (n_F - n_C); zero for a constant spectrum.
};
/// n_d and Abbe number of one of pbrt-v4's named glass spectra ("glass-BK7", "glass-BAF10", "glass-FK51A",
/// "glass-LASF9", "glass-F5", "glass-F10", "glass-F11"); false, leaving outIndex unchanged, for other names.
[[nodiscard]] bool LookUpGlassIndex(std::string_view spectrumName, DielectricIndex& outIndex) noexcept;
/// n_d and Abbe number of a tabulated index spectrum: (wavelength in nanometres, index) pairs with strictly increasing
/// wavelengths spanning the F to C lines, interpolated linearly at the F, d and C lines. False, leaving outIndex
/// unchanged, for an odd count, nonfinite or nonpositive values, a range missing a line, or an index that rises with
/// wavelength. The caller admits the result's range (scene_format::IsDielectricDispersionValid).
[[nodiscard]] bool FitGlassIndex(foundation::BorrowedView<const float64> wavelengthIndexPairs,
                                 DielectricIndex& outIndex) noexcept;
} // namespace ludus::content::scene_cook::importers
