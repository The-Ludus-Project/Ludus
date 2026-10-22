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
    return float4(DisplayColors[pixel].xyz, 1);
}
