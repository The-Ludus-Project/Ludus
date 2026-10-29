#pragma once

// Result ABI shared by polygonal_light_sampling_parity_tests.cpp and polygonal_light_sampling_estimators.slang. Each
// invocation draws one receiver, material and emissive triangle from FoundationMath's addressed random stream, writes
// those inputs and the template's four estimates for them as LUDUS_POLYGONAL_LIGHT_ESTIMATORS_RECORDS_PER_INVOCATION
// uint4 records; floats are stored as their bits. The receiver is at the World origin.
//   0: (shading normal xyz, metallic)
//   1: (view direction xyz, roughness)
//   2: (base color rgb, MIS visibility v)
//   3: (triangle vertex 0 xyz, 1 if the emission is two-sided, else 0)
//   4: (vertex 1 xyz, projected solid angle sample x)
//   5: (vertex 2 xyz, projected solid angle sample y)
//   6: (emission rgb, LTC sample x)
//   7: (LTC sample y, E_0, E_1, 1 if the triangle was prepared, else 0)
//   8 to 11: the radiance of each estimator in output order (rgb, unused)
//   12: (c_0 rgb, 1 if the receiver has a specular LTC, else 0)
//   13: (c_1 rgb, unused)
// The results occupy LUDUS_GRAPH_BUFFER0_BINDING (renderer_gpu_preview_layout.h), as in graph wrappers, and the
// renderer's LTC table is bound read-only at the next slot; ltc_table.slang gives it an explicit native register, so
// Metal and Vulkan number it alike.
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_LTC_TABLE_BINDING 7
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_RECORDS_PER_INVOCATION 14
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_GROUP_SIZE 64
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_NORMAL_RECORD 0
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VIEW_RECORD 1
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_MATERIAL_RECORD 2
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX0_RECORD 3
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX1_RECORD 4
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX2_RECORD 5
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_EMISSION_RECORD 6
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_INTEGRAL_RECORD 7
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_FIRST_RADIANCE_RECORD 8
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_DIFFUSE_COEFFICIENT_RECORD 12
#define LUDUS_POLYGONAL_LIGHT_ESTIMATORS_SPECULAR_COEFFICIENT_RECORD 13
