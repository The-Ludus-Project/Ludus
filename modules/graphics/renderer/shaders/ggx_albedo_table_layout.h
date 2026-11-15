#pragma once

// Layout of RT1's GGX directional-albedo table for ReferenceMaterialProfile::MultipleScatteringGgx, shared by the CPU
// lookup (src/ggx_albedo_table.cpp), the shader lookup (ggx_albedo_table.slang) and the GPU buffer that holds the
// table. tools/ggx-albedo/generate_ggx_albedo_table.py writes data/ggx_albedo_table.bin in this layout;
// docs/architecture/renderer-systems.md#multiple-scattering-ggx-profile describes the profile, the table and how to
// regenerate it.
//
// The table is LUDUS_GGX_ALBEDO_TABLE_FLOAT_COUNT float32 values, read by shaders as float4 entries. Value
// r * LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT + c is the unit-Fresnel directional albedo of the single-scattering lobe at
// alpha = (r / (rows - 1))^2 and the view whose stretched inclination theta_s = atan2(alpha sin theta, cos theta)
// satisfies 1 - sqrt(1 - 2 theta_s / pi) = c / (columns - 1). Value LUDUS_GGX_ALBEDO_TABLE_DEFICIT_OFFSET + r holds
// the row's average deficit 2 * integral of (1 - E(mu)) mu dmu, integrated from the same bilinear interpolant; it
// stays precise in float32 where the average albedo approaches one.

#define LUDUS_GGX_ALBEDO_TABLE_VERSION 1
#define LUDUS_GGX_ALBEDO_TABLE_ROUGHNESS_COUNT 64
#define LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT 64
#define LUDUS_GGX_ALBEDO_TABLE_DEFICIT_OFFSET                                                                          \
    (LUDUS_GGX_ALBEDO_TABLE_ROUGHNESS_COUNT * LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT)
#define LUDUS_GGX_ALBEDO_TABLE_FLOAT_COUNT                                                                             \
    (LUDUS_GGX_ALBEDO_TABLE_DEFICIT_OFFSET + LUDUS_GGX_ALBEDO_TABLE_ROUGHNESS_COUNT)
#define LUDUS_GGX_ALBEDO_TABLE_ENTRY_FLOATS 4
#define LUDUS_GGX_ALBEDO_TABLE_ENTRY_COUNT (LUDUS_GGX_ALBEDO_TABLE_FLOAT_COUNT / LUDUS_GGX_ALBEDO_TABLE_ENTRY_FLOATS)
// Composite Simpson intervals over the view cosine for a conductor's cosine-weighted average Fresnel reflectance.
#define LUDUS_GGX_FRESNEL_AVERAGE_INTERVALS 16
// An average albedo deficit at or below this value adds no compensation lobe: the surface is effectively a mirror.
#define LUDUS_GGX_MINIMUM_AVERAGE_DEFICIT 1e-6f
