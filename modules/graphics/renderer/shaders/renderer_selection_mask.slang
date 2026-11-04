#include "raster_bindings.slang"
// Selection outline, mask pass. The selected mesh is rasterized into a single-colour mask that is white where the
// mesh covers a pixel and black elsewhere. No depth test is used, so another object in front does not hide the
// outline. The edge pass in renderer_selection_outline.slang turns the mask into the outline.
//
// ABI: renderer_flat's vertex stream and instance stream, without the colour. No uniforms or textures.
//   PositionMesh    mesh-space position
//   Row0..Row3      mesh-to-clip rows
struct Input
{
    LUDUS_SHADER_LOCATION(0) float3 PositionMesh : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 Row0 : TEXCOORD0;
    LUDUS_SHADER_LOCATION(2) float4 Row1 : TEXCOORD1;
    LUDUS_SHADER_LOCATION(3) float4 Row2 : TEXCOORD2;
    LUDUS_SHADER_LOCATION(4) float4 Row3 : TEXCOORD3;
};
struct Varying
{
    float4 PositionClip : SV_Position;
};
[shader("vertex")] Varying MaskVertexMain(Input input)
{
    Varying output;
    float4 p = float4(input.PositionMesh, 1);
    output.PositionClip = float4(dot(input.Row0, p), dot(input.Row1, p), dot(input.Row2, p), dot(input.Row3, p));
    return output;
}
[shader("fragment")] float4 MaskFragmentMain(Varying input) : SV_Target
{
    return float4(1, 1, 1, 1);
}
