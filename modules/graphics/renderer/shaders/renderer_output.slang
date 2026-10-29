#include "raster_bindings.slang"
// Explicit scene-linear -> display-linear tone curve (Reinhard or Khronos PBR Neutral) -> optional sRGB encoding.
// Parameters.Output: exposure, curve (0 none, 1 Reinhard, 2 PBR Neutral; ToneCurve in tone_mapping.hpp),
// sRGB encode flag, flags: bit 0 is the backdrop, the bits above it the SpatialAntialiasing mode (lighting.hpp):
// 0 off, 1 Fast, 2 Balanced. flags = backdrop + 2 * mode, a small integer that a float holds exactly.
// With the backdrop bit the scene must have been cleared to alpha zero: every pixel the scene left uncovered shows a
// vertical gradient under the premultiplied scene colour, in display space, so the tokens appear exactly.
// UI blends into a linear display intermediate before the final encode-only pass.
struct Parameters
{
    float4 Output;
};
LUDUS_SHADER_BINDING(0) ConstantBuffer<Parameters> parameters : register(b0);
LUDUS_SHADER_BINDING(1) Texture2D<float4> image : register(t1);
LUDUS_SHADER_BINDING(2) SamplerState imageSampler : register(s2);
// Backdrop endpoints in sRGB, the surface.viewportTop and surface.viewport token colours (config/design-tokens.json;
// scripts/python/test_design_tokens.py keeps them equal).
static const float3 BACKDROP_TOP_SRGB = float3(15.0, 21.0, 27.0) / 255.0;
static const float3 BACKDROP_BOTTOM_SRGB = float3(9.0, 12.0, 15.0) / 255.0;
static const float SRGB_LINEAR_THRESHOLD = 0.04045;
static const float SRGB_LINEAR_SLOPE = 12.92;
static const float SRGB_OFFSET = 0.055;
float3 SrgbToLinear(float3 color)
{
    return float3(color.x <= SRGB_LINEAR_THRESHOLD ? color.x / SRGB_LINEAR_SLOPE
                                                   : pow((color.x + SRGB_OFFSET) / (1 + SRGB_OFFSET), 2.4),
                  color.y <= SRGB_LINEAR_THRESHOLD ? color.y / SRGB_LINEAR_SLOPE
                                                   : pow((color.y + SRGB_OFFSET) / (1 + SRGB_OFFSET), 2.4),
                  color.z <= SRGB_LINEAR_THRESHOLD ? color.z / SRGB_LINEAR_SLOPE
                                                   : pow((color.z + SRGB_OFFSET) / (1 + SRGB_OFFSET), 2.4));
}
struct Input
{
    LUDUS_SHADER_LOCATION(0) float4 PositionUvClip : POSITION;
};
struct Varying
{
    float4 PositionClip : SV_Position;
    float2 TexcoordUv : TEXCOORD0;
};
// Khronos PBR Neutral (https://github.com/KhronosGroup/ToneMapping); mirrors ApplyPbrNeutral in tone_mapping.hpp.
static const float PBR_NEUTRAL_FRESNEL = 0.04;
static const float PBR_NEUTRAL_START = 0.8 - PBR_NEUTRAL_FRESNEL;
static const float PBR_NEUTRAL_DESATURATION = 0.15;
float3 ApplyPbrNeutral(float3 color)
{
    // Infinite radiance would produce infinity * 0; it maps to white like the Reinhard path's guard.
    if (all(isfinite(color)) == false)
    {
        return float3(1);
    }
    float smallest = min(color.x, min(color.y, color.z));
    float offset = smallest <= 2 * PBR_NEUTRAL_FRESNEL ? smallest - smallest * smallest / (4 * PBR_NEUTRAL_FRESNEL)
                                                       : PBR_NEUTRAL_FRESNEL;
    color -= offset;
    float peak = max(color.x, max(color.y, color.z));
    if (peak <= PBR_NEUTRAL_START)
    {
        return color;
    }
    float headroom = 1 - PBR_NEUTRAL_START;
    float newPeak = 1 - headroom * headroom / (peak + 1 - 2 * PBR_NEUTRAL_START);
    float mixAmount = 1 / (PBR_NEUTRAL_DESATURATION * (peak - newPeak) + 1);
    return color * (newPeak / peak) * mixAmount + newPeak * (1 - mixAmount);
}
static const float3 LUMA_WEIGHTS = float3(0.299, 0.587, 0.114);
static const int ANTIALIASING_FAST = 1;
static const int ANTIALIASING_BALANCED = 2;
static const float FLAG_ROUNDING = 0.5;
// Edge search distances along the edge, in pixels, nearest first. Fast walks the first FAST_SEARCH_STEPS of them.
static const int SEARCH_STEP_COUNT = 8;
static const int FAST_SEARCH_STEPS = 4;
static const int SEARCH_OFFSETS[SEARCH_STEP_COUNT] = {1, 2, 3, 4, 6, 8, 12, 16};
// FXAA 3.11 tuning (Timothy Lottes, NVIDIA): a pixel is an edge when its local contrast exceeds
// max(MINIMUM, THRESHOLD * brightest neighbour); SUBPIXEL scales how far isolated bright pixels blend away.
static const float FAST_EDGE_THRESHOLD = 1.0 / 6.0;
static const float FAST_EDGE_MINIMUM = 1.0 / 12.0;
static const float FAST_SUBPIXEL = 0.5;
static const float BALANCED_EDGE_THRESHOLD = 1.0 / 8.0;
static const float BALANCED_EDGE_MINIMUM = 1.0 / 32.0;
static const float BALANCED_SUBPIXEL = 0.75;
static const float SUBPIXEL_AVERAGE_WEIGHT = 1.0 / 12.0;
static const float GRADIENT_SCALE = 0.25;

