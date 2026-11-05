#include "hit_record.slang"
#include "multiple_importance.slang"

// Passes 2 to 4 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting
// with the two strategies that Veach and Guibas combine (multiple_importance.slang):
// - light: The configured selector chooses one catalog light, SampleIncidentLight proposes a point or direction
//   on it and TestVisibility traces the shadow ray;
// - BSDF: SampleStandardBsdf proposes a direction and TraceClosest finds what it reaches: emission
//   from the surface it hits, or the environment on a miss.
// Each pass returns surface emission plus one estimate:
// - ShadeMultipleImportance: both strategies, MIS-weighted with each other's density for the same
//   direction (source PMF times the conditional incident density). The graph's color output.
// - ShadeLightSampling: the light strategy alone, with weight one (lightSampling output).
// - ShadeBsdfSampling: the BSDF strategy alone, with weight one (bsdfSampling output). It never reaches
//   a directional, point or spot light, so those lights are absent from this image.
// The three passes read the same random dimensions, so each MIS image combines exactly the light
// and BSDF samples shown in the other two outputs. Both densities are solid-angle densities. A delta
// light sample keeps weight one under MIS, because BSDF sampling never proposes its direction.
// A primary miss returns the environment. There is no indirect bounce. At most one surface and one
// visibility query; the camera ray is not traced again, because the surface comes from the record.
// A recorded smooth-glass surface is followed with FollowSpecularChain (up to SCENE_SPECULAR_CHAIN_EVENTS
// more surface queries) to the first surface the strategies can shade.

static const float SHADE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
    float3 ViewDirectionWorld;
    float3 Throughput; ///< Smooth-glass chain weight from the camera to Hit.
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
        FailSceneQuery(queries,
                       HasHitRecordCapacity(launch) == false ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY
                                                             : LUDUS_SCENE_FAILURE_STORED_RECORD);
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
    SpecularChain chain =
        FollowSpecularChain(scene, launch, queries, outPoint.Hit, SPECULAR_CHAIN_RANDOM_DIMENSION_FIRST);
    outRadiance = chain.Radiance;
    if (chain.IsSurface == false)
        return false;
    outPoint.Hit = chain.Hit;
    outPoint.Throughput = chain.Throughput;
    outPoint.ViewDirectionWorld = chain.ViewDirectionWorld;
    outPoint.Material = EvaluateMaterial(scene, outPoint.Hit);
    float normalLengthSquared = dot(outPoint.Material.ShadingNormalWorld, outPoint.Material.ShadingNormalWorld);
    if (outPoint.Material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outPoint.ShadingNormalWorld = outPoint.Material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    outRadiance += outPoint.Throughput * EvaluateEmission(scene, outPoint.Hit, outPoint.ViewDirectionWorld);
    return true;
}

static const LightSelectionPolicy VEACH_LIGHT_SELECTION_POLICY =
    IS_MESH_POWER_SELECTION_ENABLED ? LightSelectionPolicy::MeshPower : LightSelectionPolicy::Uniform;

/// Light strategy: one light proposal and its shadow ray. With isMisWeighted it is weighted against
/// BSDF sampling; otherwise it is the plain light-sampling estimator (weight one).
float3 SampleLightStrategy(SceneContext scene,
                           RayLaunch launch,
                           ShadingPoint shadingPoint,
                           bool isMisWeighted,
                           inout RayQueryContext queries)
{
    SceneLightSample sample = SampleLight(scene,
                                          shadingPoint.Hit,
                                          launch,
                                          SHADE_LIGHT_SELECTION_DIMENSION,
                                          SHADE_LIGHT_SHAPE_DIMENSION_FIRST,
                                          VEACH_LIGHT_SELECTION_POLICY,
                                          queries);
    IncidentLightSample light = sample.Incident;
    float lightDensity = sample.Selection.SelectionPmf * light.ConditionalProbability;
    if (light.HasSample == false || lightDensity <= 0 || any(light.IncidentCoefficient > 0) == false)
        return float3(0);
    float cosSurface =
        ComputeStandardBsdfCosine(shadingPoint.Material, shadingPoint.ViewDirectionWorld, light.DirectionWorld);
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
    if (isMisWeighted == true && light.Measure == IncidentLightMeasure::SolidAngle)
        weight = ComputeMisWeight(lightDensity, bsdfDensity);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (weight <= 0 || TestVisibility(scene, queries, shadingPoint.Hit, light) != SceneQuery::Miss)
        return float3(0);
    return shadingPoint.Throughput * bsdfValue * light.IncidentCoefficient * (cosSurface * weight / lightDensity);
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
    float cosSurface =
        ComputeStandardBsdfCosine(shadingPoint.Material, shadingPoint.ViewDirectionWorld, directionWorld);
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
    SceneQuery status =
        TraceClosest(scene,
                     queries,
                     SpawnSurfaceRay(shadingPoint.Hit, directionWorld, SHADE_MAXIMUM_TRACE_DISTANCE_WORLD),
                     endpoint);
    RayLightEvaluation light = EvaluateLightAlongRay(scene,
                                                     shadingPoint.Hit.PositionWorld,
                                                     directionWorld,
                                                     status,
                                                     endpoint,
                                                     VEACH_LIGHT_SELECTION_POLICY);
    float3 incidentRadiance = light.Radiance;
    float lightDensity = light.SelectionPmf * light.ConditionalPdfSolidAngle;
    // A failed query has latched queries.HasFailed; zero radiance adds nothing.
    if (any(incidentRadiance > 0) == false)
        return float3(0);
    float weight = isMisWeighted == true ? ComputeMisWeight(bsdfDensity, lightDensity) : 1;
    return shadingPoint.Throughput * bsdfValue * incidentRadiance * (cosSurface * weight / bsdfDensity);
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
