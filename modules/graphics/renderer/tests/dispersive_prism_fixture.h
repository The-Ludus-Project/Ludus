#pragma once

// A dispersive glass prism and its analytic spectral image, shared by the reference-capture parity tests and the
// wave-tracing template's graph tests.

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include "../src/internal/spectral_sensor.h"

namespace ludus::renderer_tests
{
/// pbrt-v4's glass-F11 flint as the pbrt importers declare it (spectrum_tables.cpp): its strong dispersion spreads
/// the visible spectrum over a few columns of the prism fixture.
inline constexpr ludus::content::scene_format::DielectricDispersion PRISM_FLINT
{
    .IndexD = 1.784743F,
    .AbbeNumber = 25.7F,
};
/// Apex angle of the prism fixture in degrees. Flint deviates 380 nm by 26.4 degrees and 780 nm by 23.2.
inline constexpr ludus::foundation::float64 PRISM_APEX_DEGREES = 25;

/// A two-face glass prism under parallel rays along +z, and a white emissive strip behind it. The entry face z = 0
/// meets the rays at normal incidence; the exit face z = ExitDepthWorld + x tan(apex) leans by the apex angle, so each
/// ray leaves deviated towards +x by asin(n sin(apex)) - apex. The strip at z = StripDepthWorld spans
/// [StripLowerWorld, StripUpperWorld] in x, so a ray at a given x sees only the wavelengths whose deviation lands
/// there.
struct PrismFixture final
{
    ludus::foundation::float64 ApexRadians = PRISM_APEX_DEGREES * ludus::foundation::math::kPi / 180;
    ludus::foundation::float64 ExitDepthWorld = 1;
    ludus::foundation::float64 StripDepthWorld = 4;
    ludus::foundation::float64 StripLowerWorld = 1.33;
    ludus::foundation::float64 StripUpperWorld = 1.40;
    ludus::content::scene_format::DielectricDispersion Glass = PRISM_FLINT;
};

/// Unpolarized Fresnel reflectance in float64 at an incident cosine, with the transmitted-to-incident index ratio.
inline ludus::foundation::float64 ComputePrismReflectance(ludus::foundation::float64 incidentCosine,
                                                          ludus::foundation::float64 relativeIndex)
{
    namespace math = ludus::foundation::math;
    using ludus::foundation::float64;
    const float64 transmittedSineSquared = (1 - incidentCosine * incidentCosine) / (relativeIndex * relativeIndex);
    if (transmittedSineSquared >= 1)
    {
        return 1;
    }
    const float64 transmittedCosine = math::Sqrt(1 - transmittedSineSquared);
    const float64 perpendicular =
        (incidentCosine - relativeIndex * transmittedCosine) / (incidentCosine + relativeIndex * transmittedCosine);
    const float64 parallel =
        (relativeIndex * incidentCosine - transmittedCosine) / (relativeIndex * incidentCosine + transmittedCosine);
    return (perpendicular * perpendicular + parallel * parallel) / 2;
}

/// The exit face's depth at x, z = ExitDepthWorld + x tan(apex).
inline ludus::foundation::float64 GetPrismExitDepthWorld(const PrismFixture& prism, ludus::foundation::float64 xWorld)
{
    namespace math = ludus::foundation::math;
    return prism.ExitDepthWorld + xWorld * math::Sin(prism.ApexRadians) / math::Cos(prism.ApexRadians);
}

/// Expected throughput of a ray at x and one vacuum wavelength: both interfaces' Fresnel transmittance when the
/// refracted ray lands on the strip, otherwise zero. Radiance scales by 1 / n^2 entering and n^2 leaving. The index
/// comes from the Cauchy coefficients that n_d and V_d determine, solved here from n_F - n_C = (n_d - 1) / V_d rather
/// than evaluated through ComputeDielectricIndex.
inline ludus::foundation::float64 EvaluatePrismTransmission(const PrismFixture& prism,
                                                            ludus::foundation::float64 xWorld,
                                                            ludus::foundation::float64 nanometers)
{
    namespace format = ludus::content::scene_format;
    namespace math = ludus::foundation::math;
    using ludus::foundation::float64;
    const float64 indexD = static_cast<float64>(prism.Glass.IndexD);
    float64 index = indexD;
    if (prism.Glass.AbbeNumber > 0)
    {
        const float64 lineF = static_cast<float64>(format::LUDUS_DIELECTRIC_F_LINE_NANOMETERS);
        const float64 lineD = static_cast<float64>(format::LUDUS_DIELECTRIC_D_LINE_NANOMETERS);
        const float64 lineC = static_cast<float64>(format::LUDUS_DIELECTRIC_C_LINE_NANOMETERS);
        const float64 cauchyB =
            (indexD - 1) / static_cast<float64>(prism.Glass.AbbeNumber) / (1 / (lineF * lineF) - 1 / (lineC * lineC));
        index = indexD - cauchyB / (lineD * lineD) + cauchyB / (nanometers * nanometers);
    }
    const float64 apexSine = math::Sin(prism.ApexRadians);
    const float64 apexCosine = math::Cos(prism.ApexRadians);
    const float64 exitSine = index * apexSine;
    if (exitSine >= 1)
    {
        return 0;
    }
    const float64 exitCosine = math::Sqrt(1 - exitSine * exitSine);
    // The leaving direction: exitCosine along the outward normal (-sin, 0, cos) plus exitSine along the tangent
    // (cos, 0, sin), towards which the incident ray's tangential part points.
    const float64 directionX = -exitCosine * apexSine + exitSine * apexCosine;
    const float64 directionZ = exitCosine * apexCosine + exitSine * apexSine;
    const float64 landingWorld =
        xWorld + (prism.StripDepthWorld - GetPrismExitDepthWorld(prism, xWorld)) * directionX / directionZ;
    if (landingWorld < prism.StripLowerWorld || landingWorld > prism.StripUpperWorld)
    {
        return 0;
    }
    const float64 entry = 1 - ComputePrismReflectance(1, index);
    const float64 exit = 1 - ComputePrismReflectance(apexCosine, 1 / index);
    return entry * exit;
}

/// First two moments of a pixel's one-wavelength radiance estimates through the prism.
struct PrismPixelMoments final
{
    /// Expected linear RGB: the sensor-weighted spectral integral of the uplifted emission times the throughput.
    ludus::foundation::math::Vector3d Mean{};
    /// Expected square of each channel's estimate range * sensor * emission at a uniform wavelength, when both
    /// interfaces transmit.
    ludus::foundation::math::Vector3d MeanSquares{};
};

/// Integrate a pixel spanning x in [lowerWorld, upperWorld] with `positions` midpoint ray positions (one for a
/// centred ray: lower = upper) and `spectrumSteps` midpoint wavelengths over the camera's sensor range.
inline PrismPixelMoments IntegratePrismPixel(const PrismFixture& prism,
                                             ludus::foundation::float64 lowerWorld,
                                             ludus::foundation::float64 upperWorld,
                                             ludus::foundation::usize positions,
                                             ludus::foundation::usize spectrumSteps,
                                             ludus::foundation::math::Vector3 emission)
{
    namespace math = ludus::foundation::math;
    namespace internal = ludus::graphics::renderer::internal;
    using ludus::foundation::float32;
    using ludus::foundation::float64;
    using ludus::foundation::usize;
    const float64 range = static_cast<float64>(internal::SPECTRUM_WAVELENGTH_RANGE_NANOMETERS);
    const float64 step = range / static_cast<float64>(spectrumSteps);
    const float64 positionWeight = 1 / static_cast<float64>(positions);
    PrismPixelMoments moments;
    for (usize index = 0; index < spectrumSteps; ++index)
    {
        const float64 nanometers = static_cast<float64>(internal::SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS) +
                                   (static_cast<float64>(index) + 0.5) * step;
        const float32 wavelength = static_cast<float32>(nanometers);
        const math::Vector3 sensitivity = internal::EvaluateSensorSensitivity(wavelength);
        const float64 radiance = static_cast<float64>(internal::EvaluateUpliftedSpectrum(emission, wavelength));
        const math::Vector3d weight{static_cast<float64>(sensitivity.X) * radiance,
                                    static_cast<float64>(sensitivity.Y) * radiance,
                                    static_cast<float64>(sensitivity.Z) * radiance};
        float64 transmission = 0;
        for (usize position = 0; position < positions; ++position)
        {
            const float64 xWorld =
                lowerWorld + (upperWorld - lowerWorld) * (static_cast<float64>(position) + 0.5) * positionWeight;
            transmission += EvaluatePrismTransmission(prism, xWorld, nanometers) * positionWeight;
        }
        moments.Mean = moments.Mean + weight * (transmission * step);
        moments.MeanSquares =
            moments.MeanSquares + math::Vector3d{weight.X * weight.X, weight.Y * weight.Y, weight.Z * weight.Z} *
                                      (range * transmission * step);
    }
    return moments;
}
} // namespace ludus::renderer_tests