// Exposure and tone curve of a scene colour; saturated when the output encodes to sRGB.
float3 ToneMapColor(float3 sceneColor)
{
    float3 color = max(sceneColor, float3(0)) * parameters.Output.x;
    if (parameters.Output.y > 1.5)
    {
        color = ApplyPbrNeutral(color);
    }
    else if (parameters.Output.y > 0.5)
    {
        color = color / (1 + color);
    }
    return parameters.Output.z > 0.5 ? saturate(color) : color;
}
float3 EncodeColor(float3 color)
{
    if (parameters.Output.z > 0.5)
    {
        color = float3(color.x <= 0.0031308 ? 12.92 * color.x : 1.055 * pow(color.x, 1.0 / 2.4) - 0.055,
                       color.y <= 0.0031308 ? 12.92 * color.y : 1.055 * pow(color.y, 1.0 / 2.4) - 0.055,
                       color.z <= 0.0031308 ? 12.92 * color.z : 1.055 * pow(color.z, 1.0 / 2.4) - 0.055);
    }
    return color;
}
float3 GetBackdrop(float uvY)
{
    float3 backdrop = lerp(BACKDROP_TOP_SRGB, BACKDROP_BOTTOM_SRGB, uvY);
    return parameters.Output.z > 0.5 ? backdrop : SrgbToLinear(backdrop);
}
// Display colour of one scene texel under the output parameters, with the backdrop composited when it is on.
float3 DisplayColor(float4 scene, float uvY, bool hasBackdrop)
{
    float3 color = EncodeColor(ToneMapColor(scene.rgb));
    if (hasBackdrop)
    {
        color += GetBackdrop(uvY) * (1 - saturate(scene.a));
    }
    return color;
}
// The tone curve applied to a grey value: ApplyPbrNeutral and the Reinhard branch of ToneMapColor for r = g = b, in
// one scalar. Edge detection only needs the display brightness, so it skips the three-channel curve.
float ToneMapGrey(float grey)
{
    float value = grey * parameters.Output.x;
    if (parameters.Output.y > 1.5)
    {
        if (isfinite(value) == false)
        {
            return 1;
        }
        float offset =
            value <= 2 * PBR_NEUTRAL_FRESNEL ? value - value * value / (4 * PBR_NEUTRAL_FRESNEL) : PBR_NEUTRAL_FRESNEL;
        value -= offset;
        if (value > PBR_NEUTRAL_START)
        {
            float headroom = 1 - PBR_NEUTRAL_START;
            // A grey has no chroma to desaturate toward the new peak, so the curve's result is the new peak itself.
            value = 1 - headroom * headroom / (value + 1 - 2 * PBR_NEUTRAL_START);
        }
    }
    else if (parameters.Output.y > 0.5)
    {
        value = value / (1 + value);
    }
    return parameters.Output.z > 0.5 ? saturate(value) : value;
}
// Perceptual brightness for edge detection: the tone-mapped scene brightness on a square-root curve in place of the
// sRGB encode, so the five-load rejection test of a flat pixel costs no powers. The backdrop is already encoded.
float ApproximateLuma(float4 scene, float uvY, bool hasBackdrop)
{
    float luma = sqrt(max(ToneMapGrey(dot(max(scene.rgb, float3(0)), LUMA_WEIGHTS)), 0));
    if (hasBackdrop)
    {
        luma += dot(lerp(BACKDROP_TOP_SRGB, BACKDROP_BOTTOM_SRGB, uvY), LUMA_WEIGHTS) * (1 - saturate(scene.a));
    }
    return luma;
}
int2 ClampPixel(int2 pixel, int2 size)
{
    return clamp(pixel, int2(0), size - 1);
}
float LoadLuma(int2 pixel, int2 size, bool hasBackdrop)
{
    int2 clamped = ClampPixel(pixel, size);
    return ApproximateLuma(image.Load(int3(clamped, 0)), (clamped.y + 0.5) / size.y, hasBackdrop);
}
float3 LoadColor(int2 pixel, int2 size, bool hasBackdrop)
{
    int2 clamped = ClampPixel(pixel, size);
    return DisplayColor(image.Load(int3(clamped, 0)), (clamped.y + 0.5) / size.y, hasBackdrop);
}
// FXAA 3.11 on the display colour: edge detection on the centre and its four neighbours, then the corners for the
// edge's orientation, a walk along the edge to both ends, and a blend toward the neighbour across the edge. The
// taps are exact texel loads, so the pair average that FXAA reads with a half-pixel bilinear tap is two loads.
float3 AntialiasedColor(int2 pixel, int2 size, bool hasBackdrop, int mode)
{
    bool isBalanced = mode == ANTIALIASING_BALANCED;
    float edgeThreshold = isBalanced ? BALANCED_EDGE_THRESHOLD : FAST_EDGE_THRESHOLD;
    float edgeMinimum = isBalanced ? BALANCED_EDGE_MINIMUM : FAST_EDGE_MINIMUM;
    float subpixel = isBalanced ? BALANCED_SUBPIXEL : FAST_SUBPIXEL;
    int stepCount = isBalanced ? SEARCH_STEP_COUNT : FAST_SEARCH_STEPS;
    float4 sceneCentre = image.Load(int3(pixel, 0));
    float uvCentreY = (pixel.y + 0.5) / size.y;
    float lumaCentre = ApproximateLuma(sceneCentre, uvCentreY, hasBackdrop);
    float lumaNorth = LoadLuma(pixel + int2(0, -1), size, hasBackdrop);
    float lumaSouth = LoadLuma(pixel + int2(0, 1), size, hasBackdrop);
    float lumaWest = LoadLuma(pixel + int2(-1, 0), size, hasBackdrop);
    float lumaEast = LoadLuma(pixel + int2(1, 0), size, hasBackdrop);
    float lumaMinimum = min(lumaCentre, min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    float lumaMaximum = max(lumaCentre, max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    float lumaRange = lumaMaximum - lumaMinimum;
    if (lumaRange < max(edgeMinimum, lumaMaximum * edgeThreshold))
    {
        return DisplayColor(sceneCentre, uvCentreY, hasBackdrop);
    }
    float lumaNorthWest = LoadLuma(pixel + int2(-1, -1), size, hasBackdrop);
    float lumaNorthEast = LoadLuma(pixel + int2(1, -1), size, hasBackdrop);
    float lumaSouthWest = LoadLuma(pixel + int2(-1, 1), size, hasBackdrop);
    float lumaSouthEast = LoadLuma(pixel + int2(1, 1), size, hasBackdrop);
    float lumaNorthSouth = lumaNorth + lumaSouth;
    float lumaWestEast = lumaWest + lumaEast;
    float lumaWestCorners = lumaNorthWest + lumaSouthWest;
    float lumaEastCorners = lumaNorthEast + lumaSouthEast;
    float lumaNorthCorners = lumaNorthWest + lumaNorthEast;
    float lumaSouthCorners = lumaSouthWest + lumaSouthEast;
    float edgeHorizontal = abs(lumaWestCorners - 2 * lumaWest) + abs(lumaNorthSouth - 2 * lumaCentre) * 2 +
                           abs(lumaEastCorners - 2 * lumaEast);
    float edgeVertical = abs(lumaNorthCorners - 2 * lumaNorth) + abs(lumaWestEast - 2 * lumaCentre) * 2 +
                         abs(lumaSouthCorners - 2 * lumaSouth);
    bool isHorizontal = edgeHorizontal >= edgeVertical;
    // The two neighbours across the edge, and the direction that walks along it.
    float lumaBefore = isHorizontal ? lumaNorth : lumaWest;
    float lumaAfter = isHorizontal ? lumaSouth : lumaEast;
    int2 alongEdge = isHorizontal ? int2(1, 0) : int2(0, 1);
    float gradientBefore = lumaBefore - lumaCentre;
    float gradientAfter = lumaAfter - lumaCentre;
    bool isBeforeSteeper = abs(gradientBefore) >= abs(gradientAfter);
    float gradientScaled = max(abs(gradientBefore), abs(gradientAfter)) * GRADIENT_SCALE;
    float lumaAcross = isBeforeSteeper ? lumaBefore : lumaAfter;
    int acrossSign = isBeforeSteeper ? -1 : 1;
    int2 acrossEdge = (isHorizontal ? int2(0, acrossSign) : int2(acrossSign, 0));
    float lumaPairAverage = 0.5 * (lumaAcross + lumaCentre);
    bool isCentreBelowAverage = lumaCentre - lumaPairAverage < 0;
    // Walk both ways along the edge until the pair's average departs from the first pair's by a quarter gradient.
    float distanceBefore = float(SEARCH_OFFSETS[stepCount - 1]);
    float distanceAfter = distanceBefore;
    float lumaEndBefore = 0;
    float lumaEndAfter = 0;
    bool isBeforeDone = false;
    bool isAfterDone = false;
    for (int searchIndex = 0; searchIndex < stepCount; ++searchIndex)
    {
        int2 offset = alongEdge * SEARCH_OFFSETS[searchIndex];
        if (isBeforeDone == false)
        {
            int2 pixelBefore = pixel - offset;
            lumaEndBefore = 0.5 * (LoadLuma(pixelBefore, size, hasBackdrop) +
                                   LoadLuma(pixelBefore + acrossEdge, size, hasBackdrop)) -
                            lumaPairAverage;
            isBeforeDone = abs(lumaEndBefore) >= gradientScaled;
            distanceBefore = isBeforeDone ? float(SEARCH_OFFSETS[searchIndex]) : distanceBefore;
        }
        if (isAfterDone == false)
        {
            int2 pixelAfter = pixel + offset;
            lumaEndAfter =
                0.5 * (LoadLuma(pixelAfter, size, hasBackdrop) + LoadLuma(pixelAfter + acrossEdge, size, hasBackdrop)) -
                lumaPairAverage;
            isAfterDone = abs(lumaEndAfter) >= gradientScaled;
            distanceAfter = isAfterDone ? float(SEARCH_OFFSETS[searchIndex]) : distanceAfter;
        }
        if (isBeforeDone && isAfterDone)
        {
            break;
        }
    }
    bool isBeforeNearer = distanceBefore < distanceAfter;
    float lumaEnd = isBeforeNearer ? lumaEndBefore : lumaEndAfter;
    float nearerDistance = isBeforeNearer ? distanceBefore : distanceAfter;
    float spanLength = distanceBefore + distanceAfter;
    // A span whose nearer end does not step to the other side of the pair average is not a real edge end.
    bool isEndValid = (isCentreBelowAverage == (lumaEnd < 0)) == false;
    float edgeBlend = isEndValid ? 0.5 - nearerDistance / spanLength : 0;
    // Isolated bright or dark pixels have no edge to walk and blend by how far they depart from their ring.
    float lumaRing = (lumaNorthSouth + lumaWestEast) * 2 + lumaWestCorners + lumaEastCorners;
    float subpixelContrast = saturate(abs(lumaRing * SUBPIXEL_AVERAGE_WEIGHT - lumaCentre) / lumaRange);
    float subpixelBlend = (3 - 2 * subpixelContrast) * subpixelContrast * subpixelContrast;
    subpixelBlend = subpixelBlend * subpixelBlend * subpixel;
    float blend = saturate(max(edgeBlend, subpixelBlend));
    return lerp(DisplayColor(sceneCentre, uvCentreY, hasBackdrop),
                LoadColor(pixel + acrossEdge, size, hasBackdrop),
                blend);
}
[shader("vertex")] Varying VertexMain(Input input) {
    Varying output;
    output.PositionClip = float4(input.PositionUvClip.xy, 0, 1);
    output.TexcoordUv = input.PositionUvClip.zw;
    return output;
}[shader("fragment")] float4 FragmentMain(Varying input)
    : SV_Target
{
    int flags = int(parameters.Output.w + FLAG_ROUNDING);
    bool hasBackdrop = (flags & 1) != 0;
    int antialiasing = flags >> 1;
    if (antialiasing == ANTIALIASING_FAST || antialiasing == ANTIALIASING_BALANCED)
    {
        uint width;
        uint height;
        image.GetDimensions(width, height);
        int2 size = int2(int(width), int(height));
        int2 pixel = clamp(int2(input.TexcoordUv * float2(size)), int2(0), size - 1);
        return float4(AntialiasedColor(pixel, size, hasBackdrop, antialiasing), 1);
    }
    float4 scene = image.SampleLevel(imageSampler, input.TexcoordUv, 0);
    return float4(DisplayColor(scene, input.TexcoordUv.y, hasBackdrop), 1);
}
