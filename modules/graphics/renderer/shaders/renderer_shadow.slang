// Portable shadow depth storage: 16-bit depth encoded in linear RGBA8 (renderer_shadow_depth.slang).
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

// The renderer pairs this fragment with its flat vertex shader (renderer_flat.slang), so the varyings match its
// position and instance colour.
struct Varying
{
    float4 PositionClip : SV_Position;
    // The instance colour: a mesh-light cube face's depth tag, near distance and range (SelectShadowDepth).
    float4 Color : COLOR0;
};

[shader("vertex")] Varying VertexMain(Input input) {
    Varying output;
    float4 positionMesh = float4(input.PositionMesh, 1);
    output.PositionClip = float4(dot(input.MeshToClipRow0, positionMesh),
                                 dot(input.MeshToClipRow1, positionMesh),
                                 dot(input.MeshToClipRow2, positionMesh),
                                 dot(input.MeshToClipRow3, positionMesh));
    output.Color = input.Color;
    return output;
}

    [shader("fragment")] float4 FragmentMain(Varying input)
    : SV_Target
{
    float depth = input.PositionClip.z;
    return EncodeShadowDepth(SelectShadowDepth(depth, input.Color.x, LinearizeShadowDepth(depth, input.Color.yz)));
}
