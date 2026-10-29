#pragma once

// Result ABIs shared by ris_projected_solid_angle_weight_tests.cpp, ris_projected_solid_angle_weights.slang and
// ris_projected_solid_angle_clipped_receivers.slang. Each invocation draws its own inputs from FoundationMath's
// addressed random stream; floats are stored as their bits. The weights kernel writes
// LUDUS_RIS_WEIGHTS_RECORDS_PER_INVOCATION uint4 records:
//   0: clamped optimal MIS on drawn inputs: (w_0, w_1, p_0, p_1)
//   1: the same call's coefficients and visibility: (c_0, c_1, v, unused)
//   2: power heuristic both ways: (weight(p, q), weight(q, p), p, q)
//   3: surrogate inputs: (P_E, target, p_src, Z-hat); every second invocation has Z-hat = 0
//   4: surrogate result: (P-tilde, M, unused, unused)
//   5: empty LTC clip: (prepared ? 1 : 0, E_0, E_1, p_1 at the diffuse sample)
//   6: empty LTC clip weights: (w_0, w_1, diffuse sampled ? 1 : 0, p_0 at the diffuse sample)
//   7: receiver chain: (w_0 + w_1, p_0, p_1, technique sampled ? 1 : 0) for a drawn receiver, LTC and triangle
//   8: the same triangle's exact and approximate target integrals: (E_0, approximate E_0, E_1, approximate E_1)
// The clipped-receivers kernel draws a receiver viewed from below the horizon of its shading normal +z, which Peters
// 2021, section 6.1 clips towards the view, and a triangle, and writes LUDUS_RIS_CLIPPED_RECORDS_PER_INVOCATION
// records:
//   0: (view xyz, roughness)
//   1: (base color rgb, metallic)
//   2: (triangle vertex 0 xyz, LUDUS_RIS_CLIPPED_* flags)
//   3: (triangle vertex 1 xyz, Y(a_0))
//   4: (triangle vertex 2 xyz, Y(a_1))
//   5: (clipped shading normal xyz, w_0 + w_1)
//   6: (E_0, E_1, p_0, p_1) at the sampled direction
//   7: (sampled world direction xyz, MIS visibility v)
// In both kernels slot 0 holds the results; the renderer's LTC table is bound read-only at this slot.
#define LUDUS_RIS_WEIGHTS_LTC_TABLE_BINDING 1
#define LUDUS_RIS_WEIGHTS_RECORDS_PER_INVOCATION 9
#define LUDUS_RIS_WEIGHTS_GROUP_SIZE 64
#define LUDUS_RIS_WEIGHTS_CLAMPED_RECORD 0
#define LUDUS_RIS_WEIGHTS_CLAMPED_INPUT_RECORD 1
#define LUDUS_RIS_WEIGHTS_POWER_RECORD 2
#define LUDUS_RIS_WEIGHTS_SURROGATE_INPUT_RECORD 3
#define LUDUS_RIS_WEIGHTS_SURROGATE_RECORD 4
#define LUDUS_RIS_WEIGHTS_EMPTY_CLIP_RECORD 5
#define LUDUS_RIS_WEIGHTS_EMPTY_CLIP_WEIGHT_RECORD 6
#define LUDUS_RIS_WEIGHTS_RECEIVER_RECORD 7
#define LUDUS_RIS_WEIGHTS_TARGET_RECORD 8
#define LUDUS_RIS_CLIPPED_RECORDS_PER_INVOCATION 8
#define LUDUS_RIS_CLIPPED_VIEW_RECORD 0
#define LUDUS_RIS_CLIPPED_MATERIAL_RECORD 1
#define LUDUS_RIS_CLIPPED_TRIANGLE_RECORD 2
#define LUDUS_RIS_CLIPPED_NORMAL_RECORD 5
#define LUDUS_RIS_CLIPPED_POLYGON_RECORD 6
#define LUDUS_RIS_CLIPPED_DIRECTION_RECORD 7
// Bits of record 2's fourth word.
#define LUDUS_RIS_CLIPPED_PREPARED 1
#define LUDUS_RIS_CLIPPED_HAS_SPECULAR 2
#define LUDUS_RIS_CLIPPED_SAMPLED 4
