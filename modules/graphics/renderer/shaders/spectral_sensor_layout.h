#pragma once
// Spectral camera constants shared by spectral_sensor.slang and the CPU oracle (src/spectral_sensor.cpp): the
// wavelength range, the three-channel sensor's normalization and the RGB-to-spectrum uplift's band matrix. The
// steinberg-plt, steinberg-physical-optics and wave-tracing templates and spectral reference captures read the same
// values. See spectral_sensor.slang for their derivation and attribution.
#define LUDUS_SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS 380.0
#define LUDUS_SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS 780.0
// Lower edges of the uplift's green and red bands; the blue band starts at the range minimum.
#define LUDUS_SPECTRUM_GREEN_BAND_MINIMUM_NANOMETERS 490.0
#define LUDUS_SPECTRUM_RED_BAND_MINIMUM_NANOMETERS 580.0
// Per-channel reciprocal of the clipped sensitivity integral over the range, in nanometres^-1.
#define LUDUS_SPECTRUM_SENSOR_SCALE_RED 0.00567609944
#define LUDUS_SPECTRUM_SENSOR_SCALE_GREEN 0.00866659498
#define LUDUS_SPECTRUM_SENSOR_SCALE_BLUE 0.00914758626
// Rows of the inverse band-integral matrix: dot(row, linearRgb) is that band's spectral value.
#define LUDUS_SPECTRUM_UPLIFT_RED_BAND_RED 1.22711003
#define LUDUS_SPECTRUM_UPLIFT_RED_BAND_GREEN (-0.174578626)
#define LUDUS_SPECTRUM_UPLIFT_RED_BAND_BLUE (-0.0525314062)
#define LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_RED (-0.116550276)
#define LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_GREEN 1.15365767
#define LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_BLUE (-0.0371073909)
#define LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_RED 0.00536857538
#define LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_GREEN (-0.0531401414)
#define LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_BLUE 1.04777157
// Wyman, Sloan and Shirley's multi-lobe fit of the CIE 1931 2-degree observer: each lobe's weight, mean and its
// deviations below and above the mean, in nanometres. x has three lobes (the third subtracts), y and z two each.
#define LUDUS_CIE_X_LOBE0_WEIGHT 1.056
#define LUDUS_CIE_X_LOBE0_MEAN 599.8
#define LUDUS_CIE_X_LOBE0_LOWER 37.9
#define LUDUS_CIE_X_LOBE0_UPPER 31.0
#define LUDUS_CIE_X_LOBE1_WEIGHT 0.362
#define LUDUS_CIE_X_LOBE1_MEAN 442.0
#define LUDUS_CIE_X_LOBE1_LOWER 16.0
#define LUDUS_CIE_X_LOBE1_UPPER 26.7
#define LUDUS_CIE_X_LOBE2_WEIGHT (-0.065)
#define LUDUS_CIE_X_LOBE2_MEAN 501.1
#define LUDUS_CIE_X_LOBE2_LOWER 20.4
#define LUDUS_CIE_X_LOBE2_UPPER 26.2
#define LUDUS_CIE_Y_LOBE0_WEIGHT 0.821
#define LUDUS_CIE_Y_LOBE0_MEAN 568.8
#define LUDUS_CIE_Y_LOBE0_LOWER 46.9
#define LUDUS_CIE_Y_LOBE0_UPPER 40.5
#define LUDUS_CIE_Y_LOBE1_WEIGHT 0.286
#define LUDUS_CIE_Y_LOBE1_MEAN 530.9
#define LUDUS_CIE_Y_LOBE1_LOWER 16.3
#define LUDUS_CIE_Y_LOBE1_UPPER 31.1
#define LUDUS_CIE_Z_LOBE0_WEIGHT 1.217
#define LUDUS_CIE_Z_LOBE0_MEAN 437.0
#define LUDUS_CIE_Z_LOBE0_LOWER 11.8
#define LUDUS_CIE_Z_LOBE0_UPPER 36.0
#define LUDUS_CIE_Z_LOBE1_WEIGHT 0.681
#define LUDUS_CIE_Z_LOBE1_MEAN 459.0
#define LUDUS_CIE_Z_LOBE1_LOWER 26.0
#define LUDUS_CIE_Z_LOBE1_UPPER 13.8
// CIE XYZ to linear sRGB (D65 white), IEC 61966-2-1, row by row.
#define LUDUS_XYZ_TO_SRGB_00 3.2404542
#define LUDUS_XYZ_TO_SRGB_01 (-1.5371385)
#define LUDUS_XYZ_TO_SRGB_02 (-0.4985314)
#define LUDUS_XYZ_TO_SRGB_10 (-0.9692660)
#define LUDUS_XYZ_TO_SRGB_11 1.8760108
#define LUDUS_XYZ_TO_SRGB_12 0.0415560
#define LUDUS_XYZ_TO_SRGB_20 0.0556434
#define LUDUS_XYZ_TO_SRGB_21 (-0.2040259)
#define LUDUS_XYZ_TO_SRGB_22 1.0572252
