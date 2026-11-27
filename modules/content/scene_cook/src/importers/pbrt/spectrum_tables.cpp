// Conductor colours: normal-incidence reflectance from the published
// "Physically Based" material charts (Dontnod / Unreal Engine 4 tables of F0
// in linear sRGB), used as a stand-in for pbrt's tabulated eta/k spectra.
// Blackbody: Tanner Helland, "How to Convert Temperature (K) to RGB" (2012),
// https://tannerhelland.com/2012/09/18/convert-temperature-rgb-algorithm-code.html,
// a curve fit adopted here and then linearised.
// Glass indices: n_d and Abbe numbers of pbrt-v4's named glass spectra, read from Matt Pharr, Wenzel Jakob and Greg
// Humphreys' pbrt-v4, src/pbrt/util/spectrum.cpp (the GlassBK7_eta ... GlassSF11_eta tables), Copyright 1998-2020
// the authors, licensed under Apache-2.0 (see third_party/pbrt/LICENSE.txt), by linear interpolation at the F, d and
// C lines, as FitGlassIndex does for a tabulated spectrum.

#include "spectrum_tables.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
namespace math = foundation::math;
using foundation::core::usize;

struct NamedColour final
{
    std::string_view Metal;
    float64 Colour[3];
};

constexpr NamedColour CONDUCTORS[] = {
    {"Ag", {.972, .960, .915}},
    {"Al", {.913, .922, .924}},
    {"Au", {1.0, .766, .336}},
    {"Cu", {.955, .638, .538}},
    {"CuZn", {.910, .778, .423}},
    {"MgO", {.95, .95, .95}},
    {"TiO2", {.95, .95, .95}},
};

struct NamedGlass final
{
    std::string_view Name;
    DielectricIndex Index;
};

constexpr NamedGlass GLASSES[] = {
    {"glass-BK7", {1.516803, 63.99}},
    {"glass-BAF10", {1.670002, 47.04}},
    {"glass-FK51A", {1.486559, 84.91}},
    {"glass-LASF9", {1.850268, 32.21}},
    {"glass-F5", {1.672722, 32.28}},
    {"glass-F10", {1.728284, 28.48}},
    {"glass-F11", {1.784743, 25.70}},
};

/// Values per tabulated spectrum sample: a wavelength and its index.
constexpr usize GLASS_SAMPLE_VALUES = 2;

/// Linear interpolation of a validated tabulated spectrum at a wavelength inside its range.
[[nodiscard]] float64 InterpolateGlassIndex(foundation::BorrowedView<const float64> pairs, float64 wavelength) noexcept
{
    const usize samples = pairs.GetSize() / GLASS_SAMPLE_VALUES;
    for (usize i = 1; i < samples; ++i)
    {
        const float64 right = pairs[i * GLASS_SAMPLE_VALUES];
        if (wavelength <= right)
        {
            const float64 left = pairs[(i - 1) * GLASS_SAMPLE_VALUES];
            const float64 t = (wavelength - left) / (right - left);
            return pairs[(i - 1) * GLASS_SAMPLE_VALUES + 1] +
                   (pairs[i * GLASS_SAMPLE_VALUES + 1] - pairs[(i - 1) * GLASS_SAMPLE_VALUES + 1]) * t;
        }
    }
    return pairs[pairs.GetSize() - 1];
}

[[nodiscard]] bool FindConductor(std::string_view metal, float64 (&outColour)[3]) noexcept
{
    for (const NamedColour& entry : CONDUCTORS)
    {
        if (entry.Metal == metal)
        {
            for (int k = 0; k < 3; ++k)
            {
                outColour[k] = entry.Colour[k];
            }
            return true;
        }
    }
    return false;
}
} // namespace

bool LookUpConductorColour(std::string_view spectrumName, float64 (&outColour)[3]) noexcept
{
    if (spectrumName.substr(0, 6) != "metal-")
    {
        return false;
    }
    std::string_view metal = spectrumName.substr(6);
    const foundation::core::usize dash = metal.rfind('-');
    if (dash != std::string_view::npos)
    {
        metal = metal.substr(0, dash);
    }
    return FindConductor(metal, outColour);
}

