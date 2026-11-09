#pragma once

// Portable shadow atlas: red and green hold the high and low bytes of
// conventional light depth. Blue and alpha store its signed texel slopes.
static const float SHADOW_BYTE_MAXIMUM = 255;
static const float SHADOW_HIGH_BYTE_SCALE = SHADOW_BYTE_MAXIMUM + 1;
static const float SHADOW_DEPTH_CODE_MAXIMUM = SHADOW_HIGH_BYTE_SCALE * SHADOW_HIGH_BYTE_SCALE - 1;
// Leave byte codes 0 and 255 for cleared and older atlas texels. The square-root
// encoding spends more of the remaining precision on small depth differences.
static const float SHADOW_SLOPE_ZERO_CODE = 128;
static const float SHADOW_SLOPE_CODE_RADIUS = 126;

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
