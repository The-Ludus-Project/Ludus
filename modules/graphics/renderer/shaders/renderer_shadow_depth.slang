#pragma once

#include "deferred_binding_slots.inc"

// Portable shadow atlas: red and green hold the high and low bytes of
// light depth (SelectShadowDepth). Blue and alpha store its signed texel slopes.
static const float SHADOW_BYTE_MAXIMUM = 255;
static const float SHADOW_HIGH_BYTE_SCALE = SHADOW_BYTE_MAXIMUM + 1;
static const float SHADOW_DEPTH_CODE_MAXIMUM = SHADOW_HIGH_BYTE_SCALE * SHADOW_HIGH_BYTE_SCALE - 1;
// Leave byte codes 0 and 255 for cleared and older atlas texels. The square-root
// encoding spends more of the remaining precision on small depth differences.
static const float SHADOW_SLOPE_ZERO_CODE = 128;
static const float SHADOW_SLOPE_CODE_RADIUS = 126;

// The depth a shadow fragment stores. Directional, sky visibility and point-light maps store the conventional
// depth they rasterize, z / w. A mesh-light cube face stores linear depth (forward - near) / (range - near), whose
// 16-bit codes resolve the same distance everywhere in the face: its instance colour red, passed as depthTag, holds
// LUDUS_LINEAR_SHADOW_DEPTH_TAG (every other instance colour lies in [0, 1]), and linearDepth is that depth: the
// clip z of the face's range-scaled projection (raster_mesh_light_shadows.h, MakeRasterMeshLightShadowFaceToClip).
float SelectShadowDepth(float rasterizedDepth, float depthTag, float linearDepth)
{
    // Halfway between the tag and zero, so an interpolated tag that varies in its last bits still selects it.
    return depthTag < LUDUS_LINEAR_SHADOW_DEPTH_TAG * 0.5 ? linearDepth : rasterizedDepth;
}

// The linear depth (forward - near) / (range - near) of a conventional depth rasterized by a projection from
// nearRange.x to nearRange.y. A float32 depth near one spans about forward^2 / near * 2^-24 metres per unit in the
// last place, which the mesh-light resolve allows for (LUDUS_MESH_LIGHT_SHADOW_RASTER_DEPTH_ERROR).
float LinearizeShadowDepth(float rasterizedDepth, float2 nearRange)
{
    float nearMeters = nearRange.x;
    float rangeMeters = nearRange.y;
    float denominator = rangeMeters - rasterizedDepth * (rangeMeters - nearMeters);
    return denominator > 0 ? nearMeters * rasterizedDepth / denominator : 1;
}

float4 EncodeShadowDepth(float depth)
{
    float encoded = floor(saturate(depth) * SHADOW_DEPTH_CODE_MAXIMUM + 0.5);
    float high = floor(encoded / SHADOW_HIGH_BYTE_SCALE);
    float2 slopePerTexel = float2(ddx(depth), ddy(depth));
    float2 slopeCode =
        SHADOW_SLOPE_ZERO_CODE + SHADOW_SLOPE_CODE_RADIUS * sign(slopePerTexel) * sqrt(saturate(abs(slopePerTexel)));
    return float4(high, encoded - high * SHADOW_HIGH_BYTE_SCALE, slopeCode) / SHADOW_BYTE_MAXIMUM;
}

float DecodeShadowDepth(float2 code)
{
    const float2 DEPTH_BYTE_WEIGHTS = float2(SHADOW_HIGH_BYTE_SCALE * SHADOW_BYTE_MAXIMUM, SHADOW_BYTE_MAXIMUM);
    return dot(code, DEPTH_BYTE_WEIGHTS) / SHADOW_DEPTH_CODE_MAXIMUM;
}

float2 DecodeShadowSlope(float2 code)
{
    if (any(code <= 0) || any(code >= 1))
    {
        return float2(0);
    }
    float2 slopeRoot = (code * SHADOW_BYTE_MAXIMUM - SHADOW_SLOPE_ZERO_CODE) / SHADOW_SLOPE_CODE_RADIUS;
    return slopeRoot * abs(slopeRoot);
}
