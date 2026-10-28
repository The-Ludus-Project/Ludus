#pragma once

// Case ABI shared by projected_solid_angle_parity_tests.cpp and linearly_transformed_cosine_parity.slang.
// A case is eleven float4 words: seven tangent-space vertex positions (xyz), then (vertex count, first random
// number, second random number, unused), then the three columns of M^-1 (xyz). The kernel writes three float4
// words per case: (made, prepared, integral, transformed vertex count), (sampled, direction xyz) and
// (polygon density, LTC density, scaled determinant, unused); flags are 0 or 1.
#define LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_CASE 11
#define LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_RESULT 3
#define LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_GROUP_SIZE 64
