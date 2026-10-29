// Analytic world grid: one world-space quad on the grid plane. The fragment stage computes line
// coverage from the distance to each line in pixels, so lines stay one pixel wide at any zoom, are
// antialiased without a resolve, and fade out where they would be closer than a few pixels.
//
// ABI: renderer_flat's vertex stream and instance stream, with the instance fields reused.
// No uniforms or textures; the CPU bakes the view into the instance.
//   position            quad corner on the plane, in world units
//   row0..row3          world-to-clip rows
//   color               minor line rgb, w = line width in pixels
//   worldRow0           major line rgb, w = minor spacing in world units
//   worldRow1           xyz = unit direction of plane coordinate u, w = radial fade start (world units)
//   worldRow2           xyz = unit direction of plane coordinate v, w = radial fade end (world units)
//   normalRow0          xyz = plane centre, the origin of the radial fade, w = axis line width in pixels
//   normalRow1          rgb = colour of the axis line along u
//   normalRow2          rgb = colour of the axis line along v
struct Input
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 row0 : TEXCOORD0;
    [[vk::location(2)]] float4 row1 : TEXCOORD1;
    [[vk::location(3)]] float4 row2 : TEXCOORD2;
    [[vk::location(4)]] float4 row3 : TEXCOORD3;
    [[vk::location(5)]] float4 minorColor : COLOR0;
    [[vk::location(6)]] float4 majorSpacing : TEXCOORD4;
    [[vk::location(7)]] float4 axisU : TEXCOORD5;
    [[vk::location(8)]] float4 axisV : TEXCOORD6;
    [[vk::location(9)]] float4 center : TEXCOORD7;
    [[vk::location(10)]] float4 axisColorU : TEXCOORD8;
    [[vk::location(11)]] float4 axisColorV : TEXCOORD9;
};
struct Varying
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    nointerpolation float4 minorColor : TEXCOORD1;
    nointerpolation float4 majorSpacing : TEXCOORD2;
    nointerpolation float4 axisU : TEXCOORD3;
    nointerpolation float4 axisV : TEXCOORD4;
    nointerpolation float4 center : TEXCOORD5;
    nointerpolation float3 axisColorU : TEXCOORD6;
    nointerpolation float3 axisColorV : TEXCOORD7;
};
static const float DECADE = 10.0;
// Lines closer than this many pixels are invisible; they reach full strength at FADE_HIGH_PIXELS.
static const float FADE_LOW_PIXELS = 2.0;
static const float FADE_HIGH_PIXELS = 10.0;
static const float MINIMUM_PIXEL_SIZE = 1e-12;
static const float HALF = 0.5;
[shader("vertex")]
Varying vertexMain(Input input)
{
    Varying output;
    float4 p = float4(input.position, 1);
    output.position = float4(dot(input.row0, p), dot(input.row1, p), dot(input.row2, p), dot(input.row3, p));
    output.world = input.position;
    output.minorColor = input.minorColor;
    output.majorSpacing = input.majorSpacing;
    output.axisU = input.axisU;
    output.axisV = input.axisV;
    output.center = input.center;
    output.axisColorU = input.axisColorU.xyz;
    output.axisColorV = input.axisColorV.xyz;
    return output;
}
// Coverage of the lines at multiples of spacing, per plane coordinate, then merged. uv is in world
// units and pixelSize is the world size of one pixel along each coordinate.
float lineLevel(float2 uv, float2 pixelSize, float spacing, float halfPixels)
{
    float2 distanceWorld = abs(frac(uv / spacing + HALF) - HALF) * spacing;
    float2 distancePixels = distanceWorld / pixelSize;
    float2 coverage = saturate(halfPixels + HALF - distancePixels);
    float2 gapPixels = spacing / pixelSize;
    float2 density = saturate((gapPixels - FADE_LOW_PIXELS) / (FADE_HIGH_PIXELS - FADE_LOW_PIXELS));
    float2 visible = coverage * density;
    return max(visible.x, visible.y);
}
float4 over(float3 color, float coverage, float4 below)
{
    return float4(color * coverage + below.rgb * (1.0 - coverage), coverage + below.a * (1.0 - coverage));
}
[shader("fragment")]
float4 fragmentMain(Varying input) : SV_Target
{
    float2 uv = float2(dot(input.world, input.axisU.xyz), dot(input.world, input.axisV.xyz));
    float2 pixelSize = max(fwidth(uv), float2(MINIMUM_PIXEL_SIZE, MINIMUM_PIXEL_SIZE));
    float halfPixels = input.minorColor.w * HALF;
    float spacing = input.majorSpacing.w;
    float minorCoverage = lineLevel(uv, pixelSize, spacing, halfPixels);
    float majorCoverage = lineLevel(uv, pixelSize, spacing * DECADE, halfPixels);
    // Axis lines are exact: they have no neighbours to fade into.
    float axisHalfPixels = input.center.w * HALF;
    float2 axisCoverage = saturate(axisHalfPixels + HALF - abs(uv) / pixelSize);
    // uv.y is the distance to the line along u, so the line along u uses axisCoverage.y.
    float alongU = axisCoverage.y;
    float alongV = axisCoverage.x;
    // Premultiplied "over" compositing, minor lines first and axes last.
    float4 result = float4(input.minorColor.rgb * minorCoverage, minorCoverage);
    result = over(input.majorSpacing.rgb, majorCoverage, result);
    result = over(input.axisColorU, alongU, result);
    result = over(input.axisColorV, alongV, result);
    float fade = 1.0 - smoothstep(input.axisU.w, input.axisV.w, length(input.world - input.center.xyz));
    return result * fade;
}
