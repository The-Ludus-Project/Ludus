#pragma once

// Result ABI shared by reference_light_sampling_parity_tests.cpp and reference_light_sampling_parity.slang. Each
// invocation draws its own inputs from FoundationMath's addressed random stream and writes
// LUDUS_REFERENCE_LIGHT_PARITY_RECORDS_PER_INVOCATION uint4 records; floats are stored as their bits.
//   0: technique weight inputs (c_0, c_1, p_0, p_1)
//   1: (visibility, w_0, w_1, unused)
//   2: surrogate inputs (P_E, target, p_src, Z-hat); every second invocation has Z-hat = 0
//   3: surrogate result (P-tilde, unused, unused, unused)
//   4: receiver view direction xyz and perceptual roughness
//   5: receiver base color rgb and metallic
//   6-8: triangle vertices xyz above or around the receiver at the origin, normal +z
//   9: (prepared ? 1 : 0, E_0, E_1, Y(a_0))
//   10: (Y(a_1), specular ? 1 : 0, p_0, p_1) with the densities at the sampled direction
//   11: sampled world direction xyz and (sampled ? 1 : 0)
//   12: (random u_0, random u_1, technique, unused) of that sample
//   13: plane intersection of the sampled direction: (hit ? 1 : 0, distance, barycentric x, barycentric y)
//   14: empty LTC clip: (prepared ? 1 : 0, E_0, E_1, w_1 at a diffuse sample)
//   15: sampler version 3's approximate integrals of record 6-8's triangle: (approximated ? 1 : 0, E_0, E_1, unused)
// Slot 0 holds the results; the renderer's LTC table is bound read-only at this slot.
#define LUDUS_REFERENCE_LIGHT_PARITY_LTC_TABLE_BINDING 1
#define LUDUS_REFERENCE_LIGHT_PARITY_RECORDS_PER_INVOCATION 16
#define LUDUS_REFERENCE_LIGHT_PARITY_GROUP_SIZE 64
#define LUDUS_REFERENCE_LIGHT_PARITY_WEIGHT_INPUT_RECORD 0
#define LUDUS_REFERENCE_LIGHT_PARITY_WEIGHT_RECORD 1
#define LUDUS_REFERENCE_LIGHT_PARITY_SURROGATE_INPUT_RECORD 2
#define LUDUS_REFERENCE_LIGHT_PARITY_SURROGATE_RECORD 3
#define LUDUS_REFERENCE_LIGHT_PARITY_VIEW_RECORD 4
#define LUDUS_REFERENCE_LIGHT_PARITY_MATERIAL_RECORD 5
#define LUDUS_REFERENCE_LIGHT_PARITY_VERTEX_RECORD 6
#define LUDUS_REFERENCE_LIGHT_PARITY_POLYGON_RECORD 9
#define LUDUS_REFERENCE_LIGHT_PARITY_DENSITY_RECORD 10
#define LUDUS_REFERENCE_LIGHT_PARITY_DIRECTION_RECORD 11
#define LUDUS_REFERENCE_LIGHT_PARITY_RANDOM_RECORD 12
#define LUDUS_REFERENCE_LIGHT_PARITY_PLANE_RECORD 13
#define LUDUS_REFERENCE_LIGHT_PARITY_EMPTY_CLIP_RECORD 14
#define LUDUS_REFERENCE_LIGHT_PARITY_APPROXIMATE_RECORD 15
// The empty-clip LTC: M^-1 = [[1, 0, 0], [0, 1, 0], [1, 0, 0.2]] is zero where x + 0.2 z <= 0.
#define LUDUS_REFERENCE_LIGHT_PARITY_EMPTY_CLIP_M22 0.2
