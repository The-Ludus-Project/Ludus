#pragma once
#include "spectral_sensor_layout.h"
// Spectral camera shared by the steinberg-plt, steinberg-physical-optics and wave-tracing templates and by spectral
// reference captures: wavelengths, the camera's three channel sensitivities and an RGB-to-spectrum uplift for the
// scene's linear RGB emissions and reflectances. spectral_sensor.cpp is the CPU oracle's twin; both read the
// constants of spectral_sensor_layout.h, so the templates' images and the reference captures compare directly.
// Thanks to Chris Wyman, Peter-Pike Sloan and Peter Shirley, "Simple Analytic Approximations to the CIE XYZ
// Color Matching Functions", JCGT 2(2), 2013, https://jcgt.org/published/0002/02/01/ : EvaluateCieXyz1931 is
// their multi-lobe piecewise-Gaussian fit of the CIE 1931 2-degree observer.
// Thanks to Alexander Wilkie, Sehera Nawaz, Marc Droske, Andrea Weidlich and Johannes Hanika, "Hero
// Wavelength Spectral Sampling", Computer Graphics Forum 33(4), EGSR 2014, https://doi.org/10.1111/cgf.12419 :
// SampleWavelengthNanometers rotates one uniform draw into equally spaced wavelengths over the range, as their
// hero wavelength does. Each rotated wavelength is itself uniform, so estimates that use several of them average
// their estimates; they do not use the paper's spectral MIS, because every estimate samples its wavelength with the
// same density.
//
// The camera is a three-channel sensor with nonnegative sensitivities: the fit's linear sRGB (D65)
// colour-matching functions with their negative lobes removed, each scaled so that a unit equal-energy
// spectrum over [380, 780] nm reads (1, 1, 1). Renderer accumulation rejects negative samples; this sensor
// never produces one. It maps saturated spectral colours that lie outside the sRGB gamut inside it.
//
// The uplift turns a linear RGB triple into a piecewise-constant spectrum over three bands, [380, 490),
// [490, 580) and [580, 780] nm. The band values are the inverse of the sensor's band-integral matrix applied to
// the triple, so the sensor reads the triple back exactly and white becomes a flat spectrum. A band value below
// zero, which only strongly saturated colours need, is clamped to zero, which breaks the round trip for them.
// The sensor scale and the uplift rows were integrated from the fit with the midpoint rule at 0.001 nm steps.

static const float SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS = LUDUS_SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS;
static const float SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS = LUDUS_SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS;
static const float SPECTRUM_WAVELENGTH_RANGE_NANOMETERS =
    SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS - SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS;
/// Equally spaced wavelengths per sample for the estimates that are cheap to repeat per wavelength.
static const uint SPECTRUM_HERO_WAVELENGTH_COUNT = 4;
/// Lower edges of the uplift's green and red bands; the blue band starts at the range minimum.
static const float SPECTRUM_GREEN_BAND_MINIMUM_NANOMETERS = LUDUS_SPECTRUM_GREEN_BAND_MINIMUM_NANOMETERS;
static const float SPECTRUM_RED_BAND_MINIMUM_NANOMETERS = LUDUS_SPECTRUM_RED_BAND_MINIMUM_NANOMETERS;
/// CIE XYZ to linear sRGB (D65 white), IEC 61966-2-1.
static const float3x3 SPECTRUM_XYZ_TO_LINEAR_SRGB = float3x3(LUDUS_XYZ_TO_SRGB_00,
                                                             LUDUS_XYZ_TO_SRGB_01,
                                                             LUDUS_XYZ_TO_SRGB_02,
                                                             LUDUS_XYZ_TO_SRGB_10,
                                                             LUDUS_XYZ_TO_SRGB_11,
                                                             LUDUS_XYZ_TO_SRGB_12,
                                                             LUDUS_XYZ_TO_SRGB_20,
                                                             LUDUS_XYZ_TO_SRGB_21,
                                                             LUDUS_XYZ_TO_SRGB_22);
/// Per-channel reciprocal of the clipped sensitivity integral over the range, in nanometres^-1.
static const float3 SPECTRUM_SENSOR_SCALE =
    float3(LUDUS_SPECTRUM_SENSOR_SCALE_RED, LUDUS_SPECTRUM_SENSOR_SCALE_GREEN, LUDUS_SPECTRUM_SENSOR_SCALE_BLUE);
/// Rows of the inverse band-integral matrix: dot(row, linearRgb) is that band's spectral value.
static const float3 SPECTRUM_UPLIFT_RED_BAND = float3(LUDUS_SPECTRUM_UPLIFT_RED_BAND_RED,
                                                      LUDUS_SPECTRUM_UPLIFT_RED_BAND_GREEN,
                                                      LUDUS_SPECTRUM_UPLIFT_RED_BAND_BLUE);
static const float3 SPECTRUM_UPLIFT_GREEN_BAND = float3(LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_RED,
                                                        LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_GREEN,
                                                        LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_BLUE);
static const float3 SPECTRUM_UPLIFT_BLUE_BAND = float3(LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_RED,
                                                       LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_GREEN,
                                                       LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_BLUE);

