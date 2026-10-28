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
[shader("fragment")] GBufferOutput GBufferMain(in Varying input) { return PackDeferredMaterial(SampleMaterial(input)); }
