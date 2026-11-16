#pragma once
#include "deferred_layout.h"
#include "display_tone_mapping.slang"
#include "raster_bindings.slang"
#include "renderer_deferred_fullscreen.slang"
#include "skin_diffusion_layout.h"

// Screen-space diffusion of skin's diffuse lighting in the clustered deferred profile. Thanks to Jorge Jimenez et
// al., "Separable Subsurface Scattering", Computer Graphics Forum 34(6), 2015, https://iryoku.com/separable-sss/:
// the pre-integrated kernel of section 4 applied as a horizontal then a vertical convolution of the diffuse layer
// only, with the specular layer composited afterwards (section 6), and seven taps shared by the three channels and
// placed by the dominant channel (section 6, "Importance sampling"). The kernel's footprint is its world-space
// extent at the pixel's depth. A tap counts only on skin and on the centre's surface (LUDUS_SKIN_SURFACE_SLOPE_LIMIT),
// and the weights of the taps that count are renormalized, so silhouettes and other materials neither bleed in nor
// darken the edge. The kernel tables and their CPU mirror are in src/internal/skin_diffusion.h.

struct SkinViewParameters
{
    float4 ClipToWorldRows[DEFERRED_VIEW_ROWS];
    float4 EyeWorld;
    float4 TargetSizePixel;
    // Z: graph ToneMap exposure; zero writes clamped linear colour. W: its ToneCurve code.
    float4 Options;
};
struct SkinKernelParameters
{
    float4 Words[LUDUS_SKIN_KERNEL_WORDS];
};

LUDUS_SHADER_BINDING(LUDUS_SKIN_SOURCE_BINDING)
Texture2D<float4> skinSourceTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_SKIN_SOURCE_BINDING);
#ifdef LUDUS_SKIN_COMPOSITE
LUDUS_SHADER_BINDING(LUDUS_SKIN_RADIANCE_BINDING)
Texture2D<float4> skinRadianceTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_SKIN_RADIANCE_BINDING);
#endif
LUDUS_SHADER_BINDING(LUDUS_SKIN_NORMAL_BINDING)
Texture2D<float4> skinNormalTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_SKIN_NORMAL_BINDING);
LUDUS_SHADER_BINDING(LUDUS_SKIN_COVERAGE_BINDING)
Texture2D<float4> skinCoverageTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_SKIN_COVERAGE_BINDING);
LUDUS_SHADER_BINDING(LUDUS_SKIN_DEPTH_BINDING)
Texture2D<float> skinDepthTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_SKIN_DEPTH_BINDING);
LUDUS_SHADER_BINDING(LUDUS_SKIN_VIEW_BINDING)
ConstantBuffer<SkinViewParameters> skinView LUDUS_SHADER_UNIFORM_REGISTER(LUDUS_SKIN_VIEW_BINDING);
LUDUS_SHADER_BINDING(LUDUS_SKIN_KERNEL_BINDING)
ConstantBuffer<SkinKernelParameters> skinKernel LUDUS_SHADER_UNIFORM_REGISTER(LUDUS_SKIN_KERNEL_BINDING);

static const uint SKIN_SIDE_TAPS = LUDUS_SKIN_KERNEL_SIDE_TAPS;
static const uint SKIN_ENERGY_ENTRIES = LUDUS_SKIN_ENERGY_ENTRIES;
static const float SKIN_ENERGY_LOG2_MIN = LUDUS_SKIN_ENERGY_LOG2_MIN;
static const float SKIN_ENERGY_LOG2_MAX = LUDUS_SKIN_ENERGY_LOG2_MAX;
static const float SKIN_SURFACE_SLOPE_LIMIT = LUDUS_SKIN_SURFACE_SLOPE_LIMIT;
static const uint FLOAT4_COMPONENTS = 4;

