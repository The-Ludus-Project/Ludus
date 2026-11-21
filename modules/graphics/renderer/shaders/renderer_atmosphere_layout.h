#pragma once

// A0 Earth shell dimensions in kilometres, from Hillaire's SetupEarthAtmosphere.
#define LUDUS_ATMOSPHERE_GROUND_KILOMETRES 6360.0
#define LUDUS_ATMOSPHERE_HEIGHT_KILOMETRES 100.0
#define LUDUS_ATMOSPHERE_TRANSMITTANCE_WIDTH 256
#define LUDUS_ATMOSPHERE_TRANSMITTANCE_HEIGHT 64
#define LUDUS_ATMOSPHERE_SKY_WIDTH 192
// Each side of the geometric horizon occupies its own filter domain.
#define LUDUS_ATMOSPHERE_SKY_HALF_HEIGHT 64

#define LUDUS_ATMOSPHERE_MULTIPLE_SIZE 32

// Earth clear-sky profile from Hillaire's SetupEarthAtmosphere: scale heights in kilometres,
// coefficients per kilometre. Shared by the GPU tables and the CPU reference sky.
#define LUDUS_ATMOSPHERE_RAYLEIGH_SCALE_HEIGHT_KILOMETRES 8.0
#define LUDUS_ATMOSPHERE_AEROSOL_SCALE_HEIGHT_KILOMETRES 1.2
#define LUDUS_ATMOSPHERE_OZONE_PEAK_HEIGHT_KILOMETRES 25.0
#define LUDUS_ATMOSPHERE_OZONE_HALF_WIDTH_KILOMETRES 15.0
#define LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_RED 0.005802
#define LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_GREEN 0.013558
#define LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_BLUE 0.033100
#define LUDUS_ATMOSPHERE_AEROSOL_SCATTERING 0.003996
#define LUDUS_ATMOSPHERE_AEROSOL_EXTINCTION 0.004440
#define LUDUS_ATMOSPHERE_OZONE_ABSORPTION_RED 0.000650
#define LUDUS_ATMOSPHERE_OZONE_ABSORPTION_GREEN 0.001881
#define LUDUS_ATMOSPHERE_OZONE_ABSORPTION_BLUE 0.000085
// Cornette-Shanks asymmetry of the aerosol phase function.
#define LUDUS_ATMOSPHERE_AEROSOL_ASYMMETRY 0.8

// Quadrature of the transmittance, sky and multiple-scattering tables.
#define LUDUS_ATMOSPHERE_TRANSMITTANCE_STEPS 96
#define LUDUS_ATMOSPHERE_SKY_STEPS 80
#define LUDUS_ATMOSPHERE_MULTIPLE_ZENITH_SAMPLES 4
#define LUDUS_ATMOSPHERE_MULTIPLE_AZIMUTH_SAMPLES 8
#define LUDUS_ATMOSPHERE_MULTIPLE_STEPS 24
// Smallest escaping fraction in the multiple-scattering series, bounding its geometric sum.
#define LUDUS_ATMOSPHERE_MULTIPLE_MINIMUM_ESCAPE 0.01
// Step optical depth below which (1 - exp(-d)) / d uses its first-order expansion.
#define LUDUS_ATMOSPHERE_OPTICAL_DEPTH_LINEAR_THRESHOLD 0.001

// Bounded cloud layer (CloudLayerDescription): uniform midpoint steps through the slab along a view ray, and the
// quadrature of the clear air between the camera and the slab's entry.
#define LUDUS_CLOUD_LAYER_STEPS 32
#define LUDUS_CLOUD_LAYER_AIR_STEPS 16
// float4 rows of the packed layer in the sky and deferred uniforms (CloudLayerUniform).
#define LUDUS_CLOUD_LAYER_UNIFORM_ROWS 6
// Upper bound in metres of any path through an admitted slab, used to clip rays toward the Sun.
#define LUDUS_CLOUD_LAYER_MAXIMUM_PATH_METRES 1000000.0

#define LUDUS_ATMOSPHERE_METRES_PER_KILOMETRE 1000
#define LUDUS_ATMOSPHERE_DEFERRED_MULTIPLE_SLOT 7
#define LUDUS_ATMOSPHERE_DEFERRED_SKY_SLOT 8
#define LUDUS_ATMOSPHERE_DEFERRED_TRANSMITTANCE_SLOT 10
