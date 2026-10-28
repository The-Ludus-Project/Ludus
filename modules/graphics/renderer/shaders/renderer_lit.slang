#include "lighting_layout.h"
#include "math.slang"
#include "raster_bindings.slang"
#include "renderer_environment_layout.h"
// Thanks to Google/Filament authors, Physically Based Rendering in Filament,
// sections 4.4 and 5.2.2, https://google.github.io/filament/main/filament.html.
// GGX + correlated Smith + Schlick, Fresnel-weighted Lambert, single scattering.
// Constant environment is a declared Fresnel approximation, not a DFG/LUT integrator.
struct Parameters
{
    float4 Tint;
    float4 UvTransform;
    float4 Options;
    float4 Pbr;
    float4 Emission;
    float4 EnvironmentIrradiance;
    float4 EnvironmentRadiance;
    float4 BakeScale;
    float4 BakeUv;
#ifdef LUDUS_CLUSTERED
    float4 Lights[512];
#else
    float4 Lights[32];
#endif
    float4 ShadowData[SHADOW_DATA_WORDS];
#ifdef LUDUS_CLUSTERED
    float4 GridRows[3];
    float4 GridMinimum;
    float4 GridScale;
#endif
#ifdef LUDUS_SURFACE_FIDELITY
    float4 SurfaceMap;
#endif
#ifdef LUDUS_AUTHORED_INPUTS
    float4 Authored;
    float4 IrradianceSh[LUDUS_ENVIRONMENT_SH_COEFFICIENTS];
#endif
};
LUDUS_SHADER_BINDING(0) ConstantBuffer<Parameters> material : register(b0);
LUDUS_SHADER_BINDING(1) Texture2D<float4> colorTexture : register(t1);
LUDUS_SHADER_BINDING(2) SamplerState colorSampler : register(s2);
LUDUS_SHADER_BINDING(3) Texture2D<float4> bakeTexture : register(t3);
LUDUS_SHADER_BINDING(4) Texture2D<float4> shadowTexture : register(t4);
LUDUS_SHADER_BINDING(5) SamplerState shadowSampler : register(s5);
#ifdef LUDUS_CLUSTERED
LUDUS_SHADER_BINDING(6) Texture2D<float4> clusterTexture : register(t6);
#    ifdef LUDUS_SURFACE_FIDELITY
LUDUS_SHADER_BINDING(7) Texture2D<float4> surfaceTexture : register(t7);
#    else
LUDUS_SHADER_BINDING(7) SamplerState clusterSampler : register(s7);
#    endif
#endif
#if defined(LUDUS_SURFACE_FIDELITY) && !defined(LUDUS_CLUSTERED)
LUDUS_SHADER_BINDING(6) Texture2D<float4> surfaceTexture : register(t6);
#endif
#ifdef LUDUS_SURFACE_FIDELITY
#    include "renderer_surface_functions.slang"
#endif
#ifdef LUDUS_AUTHORED_INPUTS
#    ifdef LUDUS_CLUSTERED
LUDUS_SHADER_BINDING(8) Texture2D<float4> environmentTexture : register(t8);
LUDUS_SHADER_BINDING(9) SamplerState environmentSampler : register(s9);
LUDUS_SHADER_BINDING(10) Texture2D<float4> emissionTexture : register(t10);
#    else
LUDUS_SHADER_BINDING(7) Texture2D<float4> environmentTexture : register(t7);
LUDUS_SHADER_BINDING(8) SamplerState environmentSampler : register(s8);
LUDUS_SHADER_BINDING(9) Texture2D<float4> emissionTexture : register(t9);
#    endif
#endif
struct Input
{
    LUDUS_SHADER_LOCATION(0) float3 PositionMesh : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 MeshToClipRow0 : TEXCOORD0;
    LUDUS_SHADER_LOCATION(2) float4 MeshToClipRow1 : TEXCOORD1;
    LUDUS_SHADER_LOCATION(3) float4 MeshToClipRow2 : TEXCOORD2;
    LUDUS_SHADER_LOCATION(4) float4 MeshToClipRow3 : TEXCOORD3;
    LUDUS_SHADER_LOCATION(5) float4 Color : COLOR0;
    LUDUS_SHADER_LOCATION(6) float2 TexcoordUv : TEXCOORD4;
    LUDUS_SHADER_LOCATION(7) float3 NormalMesh : NORMAL;
    LUDUS_SHADER_LOCATION(8) float4 MeshToWorldRow0 : TEXCOORD5;
    LUDUS_SHADER_LOCATION(9) float4 MeshToWorldRow1 : TEXCOORD6;
    LUDUS_SHADER_LOCATION(10) float4 MeshToWorldRow2 : TEXCOORD7;
#ifdef LUDUS_AUTHORED_INPUTS
    LUDUS_SHADER_LOCATION(11) float4 TangentMesh : TEXCOORD8;
    LUDUS_SHADER_LOCATION(12) float4 ColorVertex : TEXCOORD9;
    LUDUS_SHADER_LOCATION(13) float2 MaterialUv1 : TEXCOORD10;
#else
    LUDUS_SHADER_LOCATION(11) float4 NormalMeshToWorldRow0 : TEXCOORD8;
    LUDUS_SHADER_LOCATION(12) float4 NormalMeshToWorldRow1 : TEXCOORD9;
    LUDUS_SHADER_LOCATION(13) float4 NormalMeshToWorldRow2 : TEXCOORD10;
#endif
    LUDUS_SHADER_LOCATION(14) float4 EyeWorld : TEXCOORD11;
    LUDUS_SHADER_LOCATION(15) float2 BakeUv : TEXCOORD12;
};
struct Varying
{
    float4 PositionClip : SV_Position;
    float2 TexcoordUv : TEXCOORD0;
    float3 PositionWorld : TEXCOORD1;
    float3 NormalWorld : TEXCOORD2;
    float4 EyeWorld : TEXCOORD3;
    float2 BakeUv : TEXCOORD4;
    float IsBakeValid : TEXCOORD5;
#ifdef LUDUS_AUTHORED_INPUTS
    float4 TangentWorld : TEXCOORD7;
    float4 ColorVertex : TEXCOORD8;
    float2 MaterialUv1 : TEXCOORD9;
#endif
#ifdef LUDUS_CLUSTERED
    float IsDirectReference : TEXCOORD6;
#endif
#ifdef LUDUS_DEBUG_VIEW
    // Last member, so ordinary shadow fragments still match the earlier varying locations.
    float2 DebugDepthRange : TEXCOORD10;
#endif
};

