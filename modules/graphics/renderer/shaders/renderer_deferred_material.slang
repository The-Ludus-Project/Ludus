#include "deferred_layout.h"
#include "renderer_clustered_authored.slang"

// Thanks to Ola Olsson, Markus Billeter and Ulf Assarsson, "Clustered Deferred and
// Forward Shading", HPG 2012, pp. 87-96, https://doi.org/10.2312/EGGH/HPG12/087-096.
// Material attributes are stored before clustered lighting. This portable profile
// retains L5's CPU-assigned world-space cells and full-list overflow fallback.
struct GBufferOutput
{
    float4 NormalRoughnessWorld : SV_Target0;
    float4 BaseMetallic : SV_Target1;
    float4 EmissionCoverage : SV_Target2;
};
GBufferOutput PackDeferredMaterial(in SurfaceSample surface)
{
    GBufferOutput output;
    output.NormalRoughnessWorld = float4(surface.NormalWorld, surface.Roughness);
    // Skin is a dielectric, so its metallic channel holds its skin profile instead: -(one-based slot)
    // (DEFERRED_MAX_SKIN_PROFILES). Emission and coverage keep their usual meaning.
    output.BaseMetallic =
        float4(surface.BaseColor.rgb, surface.SkinProfile > .5 ? -surface.SkinProfile : surface.Metallic);
    output.EmissionCoverage = float4(min(surface.Emission, float3(DEFERRED_MAX_HALF)), float(DEFERRED_COVERED));
    return output;
}
#if defined(LUDUS_GRAPH_PROBE_GI) || defined(LUDUS_GRAPH_SURFACE_CHART)
// Normal-incidence reflectance of dielectrics, as in ShadeMaterial's diffuse Fresnel weight.
static const float DEFERRED_DIELECTRIC_F0 = 0.04;
// ShadeMaterial's weight of a baked irradiance in reflected radiance: its Fresnel-weighted Lambert term, or zero when
// the surface faces away from the eye. The resolve adds emission once, so storing a baked term there as this weight
// keeps the deferred and forward paths' radiance equal while live direct, specular and environment terms still come
// from the resolve.
float3 ComputeDeferredBakedDiffuseWeight(in SurfaceSample surface)
{
    float nv = max(dot(surface.NormalWorld, surface.ViewDirectionWorld), 0);
    if (nv <= 0)
        return float3(0);
    float3 f0 = lerp(float3(DEFERRED_DIELECTRIC_F0), surface.BaseColor.rgb, surface.Metallic);
    float3 f = f0 + (1 - f0) * ComputeFifth(1 - nv);
    return surface.BaseColor.rgb * (1 - surface.Metallic) * (1 - f) / PI;
}
#endif
#ifdef LUDUS_GRAPH_PROBE_GI
// GI Probe Receivers of a pass with Baked Lighting connected store their probe bounce in emission. Materials without
// a probe volume (BakeScale.w >= 0) pack unchanged.
GBufferOutput PackDeferredProbeMaterial(in SurfaceSample surface, in Varying input)
{
    if (material.BakeScale.w < 0)
    {
        surface.Emission += EvaluateGiProbeIrradiance(input.PositionWorld, surface.NormalWorld) *
                            ComputeDeferredBakedDiffuseWeight(surface);
    }
    return PackDeferredMaterial(surface);
}
#endif
#ifdef LUDUS_GRAPH_SURFACE_CHART
// Static surfaces with a ready chart in a pass with Baked Lighting connected. As in the forward chart fragment, the
// chart irradiance adds to the live environment's (the graph's charts set BakeScale.w to one) and replaces the baked
// lights' diffuse. Emission stores the chart term, and the chart coverage makes the resolve omit those lights' live
// diffuse while keeping their specular.
GBufferOutput PackDeferredSurfaceChartMaterial(in SurfaceSample surface, in Varying input)
{
    float3 bakedIrradiance =
        bakeTexture.SampleLevel(colorSampler, saturate(input.BakeUv) * material.BakeUv.xy + material.BakeUv.zw, 0).rgb *
        material.BakeScale.rgb;
    surface.Emission += bakedIrradiance * ComputeDeferredBakedDiffuseWeight(surface);
    GBufferOutput output = PackDeferredMaterial(surface);
    output.EmissionCoverage.w = float(DEFERRED_SURFACE_CHART_COVERED);
    return output;
}
#endif
[shader("fragment")] GBufferOutput GBufferMain(in Varying input) { return PackDeferredMaterial(SampleMaterial(input)); }
