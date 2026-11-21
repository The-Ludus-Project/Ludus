#define LUDUS_DEFERRED_MESH_LIGHTS 1
#include "deferred_layout.h"
#include "display_tone_mapping.slang"
#include "renderer_deferred_fullscreen.slang"
#include "renderer_mesh_lights.slang"
#ifdef LUDUS_ATMOSPHERE_SURFACE
#    include "renderer_aurora_layer.slang"
#    include "renderer_cloud_layer.slang"
#endif

struct DeferredViewParameters
{
    float4 ClipToWorldRows[DEFERRED_VIEW_ROWS];
    float4 EyeWorld;
    float4 TargetSizePixel; // XY: dimensions in pixels. Z: mesh-light record count. W: culling grid word.
    // X: direct-reference flag. Y: clear depth. Z: graph ToneMap exposure; zero writes clamped linear colour.
    // W: its ToneCurve code (tone_mapping.hpp).
    float4 Options;
    // X: DeferredLightingView code. Y: one-based isolated light index or zero. Z: work light enabled.
    float4 LightingView;
#ifdef LUDUS_ATMOSPHERE_SURFACE
    float4 AtmosphereRadialUpWorld; // xyz: unit radial up, w: altitude in kilometres.
    float4 AtmosphereSunWorld;      // xyz: toward the Sun, w: one-based light index.
    float4 AtmosphereIlluminance;   // xyz: linear RGB illuminance at the top boundary.
    float4 AtmosphereCameraWorld;   // xyz: eye in metres, w: orthographic flag.
    float4 AtmosphereCloudLayer[CLOUD_LAYER_UNIFORM_ROWS];
    AuroraLayerParameters AtmosphereAuroraLayer;
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

// Display colour of a lighting view that is not radiance.
static const float3 LIGHTING_VIEW_UNCOVERED_COLOUR = float3(1, 0, 1);
static const float3 LIGHTING_VIEW_UNMAPPED_COLOUR = float3(0, 1, 1);
// Least brightness of a lit pixel in the dominant-light view.
static const float LIGHTING_VIEW_DOMINANT_MINIMUM_BRIGHTNESS = 0.35;
static const float WORK_LIGHT_INNER_COSINE = 0.95;
static const float WORK_LIGHT_OUTER_COSINE = 0.75;
static const float WORK_LIGHT_INTENSITY_CANDELA = 16;
static const float WORK_LIGHT_RANGE_METRES = 12;
float4 TransformClipToWorld(in float4 positionHomogeneousClip)
{
    return float4(dot(deferredView.ClipToWorldRows[0], positionHomogeneousClip),
                  dot(deferredView.ClipToWorldRows[1], positionHomogeneousClip),
                  dot(deferredView.ClipToWorldRows[2], positionHomogeneousClip),
                  dot(deferredView.ClipToWorldRows[3], positionHomogeneousClip));
}
float3 EvaluateWorkLight(in SurfaceSample surface, float3 positionWorld, bool isOrthographic)
{
    // Perspective uses the far centre to recover the axis with a long baseline. In orthographic
    // views EyeWorld already holds the opposite axis, and the near centre is the spotlight origin.
    float centerDepth = isOrthographic == true ? 1 - deferredView.Options.y : deferredView.Options.y;
    float4 centerHomogeneousWorld = TransformClipToWorld(float4(0, 0, centerDepth, 1));
    float3 lightPositionWorld = deferredView.EyeWorld.xyz;
    float3 lightForwardWorld = -deferredView.EyeWorld.xyz;
    if (isOrthographic == true)
        lightPositionWorld = centerHomogeneousWorld.xyz / centerHomogeneousWorld.w;
    else
        lightForwardWorld = NormalizeOrZero(centerHomogeneousWorld.xyz - lightPositionWorld * centerHomogeneousWorld.w);
    float3 toLightWorld = lightPositionWorld - positionWorld;
    float3 lightDirectionWorld = NormalizeOrZero(toLightWorld);
    float cone = ComputeSpotConeAttenuation(dot(-lightDirectionWorld, lightForwardWorld),
                                            WORK_LIGHT_INNER_COSINE,
                                            WORK_LIGHT_OUTER_COSINE);
    // Clamp the inverse-square gain to its one-metre value so close surfaces remain readable.
    float distanceSquaredMetres = dot(toLightWorld, toLightWorld);
    float attenuation = min(ComputePunctualAttenuation(distanceSquaredMetres, WORK_LIGHT_RANGE_METRES), 1);
    if (cone * attenuation == 0)
        return float3(0);
    return EvaluateBrdf(surface.BaseColor.rgb,
                        surface.NormalWorld,
                        surface.ViewDirectionWorld,
                        lightDirectionWorld,
                        surface.Metallic,
                        surface.Roughness,
                        false) *
           (WORK_LIGHT_INTENSITY_CANDELA * cone * attenuation);
}
float3 MapLightingRadiance(in float3 radiance)
{
    // An authored ToneMap after this pass is fused here, before the RGBA8 target quantizes the radiance.
    if (deferredView.Options.z > 0)
        return MapSceneToDisplay(radiance, deferredView.Options.z, uint(deferredView.Options.w + 0.5));
    return radiance;
}
// The shader's copy of GetDeferredLightingViewHue: a fully saturated HSV hue per contributor category.
float3 GetLightingViewHue(int category)
{
    float hue = frac(LUDUS_DEFERRED_LIGHTING_VIEW_HUE_OFFSET + float(category) * LUDUS_DEFERRED_LIGHTING_VIEW_HUE_STEP);
    return saturate(abs(frac(hue + float3(1, 2.0 / 3.0, 1.0 / 3.0)) * 6 - 3) - 1);
}
float3 SelectLightingView(int lightingView,
                          int isolatedLight,
                          in float3 finalRadiance,
                          in LightingTerms terms,
                          in float3 areaLights,
                          Varying input)
{
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_DIRECT_LIGHTS))
        return MapLightingRadiance(terms.Direct);
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_LIGHT))
        return MapLightingRadiance(terms.Isolated);
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_SKY_DIFFUSE))
        return MapLightingRadiance(terms.SkyDiffuse);
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_SKY_SPECULAR))
        return MapLightingRadiance(terms.SkySpecular);
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_AREA_LIGHTS))
        return MapLightingRadiance(areaLights);
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_AERIAL_PERSPECTIVE))
    {
#ifdef LUDUS_ATMOSPHERE_SURFACE
        return MapLightingRadiance(ComposeAtmosphereSegment(float3(0), input.PositionWorld));
#else
        return float3(0);
#endif
    }
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_SHADOW_COVERAGE))
    {
        if (isolatedLight < 0)
            return LIGHTING_VIEW_UNMAPPED_COLOUR;
        int coverage;
        float visible = ComputeShadowCoverage(isolatedLight, input, coverage);
        if (coverage == SHADOW_COVERAGE_NONE)
            return LIGHTING_VIEW_UNMAPPED_COLOUR;
        if (coverage == SHADOW_COVERAGE_OUTSIDE)
            return LIGHTING_VIEW_UNCOVERED_COLOUR;
        return float3(visible);
    }
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_SKY_VISIBILITY))
        return terms.SkyVisibility < 0 ? LIGHTING_VIEW_UNCOVERED_COLOUR : float3(terms.SkyVisibility);
    if (lightingView == int(DEFERRED_LIGHTING_VIEW_DOMINANT_LIGHT))
    {
        int category = int(DEFERRED_LIGHTING_VIEW_SKY_CATEGORY);
        float largest = ComputeLightingLuminance(terms.SkyDiffuse + terms.SkySpecular);
        if (ComputeLightingLuminance(areaLights) > largest)
        {
            category = int(DEFERRED_LIGHTING_VIEW_AREA_CATEGORY);
            largest = ComputeLightingLuminance(areaLights);
        }
        if (terms.DominantLight >= 0 && terms.DominantLuminance > largest)
            category = int(DEFERRED_LIGHTING_VIEW_FIRST_LIGHT_CATEGORY) + terms.DominantLight;
        if (largest <= 0 && (terms.DominantLight < 0 || terms.DominantLuminance <= 0))
            return float3(0);
        // Dim pixels keep a readable hue: the brightness of the final colour spans the upper part of the range.
        float brightness = saturate(ComputeLightingLuminance(MapLightingRadiance(finalRadiance)));
        return GetLightingViewHue(category) * lerp(LIGHTING_VIEW_DOMINANT_MINIMUM_BRIGHTNESS, 1, brightness);
    }
    return MapLightingRadiance(finalRadiance);
}