float GetSkinWordComponent(float4 word, uint component)
{
    return component == 0 ? word.x : (component == 1 ? word.y : (component == 2 ? word.z : word.w));
}
float GetSkinEnergyEntry(uint entry)
{
    return GetSkinWordComponent(skinKernel.Words[entry / FLOAT4_COMPONENTS], entry % FLOAT4_COMPONENTS);
}
// LookupSkinStripEnergy in skin_diffusion.cpp: the share of a channel's energy within u scattering distances.
float LookupSkinStripEnergy(float halfWidthPerDistance)
{
    if ((halfWidthPerDistance > 0) == false)
    {
        return 0;
    }
    float last = float(SKIN_ENERGY_ENTRIES - 1);
    float position =
        (log2(halfWidthPerDistance) - SKIN_ENERGY_LOG2_MIN) / (SKIN_ENERGY_LOG2_MAX - SKIN_ENERGY_LOG2_MIN) * last;
    if (position <= 0)
    {
        return GetSkinEnergyEntry(0) * halfWidthPerDistance / exp2(SKIN_ENERGY_LOG2_MIN);
    }
    if (position >= last)
    {
        return GetSkinEnergyEntry(SKIN_ENERGY_ENTRIES - 1);
    }
    uint index = uint(position);
    float fraction = position - float(index);
    float low = GetSkinEnergyEntry(index);
    float high = GetSkinEnergyEntry(index + 1);
    return low + (high - low) * fraction;
}
// The channel's unnormalized weight of side tap `side` (zero: the centre), for its distance over the dominant one.
float ComputeSkinTapWeight(uint side, float distanceRatio)
{
    if ((distanceRatio > 0) == false)
    {
        return side == 0 ? 1 : 0;
    }
    float4 boundaries = skinKernel.Words[LUDUS_SKIN_KERNEL_BOUNDARY_WORD];
    float outer = LookupSkinStripEnergy(GetSkinWordComponent(boundaries, side) / distanceRatio);
    if (side == 0)
    {
        return outer;
    }
    float inner = LookupSkinStripEnergy(GetSkinWordComponent(boundaries, side - 1) / distanceRatio);
    return (outer - inner) / 2;
}
float3 ReconstructSkinWorld(float2 positionPixel, float depth)
{
    float2 positionClip = positionPixel / skinView.TargetSizePixel.xy * float2(2, -2) + float2(-1, 1);
    float4 positionHomogeneousClip = float4(positionClip, depth, 1);
    float4 positionHomogeneousWorld = float4(dot(skinView.ClipToWorldRows[0], positionHomogeneousClip),
                                             dot(skinView.ClipToWorldRows[1], positionHomogeneousClip),
                                             dot(skinView.ClipToWorldRows[2], positionHomogeneousClip),
                                             dot(skinView.ClipToWorldRows[3], positionHomogeneousClip));
    return positionHomogeneousWorld.xyz / positionHomogeneousWorld.w;
}
bool IsSkinCoverage(float coverage)
{
    return coverage == float(DEFERRED_SKIN_COVERAGE);
}
// One 1D pass of the kernel along axisPixel (one pixel horizontally or vertically) over skinSourceTexture. A pixel
// that is not skin, or has no scattering distance, keeps its own value.
float3 SpreadSkinDiffuse(int2 centrePixel, float2 axisPixel)
{
    float3 centreValue = skinSourceTexture.Load(int3(centrePixel, 0)).rgb;
    float4 coverage = skinCoverageTexture.Load(int3(centrePixel, 0));
    float3 distance = coverage.rgb;
    float dominant = max(max(distance.r, distance.g), distance.b);
    if (IsSkinCoverage(coverage.w) == false || (dominant > 0) == false)
    {
        return centreValue;
    }
    float depth = skinDepthTexture.Load(int3(centrePixel, 0));
    float2 centrePosition = float2(centrePixel) + 0.5;
    float3 centreWorld = ReconstructSkinWorld(centrePosition, depth);
    // The kernel's world extent becomes pixels through the footprint of one pixel at this depth.
    float metresPerPixel = length(ReconstructSkinWorld(centrePosition + axisPixel, depth) - centreWorld);
    float3 normal = skinNormalTexture.Load(int3(centrePixel, 0)).xyz;
    float normalLength = length(normal);
    if ((metresPerPixel > 0) == false || (normalLength > 0) == false)
    {
        return centreValue;
    }
    float3 normalWorld = normal / normalLength;
    float3 ratio = distance / dominant;
    float3 centreWeight =
        float3(ComputeSkinTapWeight(0, ratio.r), ComputeSkinTapWeight(0, ratio.g), ComputeSkinTapWeight(0, ratio.b));
    float3 sum = centreValue * centreWeight;
    float3 total = centreWeight;
    float4 offsets = skinKernel.Words[LUDUS_SKIN_KERNEL_OFFSET_WORD];
    int2 extentPixel = int2(skinView.TargetSizePixel.xy);
    for (uint side = 1; side <= SKIN_SIDE_TAPS; ++side)
    {
        float offsetMetres = GetSkinWordComponent(offsets, side - 1) * dominant;
        float offsetPixels = offsetMetres / metresPerPixel;
        float3 weight = float3(ComputeSkinTapWeight(side, ratio.r),
                               ComputeSkinTapWeight(side, ratio.g),
                               ComputeSkinTapWeight(side, ratio.b));
        for (int direction = -1; direction <= 1; direction += 2)
        {
            int2 tapPixel = int2(floor(centrePosition + axisPixel * (float(direction) * offsetPixels)));
            if (any(tapPixel < int2(0)) || any(tapPixel >= extentPixel))
            {
                continue;
            }
            if (IsSkinCoverage(skinCoverageTexture.Load(int3(tapPixel, 0)).w) == false)
            {
                continue;
            }
            float tapDepth = skinDepthTexture.Load(int3(tapPixel, 0));
            float3 tapWorld = ReconstructSkinWorld(float2(tapPixel) + 0.5, tapDepth);
            if (abs(dot(normalWorld, tapWorld - centreWorld)) > SKIN_SURFACE_SLOPE_LIMIT * offsetMetres)
            {
                continue;
            }
            sum += skinSourceTexture.Load(int3(tapPixel, 0)).rgb * weight;
            total += weight;
        }
    }
    return sum / total;
}

