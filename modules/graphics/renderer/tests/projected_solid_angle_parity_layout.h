#pragma once

// Case ABI shared by projected_solid_angle_parity_tests.cpp and projected_solid_angle_parity.slang.
// A case is eleven float4 words: seven tangent-space vertex positions (xyz), then (vertex count,
// first random number, second random number, unused), then the three rows of an LTC's M^-1 (xyz).
// The kernel writes five float4 words per case: (prepared, projected solid angle, mirrored, clipped
// vertex count), (sampled, direction xyz), (density, central case, unused, unused), (LTC prepared,
// LTC integral, LTC sampled, LTC density) and (LTC direction xyz, unused); flags are 0 or 1.
#define LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_CASE 11
#define LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_RESULT 5
#define LUDUS_PROJECTED_SOLID_ANGLE_PARITY_GROUP_SIZE 64
// First word of the LTC matrix rows within a case.
#define LUDUS_PROJECTED_SOLID_ANGLE_PARITY_LTC_WORD 8
