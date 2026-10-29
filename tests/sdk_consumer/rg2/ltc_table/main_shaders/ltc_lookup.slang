// The renderer binds its LTC table to this kernel because the ray technique sets
// scene_interface.polygonal_light_sampling and LookupSpecularLtc reads it. Column c of the target holds case c of
// ltc_lookup_cases.h as unit-range values the host compares with the CPU lookup: red is Norm, green FresnelNorm / Norm
// and blue a bounded function of the inverse transform, which is what the F0 axis changes. Columns past the last case
// are black.
#include "ltc_lookup_cases.h"

float3 ShadeLtcLookup(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    const uint column = launch.PositionPixel.x;
    if (column >= LTC_LOOKUP_CASE_COUNT)
    {
        return float3(0);
    }
    const uint roughnessIndex = column % LTC_LOOKUP_ROUGHNESS_COUNT;
    const uint viewIndex = (column / LTC_LOOKUP_ROUGHNESS_COUNT) % LTC_LOOKUP_VIEW_COUNT;
    const uint fresnelIndex = column / (LTC_LOOKUP_ROUGHNESS_COUNT * LTC_LOOKUP_VIEW_COUNT);
    const float roughness = (float(roughnessIndex) + 0.5) / LTC_LOOKUP_ROUGHNESS_COUNT;
    const float viewCosine = (float(viewIndex) + 0.5) / LTC_LOOKUP_VIEW_COUNT;
    const float f0 = float(fresnelIndex) / (LTC_LOOKUP_FRESNEL_COUNT - 1);
    const float3 viewTangent = float3(sqrt(1 - viewCosine * viewCosine), 0, viewCosine);
    const SpecularLtc ltc = LookupSpecularLtc(roughness, viewTangent, float3(f0));
    const float spread = abs(ltc.InverseTransform[0].z) + abs(ltc.InverseTransform[1].y) +
                         abs(ltc.InverseTransform[2].x) + abs(ltc.InverseTransform[2].z);
    return float3(ltc.Norm, ltc.FresnelNorm / ltc.Norm, 1 / (1 + spread));
}
