#include "hit_record.slang"
#include "multiple_importance.slang"

// Passes 2 to 4 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting
// with the two strategies that Veach and Guibas combine (multiple_importance.slang):
// - light: SelectLight chooses a light uniformly, SampleIncidentLight proposes a point or direction
//   on it and TestVisibility traces the shadow ray;
// - BSDF: SampleStandardBsdf proposes a direction and TraceClosest finds what it reaches: emission
//   from the surface it hits, or the environment on a miss.
// Each pass returns surface emission plus one estimate:
// - ShadeMultipleImportance: both strategies, MIS-weighted with each other's density for the same
//   direction (EvaluateLightPdfSolidAngle, EvaluateEnvironmentPdfSolidAngle). The graph's color output.
// - ShadeLightSampling: the light strategy alone, with weight one (lightSampling output).
// - ShadeBsdfSampling: the BSDF strategy alone, with weight one (bsdfSampling output). It never reaches
//   a directional, point or spot light, so those lights are absent from this image.
// The three passes read the same random dimensions, so each MIS image combines exactly the light
// and BSDF samples shown in the other two outputs. Both densities are solid-angle densities. A delta
// light sample keeps weight one under MIS, because BSDF sampling never proposes its direction.
// A primary miss returns the environment. There is no indirect bounce. At most one surface and one
// visibility query; the camera ray is not traced again, because the surface comes from the record.

static const float SHADE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
    float3 ViewDirectionWorld;
};

/// Rebuild this pixel's recorded surface. Returns false when the pixel needs no strategy: a primary
/// miss (outRadiance is the environment) or a failure (queries.HasFailed is set, outRadiance is 0).
bool TryLoadShadingPoint(SceneContext scene,
                         RayLaunch launch,
                         inout RayQueryContext queries,
                         out ShadingPoint outPoint,
                         out float3 outRadiance)
{
    outPoint = (ShadingPoint)0;
    outRadiance = float3(0);
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries, HasHitRecordCapacity(launch) == false ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY : LUDUS_SCENE_FAILURE_STORED_RECORD);
        return false;
    }
    // The primary pass traced this exact ray: same launch, camera and stored jitter.
    Ray cameraRay = MakeCameraRay(launch, record.JitterPixel);
    if (record.IsHit == false)
    {
        outRadiance = EvaluateEnvironment(scene, cameraRay.DirectionWorld);
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
    outPoint.ViewDirectionWorld = -cameraRay.DirectionWorld;
    outPoint.Material = EvaluateMaterial(scene, outPoint.Hit);
    float normalLengthSquared = dot(outPoint.Material.ShadingNormalWorld, outPoint.Material.ShadingNormalWorld);
    if (outPoint.Material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outPoint.ShadingNormalWorld = outPoint.Material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    outRadiance = EvaluateEmission(scene, outPoint.Hit, outPoint.ViewDirectionWorld);
    return true;
}

/// Light strategy: one light proposal and its shadow ray. With isMisWeighted it is weighted against
/// BSDF sampling; otherwise it is the plain light-sampling estimator (weight one).
float3 SampleLightStrategy(SceneContext scene,
                           RayLaunch launch,
                           ShadingPoint shadingPoint,
                           bool isMisWeighted,
                           inout RayQueryContext queries)
{
    SelectedLight selected = SelectLight(scene, launch, SHADE_LIGHT_SELECTION_DIMENSION);
    if (selected.HasSample == false)
        return float3(0);
    float3 shapeSample = float3(SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST),
                                SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 1),
                                SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 2));
    LightSample light = SampleIncidentLight(scene, selected, shadingPoint.Hit, shapeSample);
    float lightDensity = light.SelectionPmf * light.ConditionalPdf;
    if (light.HasSample == false || lightDensity <= 0 || any(light.IncidentCoefficient > 0) == false)
        return float3(0);
    float cosSurface = dot(shadingPoint.ShadingNormalWorld, light.DirectionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(shadingPoint.Material,
                         shadingPoint.ViewDirectionWorld,
                         light.DirectionWorld,
                         bsdfValue,
                         bsdfDensity);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false)
        return float3(0);
    float weight = 1;
    if (isMisWeighted == true && light.IsDiscrete == false)
        weight = ComputeMisWeight(lightDensity, bsdfDensity);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (weight <= 0 || TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface * weight / lightDensity);
}

