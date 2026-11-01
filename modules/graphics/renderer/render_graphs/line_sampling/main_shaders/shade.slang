#include "hit_record.slang"
#include "line_sampling.slang"

// Passes 2 and 3 (scene compute): rebuild the recorded surface and estimate first-hit direct
// lighting with one line sample in direction U (line_sampling.slang):
// - ShadeLineSampling: emission plus the line estimate. The graph's color output.
// - ShadeVisibleFraction: the visible fraction L_visible / c of the same chord, as gray, or zero for a
//   non-mesh light (visibleFraction output). Accumulated, it is the mean visible fraction, not
//   radiance: 1 where the selected emitters are unoccluded and 0 in umbra.
// Both passes read the same random dimensions, so they build the same chord for every sample.
// A primary miss returns the environment from ShadeLineSampling and zero from ShadeVisibleFraction.
// There is no indirect bounce. At most one line and one visibility query; the camera ray is not
// traced again, because the surface comes from the record.

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
    float3 ViewDirectionWorld;
};

/// Rebuild this pixel's recorded surface. Returns false when the pixel needs no strategy: a primary
/// miss (outIsMiss is set) or a failure (queries.HasFailed is set).
bool TryLoadShadingPoint(SceneContext scene,
                         RayLaunch launch,
                         inout RayQueryContext queries,
                         out ShadingPoint outPoint,
                         out bool outIsMiss)
{
    outPoint = (ShadingPoint)0;
    outIsMiss = false;
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries, HasHitRecordCapacity(launch) == false ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY : LUDUS_SCENE_FAILURE_STORED_RECORD);
        return false;
    }
    // The primary pass traced this exact ray: same launch, camera and stored jitter.
    Ray cameraRay = MakeCameraRay(launch, record.JitterPixel);
    outPoint.ViewDirectionWorld = -cameraRay.DirectionWorld;
    if (record.IsHit == false)
    {
        outIsMiss = true;
        return false;
    }
    outPoint.Hit = GetSurfaceHit(scene, record.TriangleIndex, record.Barycentric);
    if (outPoint.Hit.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    // GetSurfaceHit issues no ray; restore what TraceClosest reported, as LoadVisibilitySurface does.
    outPoint.Hit.IncomingRay = cameraRay;
    outPoint.Hit.DistanceWorld = record.DistanceWorld;
    outPoint.Hit.IsFrontFace = record.IsFrontFace;
    outPoint.Material = EvaluateMaterial(scene, outPoint.Hit);
    float normalLengthSquared = dot(outPoint.Material.ShadingNormalWorld, outPoint.Material.ShadingNormalWorld);
    if (outPoint.Material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outPoint.ShadingNormalWorld = outPoint.Material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    return true;
}

/// Reject a nonfinite or negative estimate as a shader failure.
float3 FinishRadiance(float3 radiance, inout RayQueryContext queries)
{
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}

/// Emission plus one line sample in direction U.
float3 ShadeLineSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    bool isMiss;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, isMiss) == false)
        return isMiss == true ? EvaluateEnvironment(scene, -shadingPoint.ViewDirectionWorld) : float3(0);
    float3 radiance = EvaluateEmission(scene, shadingPoint.Hit, shadingPoint.ViewDirectionWorld);
    LineProposal proposal;
    if (ProposeLineU(scene,
                     launch,
                     shadingPoint.Hit,
                     shadingPoint.ShadingNormalWorld,
                     true,
                     SHADE_LINE_U_DIMENSION_FIRST,
                     SHADE_LINE_U_POSITION_DIMENSION,
                     queries,
                     proposal) == true)
        radiance += EstimateLineProposal(scene,
                                         launch,
                                         shadingPoint.Hit,
                                         shadingPoint.Material,
                                         shadingPoint.ShadingNormalWorld,
                                         shadingPoint.ViewDirectionWorld,
                                         proposal,
                                         queries);
    return FinishRadiance(radiance, queries);
}

/// The visible fraction L_visible / c of the U chord that ShadeLineSampling builds, as gray.
float3 ShadeVisibleFraction(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    bool isMiss;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, isMiss) == false)
        return float3(0);
    LineProposal proposal;
    ProposeLineU(scene,
                 launch,
                 shadingPoint.Hit,
                 shadingPoint.ShadingNormalWorld,
                 false,
                 SHADE_LINE_U_DIMENSION_FIRST,
                 SHADE_LINE_U_POSITION_DIMENSION,
                 queries,
                 proposal);
    return FinishRadiance(float3(GetVisibleChordFraction(proposal)), queries);
}
