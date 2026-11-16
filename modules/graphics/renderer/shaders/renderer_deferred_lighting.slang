#define LUDUS_DEFERRED_MESH_LIGHTS 1
#include "deferred_layout.h"
#include "display_tone_mapping.slang"
#include "renderer_deferred_fullscreen.slang"
#include "renderer_mesh_lights.slang"

struct DeferredViewParameters
{
    float4 ClipToWorldRows[DEFERRED_VIEW_ROWS];
    float4 EyeWorld;
    float4 TargetSizePixel; // XY: dimensions in pixels. Z: mesh-light record count. W: culling grid word.
    // X: direct-reference flag. Y: clear depth. Z: graph ToneMap exposure; zero writes clamped linear colour.
    // W: its ToneCurve code (tone_mapping.hpp).
    float4 Options;
#ifdef LUDUS_ATMOSPHERE_SURFACE
    float4 AtmosphereRadialUpWorld; // xyz: unit radial up, w: altitude in kilometres.
    float4 AtmosphereSunWorld;      // xyz: toward the Sun, w: one-based light index.
    float4 AtmosphereIlluminance;   // xyz: linear RGB illuminance at the top boundary.
    float4 AtmosphereCameraWorld;   // xyz: eye in metres, w: orthographic flag.
#endif
};
LUDUS_SHADER_BINDING(LUDUS_DEFERRED_NORMAL_BINDING)
Texture2D<float4> normalRoughnessTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_DEFERRED_NORMAL_BINDING);
LUDUS_SHADER_BINDING(LUDUS_DEFERRED_BASE_BINDING)
Texture2D<float4> baseMetallicTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_DEFERRED_BASE_BINDING);
LUDUS_SHADER_BINDING(LUDUS_DEFERRED_EMISSION_BINDING)
Texture2D<float4> emissionCoverageTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_DEFERRED_EMISSION_BINDING);
LUDUS_SHADER_BINDING(LUDUS_DEFERRED_DEPTH_BINDING)
Texture2D<float> geometryDepthTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_DEFERRED_DEPTH_BINDING);
LUDUS_SHADER_BINDING(LUDUS_DEFERRED_VIEW_BINDING)
ConstantBuffer<DeferredViewParameters> deferredView LUDUS_SHADER_UNIFORM_REGISTER(LUDUS_DEFERRED_VIEW_BINDING);

#include "renderer_clustered_authored.slang"

// Relative uncertainty of a float32 depth after rasterization, storage and the reconstruction arithmetic:
// four units in the last place (2^-22).
static const float DEFERRED_DEPTH_RELATIVE_UNCERTAINTY = 2.384185791015625e-07;

