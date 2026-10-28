#pragma once

// Case ABI shared by projected_solid_angle_parity_tests.cpp and ltc_table_parity.slang. A case is one float4:
// (perceptual roughness, tangent-space view direction xyz). The kernel writes four float4 words per case: the three
// columns of the looked-up inverse transform (xyz) and (norm, Fresnel norm, unused, unused).
#define LUDUS_LTC_TABLE_PARITY_WORDS_PER_CASE 1
#define LUDUS_LTC_TABLE_PARITY_WORDS_PER_RESULT 4
#define LUDUS_LTC_TABLE_PARITY_GROUP_SIZE 64
