// Portable conventional-depth shadow storage: 16-bit depth encoded in linear RGBA8.
// This avoids assuming sampled depth/comparison-sampler support in the R1 profile.
#include "renderer_shadow_depth.slang"
struct Input
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 row0 : TEXCOORD0;
    [[vk::location(2)]] float4 row1 : TEXCOORD1;
    [[vk::location(3)]] float4 row2 : TEXCOORD2;
    [[vk::location(4)]] float4 row3 : TEXCOORD3;
    [[vk::location(5)]] float4 color : COLOR0;
};
struct Varying { float4 position : SV_Position; };
[shader("vertex")]
Varying vertexMain(Input input)
{
    Varying output; float4 p = float4(input.position,1);
    output.position = float4(dot(input.row0,p),dot(input.row1,p),dot(input.row2,p),dot(input.row3,p));
    return output;
}
[shader("fragment")]
float4 fragmentMain(Varying input) : SV_Target
{
    return EncodeShadowDepth(input.position.z);
}
