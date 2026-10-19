// Editor wireframe visualization on the renderer's unlit material interface (L3:
// Tint, UvTransform and Options at 0/16/32; colour texture and sampler at 1/2).
// The RHI has no line topology or polygon fill mode, so the editor expands each
// triangle per corner and stores the corner's barycentric coordinate in the
// material UV: (1, 0), (0, 1) and (0, 0). Edges are the pixels near a zero
// barycentric coordinate, drawn over a dim fill so hidden edges stay removed.
//
// Thanks to Bærentzen, Nielsen, Gjøl, Larsen and Christensen, "Single-pass
// wireframe rendering", ACM SIGGRAPH 2006 Sketches
// (https://people.compute.dtu.dk/janba/Wireframe/Siggraph06.pdf), for drawing
// antialiased edges in the fragment shader from per-fragment edge distances.
// This adaptation measures the distance with screen-space derivatives of the
// interpolated barycentrics instead of window-space distances from a geometry pass.
#include "raster_bindings.slang"

struct Parameters
{
    float4 Tint;        // Edge colour.
    float4 UvTransform; // Unused by the visualization; kept for the material layout.
    float4 Options;     // Unused by the visualization; kept for the material layout.
};
LUDUS_SHADER_BINDING(0) ConstantBuffer<Parameters> material;
LUDUS_SHADER_BINDING(1) Texture2D<float4> colorTexture;
LUDUS_SHADER_BINDING(2) SamplerState colorSampler;

// Edge width in pixels before the one-pixel antialiasing ramp.
static const float EDGE_WIDTH_PIXELS = 1.0;
// Fill brightness behind the edges, relative to the (white) default texture.
static const float FILL_SHADE = 0.12;
// Smallest derivative used as a divisor, so degenerate triangles stay finite.
static const float MIN_DERIVATIVE = 1e-6;

struct Input
{
    LUDUS_SHADER_LOCATION(0) float3 PositionMesh : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 MeshToClipRow0 : TEXCOORD0;
    LUDUS_SHADER_LOCATION(2) float4 MeshToClipRow1 : TEXCOORD1;
    LUDUS_SHADER_LOCATION(3) float4 MeshToClipRow2 : TEXCOORD2;
    LUDUS_SHADER_LOCATION(4) float4 MeshToClipRow3 : TEXCOORD3;
    LUDUS_SHADER_LOCATION(5) float4 Color : COLOR0;
    LUDUS_SHADER_LOCATION(6) float2 Barycentric : TEXCOORD4;
};
struct Varying
{
    float4 PositionClip : SV_Position;
    float2 Barycentric : TEXCOORD0;
    float4 Color : COLOR0;
};

[shader("vertex")]
Varying VertexMain(Input input)
{
    Varying output;
    float4 positionMesh = float4(input.PositionMesh, 1);
    output.PositionClip = float4(dot(input.MeshToClipRow0, positionMesh),
                                 dot(input.MeshToClipRow1, positionMesh),
                                 dot(input.MeshToClipRow2, positionMesh),
                                 dot(input.MeshToClipRow3, positionMesh));
    output.Barycentric = input.Barycentric;
    output.Color = input.Color;
    return output;
}

[shader("fragment")]
float4 FragmentMain(Varying input) : SV_Target
{
    float3 barycentric = float3(input.Barycentric, 1 - input.Barycentric.x - input.Barycentric.y);
    float3 distancePixel = barycentric / max(fwidth(barycentric), MIN_DERIVATIVE);
    float nearestPixel = min(distancePixel.x, min(distancePixel.y, distancePixel.z));
    float coverage = saturate(EDGE_WIDTH_PIXELS + 0.5 - nearestPixel);
    float4 fill = colorTexture.Sample(colorSampler, material.UvTransform.zw) * FILL_SHADE;
    float3 color = lerp(fill.rgb, material.Tint.rgb, coverage) * input.Color.rgb;
    return float4(color, 1);
}