[shader("vertex")] Varying VertexMain(Input input) {
    Varying output;
    float4 p = float4(input.PositionMesh, 1);
    output.PositionClip = float4(dot(input.MeshToClipRow0, p),
                                 dot(input.MeshToClipRow1, p),
                                 dot(input.MeshToClipRow2, p),
                                 dot(input.MeshToClipRow3, p));
    output.PositionWorld =
        float3(dot(input.MeshToWorldRow0, p), dot(input.MeshToWorldRow1, p), dot(input.MeshToWorldRow2, p));
#ifdef LUDUS_AUTHORED_INPUTS
    float3 column0World = float3(input.MeshToWorldRow0.x, input.MeshToWorldRow1.x, input.MeshToWorldRow2.x);
    float3 column1World = float3(input.MeshToWorldRow0.y, input.MeshToWorldRow1.y, input.MeshToWorldRow2.y);
    float3 column2World = float3(input.MeshToWorldRow0.z, input.MeshToWorldRow1.z, input.MeshToWorldRow2.z);
    // Positive common rescaling keeps cross/determinant products finite without
    // changing normal direction or mirrored handedness.
    float magnitude = max(max(max(abs(column0World.x), abs(column0World.y)), abs(column0World.z)),
                          max(max(max(abs(column1World.x), abs(column1World.y)), abs(column1World.z)),
                              max(max(abs(column2World.x), abs(column2World.y)), abs(column2World.z))));
    column0World /= magnitude;
    column1World /= magnitude;
    column2World /= magnitude;
    float determinant = dot(column0World, cross(column1World, column2World));
    output.NormalWorld = NormalizeOrZero((cross(column1World, column2World) * input.NormalMesh.x +
                                          cross(column2World, column0World) * input.NormalMesh.y +
                                          cross(column0World, column1World) * input.NormalMesh.z) /
                                         determinant);
    float3 tangentWorld =
        column0World * input.TangentMesh.x + column1World * input.TangentMesh.y + column2World * input.TangentMesh.z;
    output.TangentWorld = float4(tangentWorld, input.TangentMesh.w * (determinant < 0 ? -1 : 1));
    output.ColorVertex = input.ColorVertex;
    output.MaterialUv1 = input.MaterialUv1;
#else
    output.NormalWorld = NormalizeOrZero(float3(dot(input.NormalMeshToWorldRow0.xyz, input.NormalMesh),
                                                dot(input.NormalMeshToWorldRow1.xyz, input.NormalMesh),
                                                dot(input.NormalMeshToWorldRow2.xyz, input.NormalMesh)));
#endif
    output.TexcoordUv = input.TexcoordUv;
    output.EyeWorld = input.EyeWorld;
    output.BakeUv = input.BakeUv;
    output.IsBakeValid = input.Color.x;
#ifdef LUDUS_CLUSTERED
    output.IsDirectReference = input.Color.y;
#endif
#ifdef LUDUS_DEBUG_VIEW
    output.DebugDepthRange = input.Color.zw;
#endif
    return output;
} float ComputeFifth(float x)
{
    float square = x * x;
    return square * square * x;
}
float3 EvaluateBrdf(float3 baseColor,
                    float3 normalWorld,
                    float3 viewDirectionWorld,
                    float3 lightDirectionWorld,
                    float metallic,
                    float roughness,
                    bool isBakedDiffuse)
{
    float nv = max(dot(normalWorld, viewDirectionWorld), 0);
    float nl = max(dot(normalWorld, lightDirectionWorld), 0);
    float3 halfSumWorld = viewDirectionWorld + lightDirectionWorld;
    if (nv <= 0 || nl <= 0 || dot(halfSumWorld, halfSumWorld) < 1e-12)
        return float3(0);
    float3 halfDirectionWorld = NormalizeOrZero(halfSumWorld);
    float nh = saturate(dot(normalWorld, halfDirectionWorld));
    float lh = saturate(dot(lightDirectionWorld, halfDirectionWorld));
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (1 - nh * nh) + nh * nh * a2;
    float distribution = a2 / (PI * d * d);
    float visibility = 0.5 / max(nl * sqrt(nv * nv * (1 - a2) + a2) + nv * sqrt(nl * nl * (1 - a2) + a2), 1e-6);
    float3 f0 = lerp(float3(0.04), baseColor, metallic);
    float3 fresnel = f0 + (1 - f0) * ComputeFifth(1 - lh);
    return nl * (distribution * visibility * fresnel +
                 baseColor * (1 - metallic) * (1 - fresnel) * (isBakedDiffuse ? 0 : 1) / PI);
}
// Instance flags in EyeWorld.w: bit 0 receives shadows, bit 1 orthographic eye. The value is
// constant per instance but reaches the fragment through a perspective-interpolated varying,
// so it may arrive as 0.9999...; round instead of truncating or whole pixel rows lose a flag.
int GetEyeFlags(Varying input)
{
    return int(input.EyeWorld.w + 0.5);
}
float ComputeShadowVisibility(int light, Varying input)
{
    if ((GetEyeFlags(input) & 1) == 0)
        return 1;
    bool hasLocalShadowCoverage = false;
    for (int region = 0; region < SHADOW_REGION_CAPACITY; ++region)
    {
        if (region >= int(material.Pbr.w))
            break;
        float4 settings = material.ShadowData[region * SHADOW_REGION_WORDS + 5];
        if (int(settings.y) != light)
            continue;
        hasLocalShadowCoverage = hasLocalShadowCoverage || material.Lights[light * 4].w >= 1;
        float4 p = float4(input.PositionWorld, 1);
        float4 clip = float4(dot(material.ShadowData[region * SHADOW_REGION_WORDS], p),
                             dot(material.ShadowData[region * SHADOW_REGION_WORDS + 1], p),
                             dot(material.ShadowData[region * SHADOW_REGION_WORDS + 2], p),
                             dot(material.ShadowData[region * SHADOW_REGION_WORDS + 3], p));
        if (clip.w <= 1e-6)
            continue;
        float3 ndc = clip.xyz / clip.w;
        if (abs(ndc.x) > 1 || abs(ndc.y) > 1 || ndc.z < 0 || ndc.z > 1)
            continue;
        float4 rect = material.ShadowData[region * SHADOW_REGION_WORDS + 4];
        float2 uv = lerp(rect.xy, rect.zw, float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5));
        float visible = 0;
        for (int tap = 0; tap < 4; ++tap)
        {
            float2 delta = float2((tap & 1) != 0 ? 0.5 : -0.5, (tap & 2) != 0 ? 0.5 : -0.5) * settings.zw;
            float2 sampleUv = clamp(uv + delta, rect.xy, rect.zw);
            float2 depth = shadowTexture.SampleLevel(shadowSampler, sampleUv, 0).rg;
            float stored = dot(depth, float2(65280, 255)) / 65535;
            visible += ndc.z - settings.x <= stored ? 0.25 : 0;
        }
        return visible;
    }
    return hasLocalShadowCoverage
               ? 0
               : 1; // Local coverage gaps conservatively occlude; directional cascade contract stays explicit.
}
float4 SampleBaseTexel(Varying input)
{
    float2 materialUv = input.TexcoordUv;
#ifdef LUDUS_AUTHORED_INPUTS
    if (material.Authored.y > .5)
        materialUv = input.MaterialUv1;
#endif
    float2 uv = materialUv * material.UvTransform.xy + material.UvTransform.zw;
    float4 texel = material.Options.y < 0 ? colorTexture.Sample(colorSampler, uv)
                                          : colorTexture.SampleLevel(colorSampler, uv, material.Options.y);
    float4 baseColor = texel * material.Tint;
#ifdef LUDUS_AUTHORED_INPUTS
    if (material.Authored.x > .5)
        baseColor *= input.ColorVertex;
#endif
    if (material.Options.z == 1 && baseColor.a < material.Options.x)
        discard;
    return baseColor;
}
#ifdef LUDUS_AUTHORED_INPUTS
// Thanks to Guy/Agopian, Filament sections 5.3 and 9.6 (URL above): SH diffuse
// and prefiltered GGX split-sum IBL. DFG uses Lazarov's fit documented by Filament.
float2 ComputeEnvironmentUv(in float3 directionWorld)
{
    float longitude = atan2(directionWorld.z, directionWorld.x) / 6.283185307179586;
    return float2(longitude - floor(longitude), atan2(length(directionWorld.xz), directionWorld.y) / PI);
}
float3 EvaluateEnvironmentIrradiance(in float3 normalWorld)
{
    float basis[LUDUS_ENVIRONMENT_SH_COEFFICIENTS] = {
        .2820947918,
        .4886025119 * normalWorld.y,
        .4886025119 * normalWorld.z,
        .4886025119 * normalWorld.x,
        1.0925484306 * normalWorld.x * normalWorld.y,
        1.0925484306 * normalWorld.y * normalWorld.z,
        .3153915653 * (3 * normalWorld.z * normalWorld.z - 1),
        1.0925484306 * normalWorld.x * normalWorld.z,
        .5462742153 * (normalWorld.x * normalWorld.x - normalWorld.y * normalWorld.y)};
    float3 irradiance = float3(0);
    for (int coefficient = 0; coefficient < LUDUS_ENVIRONMENT_SH_COEFFICIENTS; ++coefficient)
        irradiance += material.IrradianceSh[coefficient].rgb * basis[coefficient];
    return max(irradiance, float3(0));
}
float2 EvaluateEnvironmentDfg(float roughness, float cosineView)
{
    float4 coefficients = roughness * float4(-1, -.0275, -.572, .022) + float4(1, .0425, 1.04, -.04);
    float approximation =
        min(coefficients.x * coefficients.x, exp2(-9.28 * cosineView)) * coefficients.x + coefficients.y;
    return float2(-1.04, 1.04) * approximation + coefficients.zw;
}
#endif
#ifdef LUDUS_DEBUG_VIEW
// Largest display-linear value the inverse reproduces (about 0.998 sRGB); mirrors PBR_NEUTRAL_INVERSE_LIMIT.
static const float DEBUG_DISPLAY_LINEAR_LIMIT = 0.995;
// Khronos PBR Neutral constants, as in renderer_output.slang.
static const float PBR_NEUTRAL_FRESNEL = 0.04;
static const float PBR_NEUTRAL_START = 0.8 - PBR_NEUTRAL_FRESNEL;
static const float PBR_NEUTRAL_DESATURATION = 0.15;
// Scene-linear colour that renderer_output.slang's PBR Neutral curve maps to a wanted display-linear colour;
// mirrors InvertPbrNeutral in tone_mapping.hpp.
float3 InvertPbrNeutral(float3 display)
{
    float peak = max(display.x, max(display.y, display.z));
    float3 shifted = display;
    if (peak > PBR_NEUTRAL_START)
    {
        float headroom = 1 - PBR_NEUTRAL_START;
        float oldPeak = headroom * headroom / (1 - peak) - 1 + 2 * PBR_NEUTRAL_START;
        float mixAmount = 1 / (PBR_NEUTRAL_DESATURATION * (oldPeak - peak) + 1);
        float scale = peak / oldPeak * mixAmount;
        shifted = max((display - peak * (1 - mixAmount)) / scale, float3(0));
    }
    float smallest = min(shifted.x, min(shifted.y, shifted.z));
    float original =
        smallest < PBR_NEUTRAL_FRESNEL ? 2 * sqrt(PBR_NEUTRAL_FRESNEL * smallest) : smallest + PBR_NEUTRAL_FRESNEL;
    float offset = original <= 2 * PBR_NEUTRAL_FRESNEL ? original - original * original / (4 * PBR_NEUTRAL_FRESNEL)
                                                       : PBR_NEUTRAL_FRESNEL;
    return shifted + offset;
}
// Inverts RasterViewport's presentation (renderer_output.slang: PBR Neutral, then sRGB encode at
// exposure one) so a diagnostic colour authored in display sRGB appears unchanged on screen.
float3 EncodeDebugDisplay(float3 displaySrgb)
{
    float3 encoded = saturate(displaySrgb);
    float3 linearColor = float3(encoded.x <= 0.04045 ? encoded.x / 12.92 : pow((encoded.x + 0.055) / 1.055, 2.4),
                                encoded.y <= 0.04045 ? encoded.y / 12.92 : pow((encoded.y + 0.055) / 1.055, 2.4),
                                encoded.z <= 0.04045 ? encoded.z / 12.92 : pow((encoded.z + 0.055) / 1.055, 2.4));
    return InvertPbrNeutral(min(linearColor, float3(DEBUG_DISPLAY_LINEAR_LIMIT)));
}
// Perceptually uniform viridis ramp over t in [0,1], dark purple through teal to yellow, as display-space sRGB.
// Thanks to Matt Zucker, "Viridis approximation" (Shadertoy WlfXRN), a degree-six polynomial fit of
// matplotlib's viridis (Nathaniel Smith and Stefan van der Walt, CC0).
float3 ComputeRampColour(float t)
{
    const float3 coefficient0 = float3(0.2777273272234177, 0.005407344544966578, 0.3340998053353061);
    const float3 coefficient1 = float3(0.1050930431085774, 1.404613529898575, 1.384590162594685);
    const float3 coefficient2 = float3(-0.3308618287255563, 0.214847559468213, 0.09509516302823659);
    const float3 coefficient3 = float3(-4.634230498983486, -5.799100973351585, -19.33244095627987);
    const float3 coefficient4 = float3(6.228269936347081, 14.17993336680509, 56.69055260068105);
    const float3 coefficient5 = float3(4.776384997670288, -13.74514537774601, -65.35303263337234);
    const float3 coefficient6 = float3(-5.435455855934631, 4.645852612178535, 26.3124352495832);
    float x = saturate(t);
    return saturate(
        coefficient0 +
        x * (coefficient1 +
             x * (coefficient2 + x * (coefficient3 + x * (coefficient4 + x * (coefficient5 + x * coefficient6))))));
}
float3 ComputeDebugColour(int mode,
                          Varying input,
                          float3 normalWorld,
                          float metallic,
                          float roughness,
                          int lightCount,
                          float3 shaded)
{
    if (mode == int(MATERIAL_DEBUG_VIEW_NORMAL_WORLD))
        return normalWorld * 0.5 + 0.5;
    if (mode == int(MATERIAL_DEBUG_VIEW_LINEAR_DEPTH))
    {
        // Perspective: radial distance from the eye. Orthographic (eye flag bit 1): distance
        // along the view direction; the renderer shifted the range into this frame.
        float distance = (GetEyeFlags(input) & 2) != 0 ? -dot(input.PositionWorld, input.EyeWorld.xyz)
                                                       : length(input.PositionWorld - input.EyeWorld.xyz);
        float span = input.DebugDepthRange.y - input.DebugDepthRange.x;
        return ComputeRampColour(span > 0 ? (distance - input.DebugDepthRange.x) / span : 0);
    }
    if (mode == int(MATERIAL_DEBUG_VIEW_ROUGHNESS))
        return float3(roughness);
    if (mode == int(MATERIAL_DEBUG_VIEW_METALLIC))
        return float3(metallic);
    if (mode == int(MATERIAL_DEBUG_VIEW_TEXCOORD_UV))
        return float3(frac(input.TexcoordUv), 0);
    if (mode == int(MATERIAL_DEBUG_VIEW_LIGHT_COUNT))
        return ComputeRampColour(float(lightCount) / float(MATERIAL_DEBUG_LIGHT_COUNT_SATURATION));
    return shaded;
}
#endif
[shader("fragment")] float4 FragmentMain(Varying input) : SV_Target
{
    float4 baseColor = SampleBaseTexel(input);
    // Thanks to the W3C GPU for the Web Working Group, WGSL, "Derivative Built-in Functions":
    // https://www.w3.org/TR/WGSL/#derivative-builtin-functions
    // Evaluate quad derivatives before the per-fragment tangent-frame choice.
    float3 positionDerivativeXWorld = ddx(input.PositionWorld);
    float3 positionDerivativeYWorld = ddy(input.PositionWorld);
    float3 normalWorld = NormalizeOrZero(input.NormalWorld);
    float3 viewDirectionWorld = (GetEyeFlags(input) & 2) != 0
                                    ? NormalizeOrZero(input.EyeWorld.xyz)
                                    : NormalizeOrZero(input.EyeWorld.xyz - input.PositionWorld);
    float metallic = material.Pbr.x;
    float roughness = material.Pbr.y;
#ifdef LUDUS_SURFACE_FIDELITY
    if (material.SurfaceMap.y > .5)
    {
        float2 materialUv = input.TexcoordUv;
#    ifdef LUDUS_AUTHORED_INPUTS
        if (material.Authored.y > .5)
            materialUv = input.MaterialUv1;
#    endif
        float2 uv = materialUv * material.UvTransform.xy + material.UvTransform.zw;
        float4 texel = material.Options.y < 0 ? surfaceTexture.Sample(colorSampler, uv)
                                              : surfaceTexture.SampleLevel(colorSampler, uv, material.Options.y);
        SurfaceParameters parameters = DecodeSurfaceParameters(texel, metallic, roughness, material.SurfaceMap.x);
        metallic = parameters.Metallic;
        roughness = parameters.Roughness;
        float2 uvDerivativeXMaterial = ddx(uv);
        float2 uvDerivativeYMaterial = ddy(uv);
#    ifdef LUDUS_AUTHORED_INPUTS
        if (material.Authored.x > .5 && abs(input.TangentWorld.w) > .5)
        {
            float3 tangentWorld =
                NormalizeOrZero(input.TangentWorld.xyz - normalWorld * dot(normalWorld, input.TangentWorld.xyz));
            float3 bitangentWorld = cross(normalWorld, tangentWorld) * input.TangentWorld.w;
            normalWorld =
                NormalizeOrZero(tangentWorld * parameters.NormalTangent.x +
                                bitangentWorld * parameters.NormalTangent.y + normalWorld * parameters.NormalTangent.z);
        }
        else
#    endif
            normalWorld = ApplyNormalWorld(normalWorld,
                                           parameters.NormalTangent,
                                           positionDerivativeXWorld,
                                           positionDerivativeYWorld,
                                           uvDerivativeXMaterial,
                                           uvDerivativeYMaterial);
    }
#endif
    float nv = max(dot(normalWorld, viewDirectionWorld), 0);
    float3 f0 = lerp(float3(0.04), baseColor.rgb, metallic);
    float3 f = f0 + (1 - f0) * ComputeFifth(1 - nv);
    float3 irradiance = material.EnvironmentIrradiance.rgb;
    float3 specularEnvironment = material.EnvironmentRadiance.rgb * f;
#ifdef LUDUS_AUTHORED_INPUTS
    if (material.Authored.z > .5)
    {
        irradiance = EvaluateEnvironmentIrradiance(normalWorld);
        float3 reflectionWorld = reflect(-viewDirectionWorld, normalWorld);
        float3 radiance =
            environmentTexture
                .SampleLevel(environmentSampler, ComputeEnvironmentUv(reflectionWorld), roughness * material.Authored.w)
                .rgb *
            material.IrradianceSh[0].w;
        float2 dfg = EvaluateEnvironmentDfg(roughness, nv);
        specularEnvironment = radiance * (f0 * dfg.x + dfg.y);
    }
#endif
    if (input.IsBakeValid > 0.5)
        irradiance =
            bakeTexture.SampleLevel(colorSampler, saturate(input.BakeUv) * material.BakeUv.xy + material.BakeUv.zw, 0)
                .rgb *
            material.BakeScale.rgb;
    float3 geometricNormalWorld = cross(positionDerivativeXWorld, positionDerivativeYWorld);
    if (dot(geometricNormalWorld, input.NormalWorld) < 0)
        geometricNormalWorld = -geometricNormalWorld;
    float3 result = material.Emission.w > .5 || dot(geometricNormalWorld, viewDirectionWorld) > 0
                        ? material.Emission.rgb
                        : float3(0);
#ifdef LUDUS_AUTHORED_INPUTS
    if (material.SurfaceMap.z > .5)
    {
        float2 emissionUv =
            (material.Authored.y > .5 ? input.MaterialUv1 : input.TexcoordUv) * material.UvTransform.xy +
            material.UvTransform.zw;
        float3 emission = material.Options.y < 0
                              ? emissionTexture.Sample(colorSampler, emissionUv).rgb
                              : emissionTexture.SampleLevel(colorSampler, emissionUv, material.Options.y).rgb;
        result *= emission;
    }
#endif
    if (nv > 0)
        result += irradiance * baseColor.rgb * (1 - metallic) * (1 - f) / PI + specularEnvironment;
#ifdef LUDUS_DEBUG_VIEW
    int lightCount = 0;
#endif
#ifdef LUDUS_CLUSTERED
    float4 world = float4(input.PositionWorld, 1);
    float3 grid =
        (float3(dot(material.GridRows[0], world), dot(material.GridRows[1], world), dot(material.GridRows[2], world)) -
         material.GridMinimum.xyz) *
        material.GridScale.xyz;
    bool direct = input.IsDirectReference > 0.5 || any(grid < 0) || any(grid >= 8);
    int3 cell = int3(clamp(grid, float3(0), float3(7)));
    int start = (cell.x + cell.y * 8 + cell.z * 64) * 32;
    float4 first = clusterTexture.SampleLevel(
#    ifdef LUDUS_SURFACE_FIDELITY
        shadowSampler,
#    else
        clusterSampler,
#    endif
        (float2((start / 4) % 64, (start / 4) / 64) + 0.5) / 64,
        0);
    direct = direct || int(first.x * 255 + 0.5) == 255;
    for (int slot = 0; slot < 128; ++slot)
    {
        int i = slot;
        if (direct)
        {
            if (slot >= int(material.Pbr.z))
                break;
        }
        else
        {
            if (slot >= 32)
                break;
            int at = start + slot;
            float4 packed = clusterTexture.SampleLevel(
#    ifdef LUDUS_SURFACE_FIDELITY
                shadowSampler,
#    else
                clusterSampler,
#    endif
                (float2((at / 4) % 64, (at / 4) / 64) + 0.5) / 64,
                0);
            i = int(packed[at % 4] * 255 + 0.5) - 1;
            if (i < 0)
                break;
        }
#else
    for (int i = 0; i < 8; ++i)
    {
        if (i >= int(material.Pbr.z))
            break;
#endif
        float4 color = material.Lights[i * 4];
        float4 position = material.Lights[i * 4 + 1];
        float4 direction = material.Lights[i * 4 + 2];
        float4 extra = material.Lights[i * 4 + 3];
        float3 lightDirectionWorld = -direction.xyz;
        float attenuation = 1;
        if (color.w != 0)
        {
            float3 delta = position.xyz - input.PositionWorld;
            float distance2 = dot(delta, delta);
            lightDirectionWorld = NormalizeOrZero(delta);
            float ratio = distance2 / (position.w * position.w);
            float edge = max(1 - ratio * ratio, 0);
            attenuation = edge * edge / max(distance2, 1e-4);
            if (color.w == 2)
            {
                float cone = saturate((dot(-lightDirectionWorld, direction.xyz) - direction.w) /
                                      max(extra.x - direction.w, 1e-6));
                attenuation *= cone * cone;
            }
            if (color.w == 3 || color.w == 4)
            {
                float emitterCosine = dot(-lightDirectionWorld, direction.xyz);
                attenuation *= color.w == 4 ? abs(emitterCosine) : max(emitterCosine, 0);
            }
        }
        float3 contribution = EvaluateBrdf(baseColor.rgb,
                                           normalWorld,
                                           viewDirectionWorld,
                                           lightDirectionWorld,
                                           metallic,
                                           roughness,
                                           input.IsBakeValid > 0.5 && extra.y > 0.5) *
                              color.rgb * attenuation * ComputeShadowVisibility(i, input);
        result += contribution;
#ifdef LUDUS_DEBUG_VIEW
        lightCount += any(contribution > 0) ? 1 : 0;
#endif
    }
#ifdef LUDUS_DEBUG_VIEW
    int debugView = int(material.Options.w + 0.5);
    if (debugView != int(MATERIAL_DEBUG_VIEW_NONE))
        return float4(EncodeDebugDisplay(
                          ComputeDebugColour(debugView, input, normalWorld, metallic, roughness, lightCount, result)),
                      1);
#endif
    float alpha = material.Options.z == 2 ? baseColor.a : 1;
    return float4(min(max(result, float3(0)), float3(65504)) * alpha, alpha);
}
[shader("fragment")] float4 ShadowMain(Varying input) : SV_Target
{
    SampleBaseTexel(input);
    float encoded = floor(saturate(input.PositionClip.z) * 65535 + 0.5);
    float high = floor(encoded / 256);
    return float4(high / 255, (encoded - high * 256) / 255, 0, 1);
}
