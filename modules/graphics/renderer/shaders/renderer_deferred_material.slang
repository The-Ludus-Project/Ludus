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
    output.BaseMetallic = float4(surface.BaseColor.rgb, surface.Metallic);
    output.EmissionCoverage = float4(min(surface.Emission, float3(DEFERRED_MAX_HALF)), 1);
    return output;
}
#ifdef LUDUS_GRAPH_PROBE_GI
// Normal-incidence reflectance of dielectrics, as in ShadeMaterial's diffuse Fresnel weight.
static const float DEFERRED_PROBE_DIELECTRIC_F0 = 0.04;
// GI Probe Receivers of a pass with Baked Lighting connected. The resolve adds emission once, so storing the
// receiver's probe bounce there as ShadeMaterial's forward diffuse term keeps both paths' radiance equal, while the
// live direct, specular and environment terms still come from the resolve. Materials without a probe volume
// (BakeScale.w >= 0) pack unchanged.
GBufferOutput PackDeferredProbeMaterial(in SurfaceSample surface, in Varying input)
{
    float nv = max(dot(surface.NormalWorld, surface.ViewDirectionWorld), 0);
    if (nv > 0 && material.BakeScale.w < 0)
    {
        float3 f0 = lerp(float3(DEFERRED_PROBE_DIELECTRIC_F0), surface.BaseColor.rgb, surface.Metallic);
        float3 f = f0 + (1 - f0) * ComputeFifth(1 - nv);
        surface.Emission += EvaluateGiProbeIrradiance(input.PositionWorld, surface.NormalWorld) *
                            surface.BaseColor.rgb * (1 - surface.Metallic) * (1 - f) / PI;
    }
    return PackDeferredMaterial(surface);
}
#endif
[shader("fragment")] GBufferOutput GBufferMain(in Varying input) { return PackDeferredMaterial(SampleMaterial(input)); }