/// BSDF strategy: one BSDF-sampled ray and the emitter or environment it reaches. With isMisWeighted
/// it is weighted against light sampling; otherwise it is the plain BSDF-sampling estimator.
float3 SampleBsdfStrategy(SceneContext scene,
                          RayLaunch launch,
                          ShadingPoint shadingPoint,
                          bool isMisWeighted,
                          inout RayQueryContext queries)
{
    float3 directionSample = float3(SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST),
                                    SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST + 1),
                                    SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST + 2));
    float3 sampledDirectionWorld =
        SampleStandardBsdf(shadingPoint.Material, shadingPoint.ViewDirectionWorld, directionSample);
    float sampledLengthSquared = dot(sampledDirectionWorld, sampledDirectionWorld);
    // A zero direction is the interface's ordinary rejection.
    if (all(isfinite(sampledDirectionWorld)) == false || sampledLengthSquared <= 0)
        return float3(0);
    float3 directionWorld = sampledDirectionWorld * rsqrt(sampledLengthSquared);
    float cosSurface = dot(shadingPoint.ShadingNormalWorld, directionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(shadingPoint.Material,
                         shadingPoint.ViewDirectionWorld,
                         directionWorld,
                         bsdfValue,
                         bsdfDensity);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false || isfinite(bsdfDensity) == false || bsdfDensity <= 0)
        return float3(0);
    SurfaceHit endpoint;
    uint status = TraceClosest(scene,
                               queries,
                               SpawnSurfaceRay(shadingPoint.Hit, directionWorld, SHADE_MAXIMUM_TRACE_DISTANCE_WORLD),
                               endpoint);
    float3 incidentRadiance = float3(0);
    float lightDensity = 0;
    if (status == SCENE_QUERY_MISS)
    {
        incidentRadiance = EvaluateEnvironment(scene, directionWorld);
        if (isMisWeighted == true)
            lightDensity = EvaluateEnvironmentPdfSolidAngle(scene, directionWorld);
    }
    else if (status == SCENE_QUERY_SURFACE)
    {
        incidentRadiance = EvaluateEmission(scene, endpoint, -directionWorld);
        // Measured from the receiver itself, as SampleIncidentLight measures its proposals.
        if (isMisWeighted == true)
            lightDensity = EvaluateLightPdfSolidAngle(scene, shadingPoint.Hit.PositionWorld, endpoint);
    }
    // A failed query has latched queries.HasFailed; zero radiance adds nothing.
    if (any(incidentRadiance > 0) == false)
        return float3(0);
    float weight = isMisWeighted == true ? ComputeMisWeight(bsdfDensity, lightDensity) : 1;
    return bsdfValue * incidentRadiance * (cosSurface * weight / bsdfDensity);
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

/// Emission plus both strategies, MIS-weighted. Complementary MIS estimates are summed, not averaged.
float3 ShadeMultipleImportance(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    radiance += SampleLightStrategy(scene, launch, shadingPoint, true, queries);
    radiance += SampleBsdfStrategy(scene, launch, shadingPoint, true, queries);
    return FinishRadiance(radiance, queries);
}

/// Emission plus the light strategy alone.
float3 ShadeLightSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    radiance += SampleLightStrategy(scene, launch, shadingPoint, false, queries);
    return FinishRadiance(radiance, queries);
}

/// Emission plus the BSDF strategy alone.
float3 ShadeBsdfSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    radiance += SampleBsdfStrategy(scene, launch, shadingPoint, false, queries);
    return FinishRadiance(radiance, queries);
}
