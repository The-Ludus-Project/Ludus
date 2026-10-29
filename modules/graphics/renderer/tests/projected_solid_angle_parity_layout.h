#pragma once

// Case ABI shared by projected_solid_angle_parity_tests.cpp and projected_solid_angle_parity.slang.
// A case is eight float4 words: seven tangent-space vertex positions (xyz), then
// (vertex count, first random number, second random number, unused). The kernel writes three
// float4 words per case: (prepared, projected solid angle, mirrored, clipped vertex count),
// (sampled, direction xyz) and (density, central case, streamed integral, triangle integral); flags are 0 or 1.
#define LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_CASE 8
#define LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_RESULT 3
#define LUDUS_PROJECTED_SOLID_ANGLE_PARITY_GROUP_SIZE 64
