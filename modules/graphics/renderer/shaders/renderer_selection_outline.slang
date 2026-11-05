#include "raster_bindings.slang"
// Selection outline, edge pass. One quad over the mesh's projected bounds reads the mask that
// renderer_selection_mask.slang rasterized and writes an outline of the mask's boundary into the display image.
// A pixel at distance d from the boundary (in pixels, measured to the edge between a covered and an uncovered pixel)
// receives the amber stroke up to d = OutlineHalfPixels and a translucent casing stroke up to d = CasingHalfPixels,
// the same two strokes the editor overlay paints, so the outline reads on bright and dark pixels alike. The search
// is bounded by the casing width, so the cost per pixel does not depend on the mesh.
//
// ABI: one vertex stream, four vertices, every field except the position repeated per vertex.
//   PositionUv      xy clip position, zw top-left UV of that position in the mask
//   OutlineColor    straight RGBA of the amber stroke, in display-encoded values
//   CasingColor     straight RGBA of the casing stroke, in display-encoded values
//   Params          xy UV size of one pixel, z outline half width and w casing half width, in pixels
LUDUS_SHADER_BINDING(0) Texture2D<float4> maskImage LUDUS_SHADER_TEXTURE_REGISTER(0);
LUDUS_SHADER_BINDING(1) SamplerState maskSampler : register(s1);
struct EdgeInput
{
    LUDUS_SHADER_LOCATION(0) float4 PositionUv : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 OutlineColor : COLOR0;
    LUDUS_SHADER_LOCATION(2) float4 CasingColor : COLOR1;
    LUDUS_SHADER_LOCATION(3) float4 Params : TEXCOORD0;
};
struct EdgeVarying
{
    float4 PositionClip : SV_Position;
    float2 Uv : TEXCOORD0;
    nointerpolation float4 OutlineColor : TEXCOORD1;
    nointerpolation float4 CasingColor : TEXCOORD2;
    nointerpolation float4 Params : TEXCOORD3;
};
// Mask value at or above which a pixel counts as covered by the mesh.
static const float COVERED_THRESHOLD = 0.5;
// Largest search radius in pixels, bounding the cost of an unusually wide stroke.
static const int MAXIMUM_SEARCH_PIXELS = 8;
// Distance beyond any stroke, meaning that no boundary is near.
static const float NO_BOUNDARY_PIXELS = 1e6;
static const float HALF = 0.5;
[shader("vertex")] EdgeVarying EdgeVertexMain(EdgeInput input) {
    EdgeVarying output;
    output.PositionClip = float4(input.PositionUv.xy, 0, 1);
    output.Uv = input.PositionUv.zw;
    output.OutlineColor = input.OutlineColor;
    output.CasingColor = input.CasingColor;
    output.Params = input.Params;
    return output;
} float IsCovered(float2 uv)
{
    return maskImage.SampleLevel(maskSampler, uv, 0).r >= COVERED_THRESHOLD ? 1.0 : 0.0;
}
[shader("fragment")] float4 EdgeFragmentMain(EdgeVarying input) : SV_Target
{
    float2 pixelUv = input.Params.xy;
    float outlineHalfPixels = input.Params.z;
    float casingHalfPixels = input.Params.w;
    // The casing covers a pixel while the boundary is nearer than its half width plus half a pixel, and a pixel
    // across the boundary is half a pixel further than the boundary itself, so every pixel offset of at most the
    // half width, rounded up, can matter.
    int searchPixels = min(int(ceil(casingHalfPixels)), MAXIMUM_SEARCH_PIXELS);
    float centre = IsCovered(input.Uv);
    float nearestPixels = NO_BOUNDARY_PIXELS;
    for (int y = -searchPixels; y <= searchPixels; ++y)
    {
        for (int x = -searchPixels; x <= searchPixels; ++x)
        {
            float2 offsetPixels = float2(x, y);
            float distancePixels = length(offsetPixels);
            if (distancePixels < nearestPixels && IsCovered(input.Uv + offsetPixels * pixelUv) != centre)
            {
                nearestPixels = distancePixels;
            }
        }
    }
    // The boundary lies half a pixel from the centre of the nearest pixel on the other side of it.
    float boundaryPixels = nearestPixels - HALF;
    float outlineAlpha = input.OutlineColor.a * saturate(outlineHalfPixels + HALF - boundaryPixels);
    float casingAlpha = input.CasingColor.a * saturate(casingHalfPixels + HALF - boundaryPixels);
    float alpha = outlineAlpha + casingAlpha * (1.0 - outlineAlpha);
    if (alpha <= 0.0)
    {
        discard;
    }
    float3 color = input.OutlineColor.rgb * outlineAlpha + input.CasingColor.rgb * casingAlpha * (1.0 - outlineAlpha);
    return float4(color, alpha);
}
