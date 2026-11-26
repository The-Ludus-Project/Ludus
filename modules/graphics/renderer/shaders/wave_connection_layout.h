#pragma once
// Scalar paraxial optical-connection profile: one CPU/GPU owner for workspace and truncation limits.
#define LUDUS_WAVE_CONNECTION_AXIS 16
#define LUDUS_WAVE_CONNECTION_CELLS (LUDUS_WAVE_CONNECTION_AXIS * LUDUS_WAVE_CONNECTION_AXIS)
#define LUDUS_WAVE_CONNECTION_REGIONS 64
#define LUDUS_WAVE_CONNECTION_ESCAPE_ATTEMPTS 16
#define LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS 3
#define LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS 2

#define LUDUS_WAVE_CONNECTION_MASK_AXIS 2
#define LUDUS_WAVE_CONNECTION_MASK_SAMPLES (LUDUS_WAVE_CONNECTION_MASK_AXIS * LUDUS_WAVE_CONNECTION_MASK_AXIS)

#define LUDUS_WAVE_CONNECTION_MAX_BALLISTIC_WAVELENGTHS 65536
#define LUDUS_WAVE_CONNECTION_BALLISTIC_SCALE 4

// Eight binary32 epsilons per world-coordinate magnitude, shared with the float64 oracle.
#define LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE 9.5367431640625e-7

// Avoid denormal/overflow-dependent GPU Gaussian denominators at the admitted paraxial distance boundary.
#define LUDUS_WAVE_CONNECTION_MIN_WAVELENGTH_WORLD 1e-9

// Uniform Theory of Diffraction edge operator (wave_edge_diffraction.cpp and .slang).
// Transition-function arguments below this limit use the power series; larger ones use the continued fraction.
#define LUDUS_WAVE_UTD_SERIES_LIMIT 2.25
// Power-series terms: the next term is below 1e-14 of the largest at the limit.
#define LUDUS_WAVE_UTD_SERIES_TERMS 32
// Continued-fraction depth: relative error about 4e-9 at the series limit, decreasing with the argument.
#define LUDUS_WAVE_UTD_FRACTION_DEPTH 48
// Below this magnitude, sin(x)/x uses its two-term Taylor series.
#define LUDUS_WAVE_UTD_SINC_LIMIT 0.0001

// Fraunhofer edge operator (wave_fraunhofer_diffraction.cpp and .slang).
// Shortest segment as a fraction of 1/sqrt(|beta|), the Gaussian-beam kernel's length scale: about 0.5% power
// error on the knife edge and rectangle checks, converging quadratically as it shrinks.
#define LUDUS_WAVE_FRAUNHOFER_SEGMENT_SCALE 0.2
// Segments lengthen in proportion to their distance from the edge's stationary point, where the field oscillates
// fastest relative to its weight, so a segment count grows logarithmically with the edge's extent.
#define LUDUS_WAVE_FRAUNHOFER_SEGMENT_GROWTH 0.1
// Segments on each side of the stationary point. The admitted 1/32 wavelength-to-distance ratio and endpoint
// escapes need at most about 120; more report a propagation capacity failure.
#define LUDUS_WAVE_FRAUNHOFER_SIDE_SEGMENTS 160
// Half-phase magnitude below which (cos(x) - sin(x)/x) / 2x uses its series; float32 keeps 1e-6 relative error.
#define LUDUS_WAVE_FRAUNHOFER_SERIES_LIMIT 0.5
// Coverage samples sit this fraction of a segment's length to either side of it.
#define LUDUS_WAVE_FRAUNHOFER_SIDE_OFFSET 0.015625