struct DeferredLightingOutput
{
    float4 Color : SV_Target0;
    float Depth : SV_Depth;
};
// A covered pixel's radiance and depth. Radiance is the selected lighting view, tone mapped by the graph's ToneMap.
// With shouldSeparateSkin and the final view, a skin pixel's diffuse radiance, after the atmosphere's transmittance,
// goes to Diffuse for the diffusion passes, and Radiance stays linear without it, as it does for every other pixel.
// Uncovered pixels are discarded.
// Pre- and post-scatter texturing for skin, after Eugene d'Eon and David Luebke, "Advanced Techniques for Realistic
// Real-Time Skin Rendering", GPU Gems 3, chapter 14, section 14.5.2, "Combining Pre-Scatter and Post-Scatter
// Texturing": the diffusion spreads the diffuse light divided by the square root of the albedo, and the composite
// multiplies by the square root of the centre's albedo, so half the albedo's detail stays sharp. Thanks to the
// authors. A channel with zero albedo carries no diffuse light to spread.
float3 RemoveSkinPreScatterAlbedo(float3 diffuse, float3 baseColor)
{
    float3 root = sqrt(max(baseColor, float3(0)));
    return float3(root.r > 0 ? diffuse.r / root.r : 0,
                  root.g > 0 ? diffuse.g / root.g : 0,
                  root.b > 0 ? diffuse.b / root.b : 0);
}
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
    float4 positionHomogeneousWorld = TransformClipToWorld(positionHomogeneousClip);
    Varying input = (Varying)0;
    input.PositionClip = value.PositionClip;
    input.ForwardDepthMetres = 1 / positionHomogeneousWorld.w;
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
    // A charted pixel's emission holds the baked lights' diffuse, so ShadeMaterial omits their live diffuse there.
    input.IsBakeValid = emissionCoverage.w == float(DEFERRED_SURFACE_CHART_COVERED) ? 1 : 0;
    float4 normalRoughness = normalRoughnessTexture.Load(int3(positionPixel, 0));
    float4 baseMetallic = baseMetallicTexture.Load(int3(positionPixel, 0));
    SurfaceSample surface;
    surface.BaseColor = float4(baseMetallic.rgb, 1);
    surface.NormalWorld = NormalizeOrZero(normalRoughness.xyz);
    bool isOrthographic = (GetEyeFlags(input) & 2) != 0;
    surface.ViewDirectionWorld = isOrthographic == true ? NormalizeOrZero(input.EyeWorld.xyz)
                                                        : NormalizeOrZero(input.EyeWorld.xyz - input.PositionWorld);
    // A skin pixel's metallic channel holds -(its profile slot); skin itself is a dielectric.
    bool isSkin = baseMetallic.w < 0;
    surface.Metallic = max(baseMetallic.w, 0);
    surface.Roughness = normalRoughness.w;
    surface.Emission = emissionCoverage.rgb;
    surface.Alpha = 1;
    // The G-buffer keeps no strand: a hair card resolves with its facing normal and the ordinary lobes.
    surface.StrandWorld = float3(0);
    input.NormalWorld = surface.NormalWorld;
    int lightingView = int(deferredView.LightingView.x + 0.5);
    int isolatedLight = int(deferredView.LightingView.y + 0.5) - 1;
    bool isFinalView = lightingView == int(DEFERRED_LIGHTING_VIEW_FINAL);
    bool shouldSeparate = shouldSeparateSkin == true && isSkin == true && isFinalView == true;
    LightingTerms terms;
    float3 radiance = ShadeMaterialTerms(surface, input, isolatedLight, shouldSeparate, terms).rgb;
    float3 areaDiffuse;
    float3 areaLights = EvaluateRasterMeshLightLobes(surface,
                                                     input.PositionWorld,
                                                     input.PositionUncertaintyWorld,
                                                     value.PositionClip.xy,
                                                     uint(deferredView.TargetSizePixel.z),
                                                     uint(deferredView.TargetSizePixel.w),
                                                     shouldSeparate,
                                                     areaDiffuse);
    radiance += areaLights;
    if (deferredView.LightingView.z > 0)
        radiance += EvaluateWorkLight(surface, input.PositionWorld, isOrthographic);
    output.Diffuse = terms.Diffuse + areaDiffuse;
