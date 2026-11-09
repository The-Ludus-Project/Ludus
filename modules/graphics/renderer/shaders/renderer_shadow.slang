// Portable conventional-depth shadow storage: 16-bit depth encoded in linear RGBA8.
// This avoids assuming sampled depth/comparison-sampler support in the R1 profile.
#include "raster_bindings.slang"
#include "renderer_shadow_depth.slang"

struct Input
{
    LUDUS_SHADER_LOCATION(0) float3 PositionMesh : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 MeshToClipRow0 : TEXCOORD0;
    LUDUS_SHADER_LOCATION(2) float4 MeshToClipRow1 : TEXCOORD1;
    LUDUS_SHADER_LOCATION(3) float4 MeshToClipRow2 : TEXCOORD2;
    LUDUS_SHADER_LOCATION(4) float4 MeshToClipRow3 : TEXCOORD3;
    LUDUS_SHADER_LOCATION(5) float4 Color : COLOR0;
};

struct Varying
{
    float4 PositionClip : SV_Position;
};

[shader("vertex")] Varying VertexMain(Input input) {
    Varying output;
    float4 positionMesh = float4(input.PositionMesh, 1);
    output.PositionClip = float4(dot(input.MeshToClipRow0, positionMesh),
                                 dot(input.MeshToClipRow1, positionMesh),
                                 dot(input.MeshToClipRow2, positionMesh),
                                 dot(input.MeshToClipRow3, positionMesh));
    return output;
}

    [shader("fragment")] float4 FragmentMain(Varying input)
    : SV_Target
{
    return EncodeShadowDepth(input.PositionClip.z);
}
