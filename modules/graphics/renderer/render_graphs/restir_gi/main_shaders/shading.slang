#pragma once
#include "reservoir.slang"

// The common linear indirect-light integrand at a visible point. Visibility, the reconnection Jacobian and
// normalization belong to resampling; both the original and decoupled shade paths use this same f(z).
float3 EvaluateGiIndirectContribution(SceneContext scene,
                                      ShadingPoint shadingPoint,
                                      GiSample sample,
                                      inout RayQueryContext queries)
{
    if (sample.TriangleIndex == SCENE_INVALID_INDEX)
        return float3(0);
    SurfaceHit samplePoint = GetSurfaceHit(scene, sample.TriangleIndex, sample.Barycentric);
    const float3 offsetWorld = samplePoint.PositionWorld - shadingPoint.Hit.PositionWorld;
    const float distanceSquared = dot(offsetWorld, offsetWorld);
    if (samplePoint.IsValid == false || distanceSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    const float3 directionWorld = offsetWorld * rsqrt(distanceSquared);
    const float cosSurface = dot(shadingPoint.ShadingNormalWorld, directionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(shadingPoint.Material,
                         shadingPoint.ViewDirectionWorld,
                         directionWorld,
                         bsdfValue,
                         bsdfDensity);
    return cosSurface > 0 ? bsdfValue * sample.RadianceOut * cosSurface : float3(0);
}