#ifdef LUDUS_ATMOSPHERE_SURFACE
    if (shouldSeparate == true)
    {
        // The segment is affine in the surface radiance: the spread diffuse light is attenuated like the rest, and
        // the inscattered light is added once, to the unspread part.
        float3 transmittance;
        float3 inscattered;
        IntegrateAtmosphereViewSegment(input.PositionWorld, transmittance, inscattered);
        radiance = radiance * transmittance + inscattered;
        output.Diffuse *= transmittance;
    }
    else
    {
        radiance = ComposeAtmosphereSegment(radiance, input.PositionWorld);
    }
#endif
    output.Diffuse = RemoveSkinPreScatterAlbedo(output.Diffuse, baseMetallic.rgb);
    // The skin resolve keeps every pixel linear for the composite's ToneMap.
    output.Radiance = shouldSeparateSkin == true && isFinalView == true
                          ? radiance
                          : SelectLightingView(lightingView, isolatedLight, radiance, terms, areaLights, input);
    output.Depth = depth;
    return output;
}
DeferredLightingOutput ResolveDeferredLighting(in DeferredVarying value)
{
    DeferredRadiance radiance = ResolveDeferredRadiance(value, false);
    DeferredLightingOutput output;
    output.Color = float4(radiance.Radiance, 1);
    output.Depth = radiance.Depth;
    return output;
}
[shader("fragment")] DeferredLightingOutput DeferredLightingMain(in DeferredVarying value) {
    return ResolveDeferredLighting(value);
}
#ifdef LUDUS_DEFERRED_SKIN
// The resolve of a view with skin (renderer_skin_deferred.slang): linear radiance without the skin diffuse, and the
// skin diffuse that renderer_skin_diffusion.slang spreads and adds back before tone mapping. Both are RGBA16F; the
// radiance's alpha marks covered pixels for the composite.
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
