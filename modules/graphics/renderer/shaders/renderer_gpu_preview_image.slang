#include "raster_bindings.slang"

// Read the GPU color buffer in one fullscreen pass, independent of the pixel count.
LUDUS_SHADER_BINDING(0) cbuffer ImageParameters
{
    float4 ExtentPixel;
};
LUDUS_SHADER_BINDING(1) StructuredBuffer<float4> DisplayColors;
struct VertexInput
{
    float2 PositionClip : POSITION0;
};
struct VertexOutput
{
    float4 PositionClip : SV_Position;
};
VertexOutput VertexMain(VertexInput input)
{
    VertexOutput output;
    output.PositionClip = float4(input.PositionClip, 0, 1);
    return output;
}
float4 FragmentMain(VertexOutput input) : SV_Target0
{
    uint pixel = uint(input.PositionClip.y) * uint(ExtentPixel.x) + uint(input.PositionClip.x);
    float4 color = DisplayColors[pixel];
    if (color.w < 0)
        return float4(1, 0, 1, 1);
    if (ExtentPixel.z > 0)
    {
        static const uint CHANNEL_COUNT = 3;
        static const float SRGB_LINEAR_THRESHOLD = .0031308;
        static const float SRGB_POWER_SCALE = 1.055;
        static const float SRGB_GAMMA = 2.4;
        static const float SRGB_POWER_OFFSET = .055;
        static const float SRGB_LINEAR_SCALE = 12.92;
        float3 linear = color.xyz * ExtentPixel.z;
        for (uint channel = 0; channel < CHANNEL_COUNT; ++channel)
            linear[channel] = isfinite(linear[channel]) == true ? linear[channel] / (1 + linear[channel]) : 1;
        color.xyz = select(linear > SRGB_LINEAR_THRESHOLD,
                           SRGB_POWER_SCALE * pow(linear, 1.0 / SRGB_GAMMA) - SRGB_POWER_OFFSET,
                           linear * SRGB_LINEAR_SCALE);
    }
    return float4(color.xyz, 1);
}