struct DeferredLightingOutput
{
    float4 Color : SV_Target0;
    float Depth : SV_Depth;
};
// A covered pixel's linear radiance and depth. With shouldSeparateSkin, a skin pixel's diffuse radiance, after the
// atmosphere's transmittance, goes to Diffuse for the diffusion passes and Radiance keeps the rest; otherwise Diffuse
// is zero and Radiance is the whole. Uncovered pixels are discarded.
struct DeferredRadiance
{
    float3 Radiance;
    float3 Diffuse;
    float Depth;
};
DeferredRadiance ResolveDeferredRadiance(in DeferredVarying value, bool shouldSeparateSkin)
{
    int2 positionPixel = int2(value.PositionClip.xy);
    float4 emissionCoverage = emissionCoverageTexture.Load(int3(positionPixel, 0));
    DeferredRadiance output;
    output.Radiance = float3(0);
    output.Diffuse = float3(0);
    output.Depth = deferredView.Options.y;
    if (emissionCoverage.w == 0)
    {
        discard;
    }
    float depth = geometryDepthTexture.Load(int3(positionPixel, 0));
    float2 positionClip = value.PositionClip.xy / deferredView.TargetSizePixel.xy * float2(2, -2) + float2(-1, 1);
    float4 positionHomogeneousClip = float4(positionClip, depth, 1);
    float4 positionHomogeneousWorld = float4(dot(deferredView.ClipToWorldRows[0], positionHomogeneousClip),
                                             dot(deferredView.ClipToWorldRows[1], positionHomogeneousClip),
                                             dot(deferredView.ClipToWorldRows[2], positionHomogeneousClip),
                                             dot(deferredView.ClipToWorldRows[3], positionHomogeneousClip));
    Varying input = (Varying)0;
    input.PositionClip = value.PositionClip;
    input.PositionWorld = positionHomogeneousWorld.xyz / positionHomogeneousWorld.w;
    // The world position is a rational function of the stored depth, so its derivative says how far one
    // rounding of that depth moves it. Conventional depth near one with a small near plane moves it by
    // centimetres tens of metres away; the directional and mesh-light shadow comparisons allow for that distance.
    float4 depthColumn = float4(deferredView.ClipToWorldRows[0].z,
                                deferredView.ClipToWorldRows[1].z,
                                deferredView.ClipToWorldRows[2].z,
                                deferredView.ClipToWorldRows[3].z);
    float3 positionPerDepthWorld =
        (depthColumn.xyz * positionHomogeneousWorld.w - positionHomogeneousWorld.xyz * depthColumn.w) /
        (positionHomogeneousWorld.w * positionHomogeneousWorld.w);
    input.PositionUncertaintyWorld = positionPerDepthWorld * (depth * DEFERRED_DEPTH_RELATIVE_UNCERTAINTY);
    input.EyeWorld = deferredView.EyeWorld;
    input.IsDirectReference = deferredView.Options.x;
    float4 normalRoughness = normalRoughnessTexture.Load(int3(positionPixel, 0));
    float4 baseMetallic = baseMetallicTexture.Load(int3(positionPixel, 0));
    // A skin pixel's emission channels hold its scattering distance; skin itself never emits (IsEmissionAllowed).
    bool isSkin = emissionCoverage.w == float(DEFERRED_SKIN_COVERAGE);
    SurfaceSample surface;
    surface.BaseColor = float4(baseMetallic.rgb, 1);
    surface.NormalWorld = NormalizeOrZero(normalRoughness.xyz);
    surface.ViewDirectionWorld = (GetEyeFlags(input) & 2) != 0
                                     ? NormalizeOrZero(input.EyeWorld.xyz)
                                     : NormalizeOrZero(input.EyeWorld.xyz - input.PositionWorld);
    surface.Metallic = baseMetallic.w;
    surface.Roughness = normalRoughness.w;
    surface.Emission = isSkin == true ? float3(0) : emissionCoverage.rgb;
    surface.Alpha = 1;
    // The G-buffer keeps no strand: a hair card resolves with its facing normal and the ordinary lobes.
    surface.StrandWorld = float3(0);
    input.NormalWorld = surface.NormalWorld;
    bool shouldSeparate = shouldSeparateSkin == true && isSkin == true;
    float3 diffuse;
    float3 meshLightDiffuse;
    output.Radiance = ShadeMaterialLobes(surface, input, shouldSeparate, diffuse).rgb;
    output.Radiance += EvaluateRasterMeshLightLobes(surface,
                                                    input.PositionWorld,
                                                    input.PositionUncertaintyWorld,
                                                    uint(deferredView.TargetSizePixel.z),
                                                    uint(deferredView.TargetSizePixel.w),
                                                    shouldSeparate,
                                                    meshLightDiffuse);
    output.Diffuse = diffuse + meshLightDiffuse;
#ifdef LUDUS_ATMOSPHERE_SURFACE
    if (shouldSeparate == true)
    {
        // The segment is affine in the surface radiance: the spread diffuse light is attenuated like the rest, and
        // the inscattered light is added once, to the unspread part.
        float3 transmittance;
        float3 inscattered;
        IntegrateAtmosphereViewSegment(input.PositionWorld, transmittance, inscattered);
        output.Radiance = output.Radiance * transmittance + inscattered;
        output.Diffuse *= transmittance;
    }
    else
    {
        output.Radiance = ComposeAtmosphereSegment(output.Radiance, input.PositionWorld);
    }
#endif
    output.Depth = depth;
    return output;
}
DeferredLightingOutput ResolveDeferredLighting(in DeferredVarying value)
{
    DeferredRadiance radiance = ResolveDeferredRadiance(value, false);
    DeferredLightingOutput output;
    output.Color = float4(radiance.Radiance, 1);
    // An authored ToneMap after this pass is fused here, before the RGBA8 target quantizes the radiance.
    if (deferredView.Options.z > 0)
    {
        output.Color.rgb =
            MapSceneToDisplay(output.Color.rgb, deferredView.Options.z, uint(deferredView.Options.w + 0.5));
    }
    output.Depth = radiance.Depth;
    return output;
}
[shader("fragment")] DeferredLightingOutput DeferredLightingMain(in DeferredVarying value) {
    return ResolveDeferredLighting(value);
}
#ifdef LUDUS_DEFERRED_SKIN
// The resolve of a view with skin (renderer_skin_deferred.slang): linear radiance without the skin diffuse, and the
// skin diffuse that renderer_skin_diffusion.slang spreads and adds back before tone mapping. Both are RGBA16F.
struct DeferredSkinLightingOutput
{
    float4 Radiance : SV_Target0;
    float4 Diffuse : SV_Target1;
};
[shader("fragment")] DeferredSkinLightingOutput DeferredSkinLightingMain(in DeferredVarying value) {
    DeferredRadiance radiance = ResolveDeferredRadiance(value, true);
    DeferredSkinLightingOutput output;
    output.Radiance = float4(radiance.Radiance, 1);
    output.Diffuse = float4(radiance.Diffuse, 1);
    return output;
}
#endif
