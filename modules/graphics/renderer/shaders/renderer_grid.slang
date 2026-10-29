#include "raster_bindings.slang"
// Analytic world grid: one world-space quad on the grid plane. The fragment stage computes line
// coverage from the distance to each line in pixels, so lines stay one pixel wide at any zoom, are
// antialiased without a resolve, and fade out where they would be closer than a few pixels.
//
// ABI: renderer_flat's vertex stream and instance stream, with the instance fields reused.
// No uniforms or textures; the CPU bakes the view into the instance.
//   PositionWorld       quad corner on the plane, in world units
//   Row0..Row3          world-to-clip rows
//   MinorColor          minor line rgb, w = line width in pixels
//   MajorSpacing        major line rgb, w = minor spacing in world units
//   AxisU               xyz = unit direction of plane coordinate u, w = radial fade start (world units)
//   AxisV               xyz = unit direction of plane coordinate v, w = radial fade end (world units)
//   Center              xyz = plane centre, the origin of the radial fade, w = axis line width in pixels
//   AxisColorU          rgb = colour of the axis line along u
//   AxisColorV          rgb = colour of the axis line along v
struct Input
{
    LUDUS_SHADER_LOCATION(0) float3 PositionWorld : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 Row0 : TEXCOORD0;
    LUDUS_SHADER_LOCATION(2) float4 Row1 : TEXCOORD1;
    LUDUS_SHADER_LOCATION(3) float4 Row2 : TEXCOORD2;
    LUDUS_SHADER_LOCATION(4) float4 Row3 : TEXCOORD3;
    LUDUS_SHADER_LOCATION(5) float4 MinorColor : COLOR0;
    LUDUS_SHADER_LOCATION(6) float4 MajorSpacing : TEXCOORD4;
    LUDUS_SHADER_LOCATION(7) float4 AxisU : TEXCOORD5;
    LUDUS_SHADER_LOCATION(8) float4 AxisV : TEXCOORD6;
    LUDUS_SHADER_LOCATION(9) float4 Center : TEXCOORD7;
    LUDUS_SHADER_LOCATION(10) float4 AxisColorU : TEXCOORD8;
    LUDUS_SHADER_LOCATION(11) float4 AxisColorV : TEXCOORD9;
};
struct Varying
{
    float4 PositionClip : SV_Position;
    float3 PositionWorld : TEXCOORD0;
    nointerpolation float4 MinorColor : TEXCOORD1;
    nointerpolation float4 MajorSpacing : TEXCOORD2;
    nointerpolation float4 AxisU : TEXCOORD3;
    nointerpolation float4 AxisV : TEXCOORD4;
    nointerpolation float4 Center : TEXCOORD5;
    nointerpolation float3 AxisColorU : TEXCOORD6;
    nointerpolation float3 AxisColorV : TEXCOORD7;
};
static const float DECADE = 10.0;
// Lines closer than this many pixels are invisible; they reach full strength at FADE_HIGH_PIXELS.
static const float FADE_LOW_PIXELS = 2.0;
static const float FADE_HIGH_PIXELS = 10.0;
static const float MINIMUM_PIXEL_SIZE = 1e-12;
static const float HALF = 0.5;
[shader("vertex")] Varying GridVertexMain(Input input) {
    Varying output;
    float4 p = float4(input.PositionWorld, 1);
    output.PositionClip = float4(dot(input.Row0, p), dot(input.Row1, p), dot(input.Row2, p), dot(input.Row3, p));
    output.PositionWorld = input.PositionWorld;
    output.MinorColor = input.MinorColor;
    output.MajorSpacing = input.MajorSpacing;
    output.AxisU = input.AxisU;
    output.AxisV = input.AxisV;
    output.Center = input.Center;
    output.AxisColorU = input.AxisColorU.xyz;
    output.AxisColorV = input.AxisColorV.xyz;
    return output;
}
// Coverage of the lines at multiples of spacing, per plane coordinate, then merged. uv is in world
// units and pixelSize is the world size of one pixel along each coordinate.
float LineLevel(float2 uv, float2 pixelSize, float spacing, float halfPixels)
{
    float2 distanceWorld = abs(frac(uv / spacing + HALF) - HALF) * spacing;
    float2 distancePixels = distanceWorld / pixelSize;
    float2 coverage = saturate(halfPixels + HALF - distancePixels);
    float2 gapPixels = spacing / pixelSize;
    float2 density = saturate((gapPixels - FADE_LOW_PIXELS) / (FADE_HIGH_PIXELS - FADE_LOW_PIXELS));
    float2 visible = coverage * density;
    return max(visible.x, visible.y);
}
float4 Over(float3 color, float coverage, float4 below)
{
    return float4(color * coverage + below.rgb * (1.0 - coverage), coverage + below.a * (1.0 - coverage));
}
[shader("fragment")] float4 GridFragmentMain(Varying input) : SV_Target
{
    float2 uv = float2(dot(input.PositionWorld, input.AxisU.xyz), dot(input.PositionWorld, input.AxisV.xyz));
    float2 pixelSize = max(fwidth(uv), float2(MINIMUM_PIXEL_SIZE, MINIMUM_PIXEL_SIZE));
    float halfPixels = input.MinorColor.w * HALF;
    float spacing = input.MajorSpacing.w;
    float minorCoverage = LineLevel(uv, pixelSize, spacing, halfPixels);
    float majorCoverage = LineLevel(uv, pixelSize, spacing * DECADE, halfPixels);
    // Axis lines are exact: they have no neighbours to fade into.
    float axisHalfPixels = input.Center.w * HALF;
    float2 axisCoverage = saturate(axisHalfPixels + HALF - abs(uv) / pixelSize);
    // uv.y is the distance to the line along u, so the line along u uses axisCoverage.y.
    float alongU = axisCoverage.y;
    float alongV = axisCoverage.x;
    // Premultiplied "over" compositing, minor lines first and axes last.
    float4 result = float4(input.MinorColor.rgb * minorCoverage, minorCoverage);
    result = Over(input.MajorSpacing.rgb, majorCoverage, result);
    result = Over(input.AxisColorU, alongU, result);
    result = Over(input.AxisColorV, alongV, result);
    float fade = 1.0 - smoothstep(input.AxisU.w, input.AxisV.w, length(input.PositionWorld - input.Center.xyz));
    return result * fade;
}