#ifdef LUDUS_SKIN_BLUR
[shader("fragment")] float4 SkinBlurMain(in DeferredVarying value) : SV_Target
{
    return float4(SpreadSkinDiffuse(int2(value.PositionClip.xy), float2(1, 0)), 1);
}
#endif

#ifdef LUDUS_SKIN_COMPOSITE
struct SkinCompositeOutput
{
    float4 Color : SV_Target0;
    float Depth : SV_Depth;
};
[shader("fragment")] SkinCompositeOutput SkinCompositeMain(in DeferredVarying value) {
    int2 positionPixel = int2(value.PositionClip.xy);
    float coverage = skinCoverageTexture.Load(int3(positionPixel, 0)).w;
    if (coverage == 0)
    {
        discard;
    }
    float3 radiance = skinRadianceTexture.Load(int3(positionPixel, 0)).rgb;
    if (IsSkinCoverage(coverage) == true)
    {
        radiance += SpreadSkinDiffuse(positionPixel, float2(0, 1));
    }
    SkinCompositeOutput output;
    output.Color = float4(radiance, 1);
    // The graph's ToneMap, fused here as the ordinary resolve fuses it.
    if (skinView.Options.z > 0)
    {
        output.Color.rgb = MapSceneToDisplay(output.Color.rgb, skinView.Options.z, uint(skinView.Options.w + 0.5));
    }
    output.Depth = skinDepthTexture.Load(int3(positionPixel, 0));
    return output;
}
#endif
