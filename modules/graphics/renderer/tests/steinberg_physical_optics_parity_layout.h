#pragma once

// ABI shared by steinberg_physical_optics_parity_tests.cpp and steinberg_physical_optics_parity.slang. Invocation c
// reads case c from the cases buffer, LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS uint4 records of float bits (u: uint):
//   0: incident propagation xyz, wavelength (nm)      5: normal xyz, eta real part
//   1: incident transverse x xyz, distance r (m)      6: unit tangent xyz, eta imaginary part
//   2: incident Stokes vector                         7: outgoing direction xyz, rms height q (m)
//   3: theta_x (xx, xy, yy), lobe (u)                 8: sigma_s, sigma_zz, grating period, grating height (m)
//   4: theta_y (xx, xy, yy), is wave (u)              9: highest grating order (u), envelope variance, weight, 0
// and writes LUDUS_PHYSICAL_OPTICS_PARITY_RESULT_RECORDS records to the results buffer:
//   0: scattered Stokes vector                        3: D of the 1/2 component, alpha_direct, envelope density,
//   1: scattered theta_x, D of the x component           incident coherence length (m)
//   2: scattered theta_y, D of the y component        4: scattered transverse x xyz, 0
// Lobes: 0 Gaussian surface, 1 perfectly diffuse, 2 grating, 3 direct (OpticsLobe in
// steinberg_physical_optics_oracle.h).
#define LUDUS_PHYSICAL_OPTICS_PARITY_CASES_BINDING 0
#define LUDUS_PHYSICAL_OPTICS_PARITY_RESULTS_BINDING 1
#define LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS 10
#define LUDUS_PHYSICAL_OPTICS_PARITY_RESULT_RECORDS 5
#define LUDUS_PHYSICAL_OPTICS_PARITY_GROUP_SIZE 64
#define LUDUS_PHYSICAL_OPTICS_PARITY_CASE_COUNT 64
#define LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_SCATTERED 0
#define LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_DIFFUSE 1
#define LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_GRATING 2
#define LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_DIRECT 3
