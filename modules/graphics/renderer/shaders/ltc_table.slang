#pragma once
#include "math.slang"

// Lookup of the renderer's linearly transformed cosine (LTC) table, fitted by tools/ltc-fit into data/ltc_table.bin
// with its layout in ltc_table_layout.h. The table is too large for shader constants: the includer defines
// LUDUS_LTC_TABLE_BINDING, and binds the renderer's GPU copy, LUDUS_LTC_TABLE_ENTRY_COUNT float4 entries, at that slot
// of the raster binding vocabulary (raster_bindings.slang). Include it after the declarations of every lower binding:
// Slang numbers Metal buffers in declaration order. The CPU lookup with the same arithmetic
// is src/ltc_table.cpp. Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, "Real-Time
// Polygonal-Light Shading with Linearly Transformed Cosines", ACM Transactions on Graphics 35(4), Article 41, 2016
// (SIGGRAPH 2016), section 4, https://doi.org/10.1145/2897824.2925895, and to Christoph Peters, "BRDF Importance
// Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021, section 4.1,
// https://doi.org/10.1145/3450626.3459672, for the F0 axis. Pair it with FoundationMath's
// linearly_transformed_cosine.slang. See docs/development/ltc-table.md.

#include "ltc_table_layout.h"
#include "raster_bindings.slang"

#ifndef LUDUS_LTC_TABLE_BINDING
#    error                                                                                                             \
        "Define LUDUS_LTC_TABLE_BINDING, the slot of the renderer's LTC table buffer, before including ltc_table.slang"
#endif
LUDUS_SHADER_BINDING(LUDUS_LTC_TABLE_BINDING) StructuredBuffer<float4> LudusLtcTable;

// Rec.709 luminance weights, with which Peters' renderer reduces an RGB F0 to the scalar that selects a slice.
static const float3 LTC_FRESNEL_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);

// The specular LTC of one receiver; matrices are three float3 columns.
struct SpecularLtc
{
    // M^-1 up to a positive scale, in the caller's tangent frame (+z the shading normal).
    float3 InverseTransform[3];
    // Directional albedo of the specular lobe with a Fresnel factor of one.
    float Norm;
    // The (1 - v.h)^5-weighted albedo.
    float FresnelNorm;
};

// Bilinear interpolation of the four entries around (row, column) of the roughness x view grid starting at offset.
float4 InterpolateLtcTable(uint offset, uint row, float rowFraction, uint column, float columnFraction)
{
    uint low = offset + row * LUDUS_LTC_TABLE_VIEW_COUNT + column;
    uint high = low + LUDUS_LTC_TABLE_VIEW_COUNT;
    float4 lowRow = lerp(LudusLtcTable[low], LudusLtcTable[low + 1], columnFraction);
    float4 highRow = lerp(LudusLtcTable[high], LudusLtcTable[high + 1], columnFraction);
    return lerp(lowRow, highRow, rowFraction);
}
// The F0 slice nearest the clamped Rec.709 luminance of f0, as in Peters' renderer.
uint GetLtcTableFresnelSlice(float3 f0)
{
    float luminance = saturate(dot(f0, LTC_FRESNEL_LUMINANCE_WEIGHTS));
    return min(uint(luminance * float(LUDUS_LTC_TABLE_FRESNEL_COUNT - 1) + 0.5),
               uint(LUDUS_LTC_TABLE_FRESNEL_COUNT - 1));
}
// Look up the specular LTC for a perceptual roughness, a unit view direction in the tangent frame and Schlick's F0 per
// channel. Roughness is clamped to [LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS, 1] and the view cosine to [0, 1]; a view along
// the normal uses +x. The transform comes from the F0 slice of GetLtcTableFresnelSlice; roughness and view are
// interpolated bilinearly within it.
SpecularLtc LookupSpecularLtc(float roughness, float3 viewTangent, float3 f0)
{
    float rowCoordinate =
        saturate((roughness - LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS) / (1.0 - LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS)) *
        float(LUDUS_LTC_TABLE_ROUGHNESS_COUNT - 1);
    float sine = sqrt(viewTangent.x * viewTangent.x + viewTangent.y * viewTangent.y);
    float theta = atan2(sine, max(viewTangent.z, 0.0));
    float columnCoordinate = saturate(theta / (0.5 * PI)) * float(LUDUS_LTC_TABLE_VIEW_COUNT - 1);
    uint row = min(uint(rowCoordinate), uint(LUDUS_LTC_TABLE_ROUGHNESS_COUNT - 2));
    uint column = min(uint(columnCoordinate), uint(LUDUS_LTC_TABLE_VIEW_COUNT - 2));
    float rowFraction = rowCoordinate - float(row);
    float columnFraction = columnCoordinate - float(column);
    uint transformOffset = GetLtcTableFresnelSlice(f0) * uint(LUDUS_LTC_TABLE_SLICE_ENTRY_COUNT);
    float4 transform = InterpolateLtcTable(transformOffset, row, rowFraction, column, columnFraction);
    float4 moments = InterpolateLtcTable(uint(LUDUS_LTC_TABLE_MOMENT_OFFSET), row, rowFraction, column, columnFraction);
    // M^-1 in the table's frame is [[1, 0, M02], [0, M11, 0], [M20, 0, M22]]; the tangent-space inverse is M^-1 R^T for
    // the rotation R about z by the view's azimuth.
    float3 inverseCanonical[3];
    inverseCanonical[0] = float3(1.0, 0.0, transform.z);
    inverseCanonical[1] = float3(0.0, transform.y, 0.0);
    inverseCanonical[2] = float3(transform.x, 0.0, transform.w);
    bool hasAzimuth = sine > 0.0;
    float cosineAzimuth = hasAzimuth == true ? viewTangent.x / sine : 1.0;
    float sineAzimuth = hasAzimuth == true ? viewTangent.y / sine : 0.0;
    SpecularLtc result;
    result.InverseTransform[0] = inverseCanonical[0] * cosineAzimuth - inverseCanonical[1] * sineAzimuth;
    result.InverseTransform[1] = inverseCanonical[0] * sineAzimuth + inverseCanonical[1] * cosineAzimuth;
    result.InverseTransform[2] = inverseCanonical[2];
    result.Norm = moments.x;
    result.FresnelNorm = moments.y;
    return result;
}
// Specular albedo with Schlick's Fresnel per channel: F0 * (Norm - FresnelNorm) + FresnelNorm.
float3 ComputeSpecularLtcAlbedo(SpecularLtc ltc, float3 f0)
{
    return f0 * (ltc.Norm - ltc.FresnelNorm) + ltc.FresnelNorm;
}