/// One lobe of the fit: a Gaussian with separate deviations below and above its mean.
float EvaluatePiecewiseGaussian(float wavelengthNanometers,
                                float meanNanometers,
                                float lowerDeviationNanometers,
                                float upperDeviationNanometers)
{
    float deviation = wavelengthNanometers < meanNanometers ? lowerDeviationNanometers : upperDeviationNanometers;
    float t = (wavelengthNanometers - meanNanometers) / deviation;
    return exp(-0.5 * t * t);
}

/// CIE 1931 colour-matching functions (x, y, z) at one wavelength, Wyman, Sloan and Shirley's fit.
float3 EvaluateCieXyz1931(float wavelengthNanometers)
{
    float x = LUDUS_CIE_X_LOBE0_WEIGHT * EvaluatePiecewiseGaussian(wavelengthNanometers,
                                                                   LUDUS_CIE_X_LOBE0_MEAN,
                                                                   LUDUS_CIE_X_LOBE0_LOWER,
                                                                   LUDUS_CIE_X_LOBE0_UPPER) +
              LUDUS_CIE_X_LOBE1_WEIGHT * EvaluatePiecewiseGaussian(wavelengthNanometers,
                                                                   LUDUS_CIE_X_LOBE1_MEAN,
                                                                   LUDUS_CIE_X_LOBE1_LOWER,
                                                                   LUDUS_CIE_X_LOBE1_UPPER) +
              LUDUS_CIE_X_LOBE2_WEIGHT * EvaluatePiecewiseGaussian(wavelengthNanometers,
                                                                   LUDUS_CIE_X_LOBE2_MEAN,
                                                                   LUDUS_CIE_X_LOBE2_LOWER,
                                                                   LUDUS_CIE_X_LOBE2_UPPER);
    float y = LUDUS_CIE_Y_LOBE0_WEIGHT * EvaluatePiecewiseGaussian(wavelengthNanometers,
                                                                   LUDUS_CIE_Y_LOBE0_MEAN,
                                                                   LUDUS_CIE_Y_LOBE0_LOWER,
                                                                   LUDUS_CIE_Y_LOBE0_UPPER) +
              LUDUS_CIE_Y_LOBE1_WEIGHT * EvaluatePiecewiseGaussian(wavelengthNanometers,
                                                                   LUDUS_CIE_Y_LOBE1_MEAN,
                                                                   LUDUS_CIE_Y_LOBE1_LOWER,
                                                                   LUDUS_CIE_Y_LOBE1_UPPER);
    float z = LUDUS_CIE_Z_LOBE0_WEIGHT * EvaluatePiecewiseGaussian(wavelengthNanometers,
                                                                   LUDUS_CIE_Z_LOBE0_MEAN,
                                                                   LUDUS_CIE_Z_LOBE0_LOWER,
                                                                   LUDUS_CIE_Z_LOBE0_UPPER) +
              LUDUS_CIE_Z_LOBE1_WEIGHT * EvaluatePiecewiseGaussian(wavelengthNanometers,
                                                                   LUDUS_CIE_Z_LOBE1_MEAN,
                                                                   LUDUS_CIE_Z_LOBE1_LOWER,
                                                                   LUDUS_CIE_Z_LOBE1_UPPER);
    return float3(x, y, z);
}

/// The sensor's linear RGB response per unit spectral radiance at one wavelength, in nanometres^-1.
float3 EvaluateSensorSensitivity(float wavelengthNanometers)
{
    return max(mul(SPECTRUM_XYZ_TO_LINEAR_SRGB, EvaluateCieXyz1931(wavelengthNanometers)), float3(0)) *
           SPECTRUM_SENSOR_SCALE;
}

/// Spectral value at one wavelength of the uplifted linear RGB triple; never negative.
float EvaluateUpliftedSpectrum(float3 linearRgb, float wavelengthNanometers)
{
    float3 band = SPECTRUM_UPLIFT_BLUE_BAND;
    if (wavelengthNanometers >= SPECTRUM_RED_BAND_MINIMUM_NANOMETERS)
        band = SPECTRUM_UPLIFT_RED_BAND;
    else if (wavelengthNanometers >= SPECTRUM_GREEN_BAND_MINIMUM_NANOMETERS)
        band = SPECTRUM_UPLIFT_GREEN_BAND;
    return max(dot(band, linearRgb), 0);
}

/// The heroIndex-th of SPECTRUM_HERO_WAVELENGTH_COUNT equally spaced wavelengths rotated by randomValue in [0, 1).
/// Each is uniform over the range, with density 1 / SPECTRUM_WAVELENGTH_RANGE_NANOMETERS.
float SampleWavelengthNanometers(float randomValue, uint heroIndex)
{
    float fraction = frac(randomValue + float(heroIndex) / float(SPECTRUM_HERO_WAVELENGTH_COUNT));
    return SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS + fraction * SPECTRUM_WAVELENGTH_RANGE_NANOMETERS;
}

/// Linear RGB estimate of a spectral radiance sampled at one uniformly drawn wavelength.
float3 ConvertSpectralRadiance(float spectralRadiance, float wavelengthNanometers)
{
    return spectralRadiance * EvaluateSensorSensitivity(wavelengthNanometers) * SPECTRUM_WAVELENGTH_RANGE_NANOMETERS;
}
