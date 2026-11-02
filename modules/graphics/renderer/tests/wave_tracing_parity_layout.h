#pragma once
// Shared scalar-wave parity buffer schema. Three uint4 header records followed by Resolution^2 float-bit masks.
// Header 0: wavelength, SigmaX, SigmaY, half width. Header 1: half height, resolution (uint), receiver x/y.
// Header 2: receiver z, initial rho, propagation distance, reserved. Results: field xy, power, phase admission;
// then variances xy and covariances xy. All physical inputs use metres and square metres.
#define LUDUS_WAVE_PARITY_CASE_COUNT 16
#define LUDUS_WAVE_PARITY_RESOLUTION 32
#define LUDUS_WAVE_PARITY_HEADER_RECORDS 3
#define LUDUS_WAVE_PARITY_COMPONENTS 4
#define LUDUS_WAVE_PARITY_CASE_RECORDS                                                                                 \
    (LUDUS_WAVE_PARITY_HEADER_RECORDS +                                                                                \
     LUDUS_WAVE_PARITY_RESOLUTION * LUDUS_WAVE_PARITY_RESOLUTION / LUDUS_WAVE_PARITY_COMPONENTS)
#define LUDUS_WAVE_PARITY_RESULT_RECORDS 2