bool LookUpNamedSpectrum(std::string_view spectrumName, float64 (&outColour)[3]) noexcept
{
    if (spectrumName.substr(0, 9) == "stdillum-")
    {
        outColour[0] = outColour[1] = outColour[2] = 1.0;
        if (spectrumName == "stdillum-A")
        {
            ConvertBlackbodyToRgb(2856.0, outColour);
        }
        return true;
    }
    return LookUpConductorColour(spectrumName, outColour);
}

bool LookUpGlassIndex(std::string_view spectrumName, DielectricIndex& outIndex) noexcept
{
    for (const NamedGlass& entry : GLASSES)
    {
        if (entry.Name == spectrumName)
        {
            outIndex = entry.Index;
            return true;
        }
    }
    return false;
}

bool FitGlassIndex(foundation::BorrowedView<const float64> wavelengthIndexPairs, DielectricIndex& outIndex) noexcept
{
    const usize count = wavelengthIndexPairs.GetSize();
    if (wavelengthIndexPairs.IsValid() == false || count < 2 * GLASS_SAMPLE_VALUES || count % GLASS_SAMPLE_VALUES != 0)
    {
        return false;
    }
    for (usize i = 0; i < count; ++i)
    {
        const float64 value = wavelengthIndexPairs[i];
        const bool isAscending = i < GLASS_SAMPLE_VALUES || i % GLASS_SAMPLE_VALUES != 0 ||
                                 value > wavelengthIndexPairs[i - GLASS_SAMPLE_VALUES];
        if (math::IsFinite(value) == false || value <= 0 || isAscending == false)
        {
            return false;
        }
    }
    const float64 lineF = static_cast<float64>(scene_format::LUDUS_DIELECTRIC_F_LINE_NANOMETERS);
    const float64 lineD = static_cast<float64>(scene_format::LUDUS_DIELECTRIC_D_LINE_NANOMETERS);
    const float64 lineC = static_cast<float64>(scene_format::LUDUS_DIELECTRIC_C_LINE_NANOMETERS);
    if (wavelengthIndexPairs[0] > lineF || wavelengthIndexPairs[count - GLASS_SAMPLE_VALUES] < lineC)
    {
        return false;
    }
    const float64 indexF = InterpolateGlassIndex(wavelengthIndexPairs, lineF);
    const float64 indexD = InterpolateGlassIndex(wavelengthIndexPairs, lineD);
    const float64 indexC = InterpolateGlassIndex(wavelengthIndexPairs, lineC);
    if (indexF < indexC)
    {
        return false;
    }
    outIndex = { .IndexD = indexD, .AbbeNumber = indexF == indexC ? 0.0 : (indexD - 1) / (indexF - indexC) };
    return true;
}

void ConvertBlackbodyToRgb(float64 kelvin, float64 (&outColour)[3]) noexcept
{
    const float64 t = math::Clamp(kelvin, 1000.0, 40000.0) / 100.0;
    float64 red = 255.0;
    float64 green = 0.0;
    float64 blue = 255.0;
    if (t > 66.0)
    {
        red = 329.698727446 * math::Pow(t - 60.0, -0.1332047592);
        green = 288.1221695283 * math::Pow(t - 60.0, -0.0755148492);
    }
    else
    {
        green = 99.4708025861 * math::Log(t) - 161.1195681661;
        if (t <= 19.0)
        {
            blue = 0.0;
        }
        else if (t < 66.0)
        {
            blue = 138.5177312231 * math::Log(t - 10.0) - 305.0447927307;
        }
    }
    const float64 encoded[3] = {red, green, blue};
    float64 maximum = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        outColour[k] = static_cast<float64>(
            math::DecodeSrgb(static_cast<foundation::core::float32>(math::Clamp(encoded[k], 0.0, 255.0) / 255.0)));
        maximum = math::Max(maximum, outColour[k]);
    }
    for (int k = 0; k < 3 && maximum > 0.0; ++k)
    {
        outColour[k] /= maximum;
    }
}
} // namespace ludus::content::scene_cook::importers
