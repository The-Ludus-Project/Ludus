#pragma once
// Scene-linear -> display conversion shared by the viewport presentation (renderer_output.slang) and the graph
// ToneMap operation fused into the clustered deferred resolve (renderer_deferred_lighting.slang). The curve codes
// are ToneCurve in tone_mapping.hpp: 0 none, 1 Reinhard, 2 Khronos PBR Neutral.
static const uint DISPLAY_CURVE_NONE = 0;
static const uint DISPLAY_CURVE_REINHARD = 1;
static const uint DISPLAY_CURVE_PBR_NEUTRAL = 2;

// Thanks to the Khronos 3D Formats Working Group (Emmett Lalish et al.), "Khronos PBR Neutral Tone Mapper"
// (https://github.com/KhronosGroup/ToneMapping), reference implementation; mirrors ApplyPbrNeutral in
// tone_mapping.hpp.
static const float PBR_NEUTRAL_FRESNEL = 0.04;
static const float PBR_NEUTRAL_START = 0.8 - PBR_NEUTRAL_FRESNEL;
static const float PBR_NEUTRAL_DESATURATION = 0.15;
float3 ApplyPbrNeutral(float3 color)
{
    // Infinite radiance would produce infinity * 0; it maps to white like the Reinhard path's guard.
    if (all(isfinite(color)) == false)
    {
        return float3(1);
    }
    float smallest = min(color.x, min(color.y, color.z));
    float offset = smallest <= 2 * PBR_NEUTRAL_FRESNEL ? smallest - smallest * smallest / (4 * PBR_NEUTRAL_FRESNEL)
                                                       : PBR_NEUTRAL_FRESNEL;
    color -= offset;
    float peak = max(color.x, max(color.y, color.z));
    if (peak <= PBR_NEUTRAL_START)
    {
        return color;
    }
    float headroom = 1 - PBR_NEUTRAL_START;
    float newPeak = 1 - headroom * headroom / (peak + 1 - 2 * PBR_NEUTRAL_START);
    float mixAmount = 1 / (PBR_NEUTRAL_DESATURATION * (peak - newPeak) + 1);
    return color * (newPeak / peak) * mixAmount + newPeak * (1 - mixAmount);
}
// Per-channel x / (1 + x). A channel that overflowed to infinity tends to one rather than infinity / infinity.
float3 ApplyReinhard(float3 color)
{
    return float3(isfinite(color.x) == true ? color.x / (1 + color.x) : 1,
                  isfinite(color.y) == true ? color.y / (1 + color.y) : 1,
                  isfinite(color.z) == true ? color.z / (1 + color.z) : 1);
}
// Exposure, then the selected curve, of nonnegative scene-linear colour; display-linear and unclamped.
float3 ApplyDisplayCurve(float3 sceneColor, float exposure, uint curve)
{
    float3 color = max(sceneColor, float3(0)) * exposure;
    if (curve == DISPLAY_CURVE_PBR_NEUTRAL)
    {
        return ApplyPbrNeutral(color);
    }
    return curve == DISPLAY_CURVE_REINHARD ? ApplyReinhard(color) : color;
}
// IEC 61966-2-1 sRGB encoding of display-linear colour in [0, 1].
float3 EncodeSrgb(float3 color)
{
    return float3(color.x <= 0.0031308 ? 12.92 * color.x : 1.055 * pow(color.x, 1.0 / 2.4) - 0.055,
                  color.y <= 0.0031308 ? 12.92 * color.y : 1.055 * pow(color.y, 1.0 / 2.4) - 0.055,
                  color.z <= 0.0031308 ? 12.92 * color.z : 1.055 * pow(color.z, 1.0 / 2.4) - 0.055);
}
// Exposure, curve and sRGB encoding into the [0, 1] display values that an RGBA8 target stores.
float3 MapSceneToDisplay(float3 sceneColor, float exposure, uint curve)
{
    return EncodeSrgb(saturate(ApplyDisplayCurve(sceneColor, exposure, curve)));
}
