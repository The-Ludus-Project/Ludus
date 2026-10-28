#pragma once

// Case ABI shared by projected_solid_angle_parity_tests.cpp and ltc_table_parity.slang. A case is two float4 words:
// (perceptual roughness, tangent-space view direction xyz) and (Schlick F0 rgb, unused). The kernel reads the
// renderer's LTC table at binding 2 and writes four float4 words per case: the three columns of the looked-up inverse
// transform (xyz) and (norm, Fresnel norm, unused, unused).
#define LUDUS_LTC_TABLE_PARITY_WORDS_PER_CASE 2
#define LUDUS_LTC_TABLE_PARITY_WORDS_PER_RESULT 4
#define LUDUS_LTC_TABLE_PARITY_GROUP_SIZE 64
