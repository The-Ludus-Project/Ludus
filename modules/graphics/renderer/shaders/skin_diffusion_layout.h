#pragma once

// Private host/shader schema of the raster skin diffusion passes (renderer_skin_diffusion.slang,
// src/internal/skin_diffusion.h). Kernel words are four binary32 values.
//
// The separable kernel is Jimenez et al.'s pre-integrated kernel (Separable Subsurface Scattering, section 4) of
// the Christensen-Burley normalized diffusion profile, sampled at taps shared by the three channels and placed by
// the dominant channel's energy (section 6).

// Taps on each side of the centre tap; the kernel has 2 * SIDE + 1 taps, the paper's seven.
#define LUDUS_SKIN_KERNEL_SIDE_TAPS 3
#define LUDUS_SKIN_KERNEL_TAPS (2 * LUDUS_SKIN_KERNEL_SIDE_TAPS + 1)
// Strip-energy table: entries at half-widths 2^x, x evenly spaced over [LOG2_MIN, LOG2_MAX], in units of the
// scattering distance.
#define LUDUS_SKIN_ENERGY_ENTRIES 32
#define LUDUS_SKIN_ENERGY_LOG2_MIN (-8.0)
#define LUDUS_SKIN_ENERGY_LOG2_MAX 7.0
#define LUDUS_SKIN_ENERGY_WORDS (LUDUS_SKIN_ENERGY_ENTRIES / 4)
// Word holding the side taps' offsets (x, y, z) in units of the dominant channel's scattering distance.
#define LUDUS_SKIN_KERNEL_OFFSET_WORD LUDUS_SKIN_ENERGY_WORDS
// Word holding the bin half-widths (x: centre bin, then one per side tap) in the same units.
#define LUDUS_SKIN_KERNEL_BOUNDARY_WORD (LUDUS_SKIN_ENERGY_WORDS + 1)
#define LUDUS_SKIN_KERNEL_WORDS (LUDUS_SKIN_ENERGY_WORDS + 2)
// Fraction of the dominant channel's 1D kernel energy inside the outermost bin; the rest is truncated and the
// weights renormalized.
#define LUDUS_SKIN_KERNEL_CAPTURED_ENERGY 0.99
// A tap belongs to the centre pixel's surface when its world position lies within this fraction of its lateral
// offset from the centre's tangent plane, about 27 degrees of curvature. Silhouettes and surfaces behind or in front
// fail the test and are left out of the blur.
#define LUDUS_SKIN_SURFACE_SLOPE_LIMIT 0.5

// Bindings of the diffusion passes. The blur reads the lighting pass's diffuse target; the composite reads the
// blurred diffuse and the remaining radiance. Both read a skin pixel's profile from the G-buffer's base/metallic
// target and the profile's scattering distance from the view's profile table.
#define LUDUS_SKIN_SOURCE_BINDING 0
#define LUDUS_SKIN_RADIANCE_BINDING 1
#define LUDUS_SKIN_NORMAL_BINDING 2
#define LUDUS_SKIN_BASE_BINDING 3
#define LUDUS_SKIN_DEPTH_BINDING 4
#define LUDUS_SKIN_VIEW_BINDING 5
#define LUDUS_SKIN_KERNEL_BINDING 6
#define LUDUS_SKIN_PROFILE_BINDING 7
#define LUDUS_SKIN_BINDING_COUNT 8
