#include "raster_bindings.slang"
// Selection outline, mask pass. The selected mesh is rasterized into a single-colour mask that is white where the
// mesh covers a pixel and black elsewhere. No depth test is used, so another object in front does not hide the
// outline. The edge pass in renderer_selection_outline.slang turns the mask into the outline.
//
// ABI: renderer_flat's vertex stream and instance stream, without the colour. No uniforms or textures.
//   PositionMesh    mesh-space position
//   Row0..Row3      mesh-to-clip rows (Row2, the depth row, is not read)
static const float DEPTH_FRACTION = 0.5;
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
    float4 positionHomogeneous = float4(input.PositionMesh, 1);
    float clipX = dot(input.Row0, positionHomogeneous);
    float clipY = dot(input.Row1, positionHomogeneous);
    float clipW = dot(input.Row3, positionHomogeneous);
    // Depth is pinned inside the clip volume: only the camera plane and the frame sides clip the mesh, so the mask
    // matches the silhouette whatever the depth convention or far distance of the view.
    output.PositionClip = float4(clipX, clipY, clipW * DEPTH_FRACTION, clipW);
    return output;
}
[shader("fragment")] float4 MaskFragmentMain(Varying input) : SV_Target
{
    return float4(1, 1, 1, 1);
}
