#include "raster_bindings.slang"
// Explicit scene-linear -> display-linear tone curve (Reinhard or Khronos PBR Neutral) -> optional sRGB encoding.
// Parameters.Output: exposure, curve (0 none, 1 Reinhard, 2 PBR Neutral; ToneCurve in tone_mapping.hpp),
// sRGB encode flag.
// UI blends into a linear display intermediate before the final encode-only pass.
struct Parameters
{
    float4 Output;
};
LUDUS_SHADER_BINDING(0) ConstantBuffer<Parameters> parameters : register(b0);
LUDUS_SHADER_BINDING(1) Texture2D<float4> image : register(t1);
LUDUS_SHADER_BINDING(2) SamplerState imageSampler : register(s2);
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
    float3 color = max(image.SampleLevel(imageSampler, input.TexcoordUv, 0).rgb, float3(0)) * parameters.Output.x;
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
    return float4(color, 1);
}
