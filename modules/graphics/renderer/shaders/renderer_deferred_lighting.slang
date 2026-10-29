#define LUDUS_DEFERRED_MESH_LIGHTS 1
#include "deferred_layout.h"
#include "renderer_clustered_authored.slang"
#include "renderer_deferred_fullscreen.slang"
#include "renderer_mesh_lights.slang"

struct DeferredViewParameters
{
    float4 ClipToWorldRows[DEFERRED_VIEW_ROWS];
    float4 EyeWorld;
    float4 TargetSizePixel; // XY: dimensions in pixels. Z: mesh-light triangle count. W: reserved.
    float4 Options;
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

struct DeferredLightingOutput
{
    float4 Color : SV_Target0;
    float Depth : SV_Depth;
};
DeferredLightingOutput ResolveDeferredLighting(in DeferredVarying value)
{
    int2 positionPixel = int2(value.PositionClip.xy);
    float4 emissionCoverage = emissionCoverageTexture.Load(int3(positionPixel, 0));
    DeferredLightingOutput output;
    output.Color = float4(0, 0, 0, 1);
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
    input.EyeWorld = deferredView.EyeWorld;
    input.IsDirectReference = deferredView.Options.x;
    float4 normalRoughness = normalRoughnessTexture.Load(int3(positionPixel, 0));
    float4 baseMetallic = baseMetallicTexture.Load(int3(positionPixel, 0));
    SurfaceSample surface;
    surface.BaseColor = float4(baseMetallic.rgb, 1);
    surface.NormalWorld = NormalizeOrZero(normalRoughness.xyz);
    surface.ViewDirectionWorld = (GetEyeFlags(input) & 2) != 0
                                     ? NormalizeOrZero(input.EyeWorld.xyz)
                                     : NormalizeOrZero(input.EyeWorld.xyz - input.PositionWorld);
    surface.Metallic = baseMetallic.w;
    surface.Roughness = normalRoughness.w;
    surface.Emission = emissionCoverage.rgb;
    surface.Alpha = 1;
    input.NormalWorld = surface.NormalWorld;
    output.Color = ShadeMaterial(surface, input);
    output.Color.rgb += EvaluateRasterMeshLights(surface, input.PositionWorld, uint(deferredView.TargetSizePixel.z));
    output.Depth = depth;
    return output;
}
[shader("fragment")] DeferredLightingOutput DeferredLightingMain(in DeferredVarying value) {
    return ResolveDeferredLighting(value);
}
