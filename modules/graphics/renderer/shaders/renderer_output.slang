#include "raster_bindings.slang"
// Explicit scene-linear -> display-linear tone curve (Reinhard or Khronos PBR Neutral) -> optional sRGB encoding.
// Parameters.Output: exposure, curve (0 none, 1 Reinhard, 2 PBR Neutral; ToneCurve in tone_mapping.hpp),
// sRGB encode flag, backdrop flag (above 0.5).
// With the backdrop flag the scene must have been cleared to alpha zero: every pixel the scene left uncovered shows a
// vertical gradient under the premultiplied scene colour, in display space, so the tokens appear exactly.
// UI blends into a linear display intermediate before the final encode-only pass.
struct Parameters
{
    float4 Output;
};
LUDUS_SHADER_BINDING(0) ConstantBuffer<Parameters> parameters : register(b0);
LUDUS_SHADER_BINDING(1) Texture2D<float4> image : register(t1);
LUDUS_SHADER_BINDING(2) SamplerState imageSampler : register(s2);
// Backdrop endpoints in sRGB, the surface.viewportTop and surface.viewport token colours (config/design-tokens.json;
// scripts/python/test_design_tokens.py keeps them equal).
static const float3 BACKDROP_TOP_SRGB = float3(15.0, 21.0, 27.0) / 255.0;
static const float3 BACKDROP_BOTTOM_SRGB = float3(9.0, 12.0, 15.0) / 255.0;
static const float SRGB_LINEAR_THRESHOLD = 0.04045;
static const float SRGB_LINEAR_SLOPE = 12.92;
static const float SRGB_OFFSET = 0.055;
float3 SrgbToLinear(float3 color)
{
    return float3(color.x <= SRGB_LINEAR_THRESHOLD ? color.x / SRGB_LINEAR_SLOPE : pow((color.x + SRGB_OFFSET) / (1 + SRGB_OFFSET), 2.4),
                  color.y <= SRGB_LINEAR_THRESHOLD ? color.y / SRGB_LINEAR_SLOPE : pow((color.y + SRGB_OFFSET) / (1 + SRGB_OFFSET), 2.4),
                  color.z <= SRGB_LINEAR_THRESHOLD ? color.z / SRGB_LINEAR_SLOPE : pow((color.z + SRGB_OFFSET) / (1 + SRGB_OFFSET), 2.4));
}
struct Input
{
    LUDUS_SHADER_LOCATION(0) float4 PositionUvClip : POSITION;
};
struct Varying
{
    float4 PositionClip : SV_Position;
    float2 TexcoordUv : TEXCOORD0;
};
// Khronos PBR Neutral (https://github.com/KhronosGroup/ToneMapping); mirrors ApplyPbrNeutral in tone_mapping.hpp.
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
[shader("vertex")] Varying VertexMain(Input input) {
    Varying output;
    output.PositionClip = float4(input.PositionUvClip.xy, 0, 1);
    output.TexcoordUv = input.PositionUvClip.zw;
    return output;
}[shader("fragment")] float4 FragmentMain(Varying input)
    : SV_Target
{
    float4 scene = image.SampleLevel(imageSampler, input.TexcoordUv, 0);
    float3 color = max(scene.rgb, float3(0)) * parameters.Output.x;
    if (parameters.Output.y > 1.5)
    {
        color = ApplyPbrNeutral(color);
    }
    else if (parameters.Output.y > 0.5)
    {
        color = color / (1 + color);
    }
    if (parameters.Output.z > 0.5)
    {
        color = saturate(color);
        color = float3(color.x <= 0.0031308 ? 12.92 * color.x : 1.055 * pow(color.x, 1.0 / 2.4) - 0.055,
                       color.y <= 0.0031308 ? 12.92 * color.y : 1.055 * pow(color.y, 1.0 / 2.4) - 0.055,
                       color.z <= 0.0031308 ? 12.92 * color.z : 1.055 * pow(color.z, 1.0 / 2.4) - 0.055);
    }
    if (parameters.Output.w > 0.5)
    {
        float3 backdrop = lerp(BACKDROP_TOP_SRGB, BACKDROP_BOTTOM_SRGB, input.TexcoordUv.y);
        backdrop = parameters.Output.z > 0.5 ? backdrop : SrgbToLinear(backdrop);
        color += backdrop * (1 - saturate(scene.a));
    }
    return float4(color, 1);
}
