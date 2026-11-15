#pragma once
#include "math.slang"

// Lookup of RT1's GGX directional-albedo table for ReferenceMaterialProfile::MultipleScatteringGgx, generated offline
// into data/ggx_albedo_table.bin with its layout in ggx_albedo_table_layout.h. The includer defines
// LUDUS_GGX_ALBEDO_TABLE_BINDING and binds the renderer's GPU copy, LUDUS_GGX_ALBEDO_TABLE_ENTRY_COUNT float4 entries,
// at that slot of the raster binding vocabulary (raster_bindings.slang). Its explicit native register keeps Metal
// buffer numbering independent of declaration order. src/ggx_albedo_table.cpp is the CPU lookup with the same
// arithmetic. Thanks to Christopher Kulla and Alejandro Conty, "Revisiting Physically Based Shading at Imageworks",
// SIGGRAPH 2017, slides 9-14,
// https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_slides_v2.pdf#page=12.
// See docs/architecture/renderer-systems.md#multiple-scattering-ggx-profile.

#include "ggx_albedo_table_layout.h"
#include "raster_bindings.slang"

#ifndef LUDUS_GGX_ALBEDO_TABLE_BINDING
#    error                                                                                                             \
        "Define LUDUS_GGX_ALBEDO_TABLE_BINDING, the slot of the GGX albedo table, before including ggx_albedo_table.slang"
#endif
LUDUS_SHADER_BINDING(LUDUS_GGX_ALBEDO_TABLE_BINDING)
StructuredBuffer<float4> LudusGgxAlbedoTable LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_GGX_ALBEDO_TABLE_BINDING);

float ReadGgxAlbedoTable(uint index)
{
    return LudusGgxAlbedoTable[index / LUDUS_GGX_ALBEDO_TABLE_ENTRY_FLOATS]
                              [index % LUDUS_GGX_ALBEDO_TABLE_ENTRY_FLOATS];
}
// Fractional row of alpha: rows are linear in sqrt(alpha) over [0, 1].
float GetGgxAlbedoRowCoordinate(float alpha)
{
    return sqrt(saturate(alpha)) * float(LUDUS_GGX_ALBEDO_TABLE_ROUGHNESS_COUNT - 1);
}
// Unit-Fresnel directional albedo of RT1's single-scattering GGX lobe for a cosine to the shading normal, bilinearly
// interpolated over sqrt(alpha) and the stretched-view coordinate, clamped to [0, 1].
float LookupGgxDirectionalAlbedo(float alpha, float cosine)
{
    float clampedAlpha = saturate(alpha);
    float clampedCosine = saturate(cosine);
    float sine = sqrt(max(0, 1 - clampedCosine * clampedCosine));
    float stretchedInclination = atan2(clampedAlpha * sine, clampedCosine);
    float x = 1 - sqrt(max(0, 1 - stretchedInclination / (.5 * PI)));
    float columnCoordinate = saturate(x) * float(LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT - 1);
    float rowCoordinate = GetGgxAlbedoRowCoordinate(clampedAlpha);
    uint row = min(uint(rowCoordinate), uint(LUDUS_GGX_ALBEDO_TABLE_ROUGHNESS_COUNT - 2));
    uint column = min(uint(columnCoordinate), uint(LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT - 2));
    float rowFraction = rowCoordinate - float(row);
    float columnFraction = columnCoordinate - float(column);
    uint low = row * LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT + column;
    uint high = low + LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT;
    float lowRow = lerp(ReadGgxAlbedoTable(low), ReadGgxAlbedoTable(low + 1), columnFraction);
    float highRow = lerp(ReadGgxAlbedoTable(high), ReadGgxAlbedoTable(high + 1), columnFraction);
    return saturate(lerp(lowRow, highRow, rowFraction));
}
// Average deficit 2 * integral of (1 - E(mu)) mu dmu, linearly interpolated over sqrt(alpha).
float LookupGgxAverageDeficit(float alpha)
{
    float rowCoordinate = GetGgxAlbedoRowCoordinate(alpha);
    uint row = min(uint(rowCoordinate), uint(LUDUS_GGX_ALBEDO_TABLE_ROUGHNESS_COUNT - 2));
    uint low = LUDUS_GGX_ALBEDO_TABLE_DEFICIT_OFFSET + row;
    return saturate(lerp(ReadGgxAlbedoTable(low), ReadGgxAlbedoTable(low + 1), rowCoordinate - float(row)));
}
