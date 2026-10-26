#include "hit_record.slang"
#include "multiple_importance.slang"

// Pass 2 of 2 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting.
// Radiance = surface emission + one sample from each of two strategies, combined with multiple
// importance sampling (multiple_importance.slang):
// - light: SelectLight chooses a light uniformly, SampleIncidentLight proposes a point or direction
//   on it and TestVisibility traces the shadow ray;
// - BSDF: SampleStandardBsdf proposes a direction and TraceClosest finds what it reaches. Emission
//   from the surface it hits, or the environment on a miss, is weighted against the light
//   strategy's density for the same direction (EvaluateLightPdfSolidAngle,
//   EvaluateEnvironmentPdfSolidAngle).
// Both densities are solid-angle densities. A directional, point or spot light is a delta light that
// BSDF sampling never reaches, so its light sample keeps weight one. A primary miss returns the
// environment. There is no indirect bounce. One surface and one visibility query; the camera ray is
// not traced again, because the surface comes from the record (GetSurfaceHit).

static const float SHADE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;

/// Light strategy: one light proposal and its shadow ray, MIS-weighted against BSDF sampling.
float3 SampleLightStrategy(SceneContext scene,
                           RayLaunch launch,
                           SurfaceHit receiver,
                           MaterialSample material,
                           float3 shadingNormalWorld,
                           float3 viewDirectionWorld,
                           inout RayQueryContext queries)
{
    SelectedLight selected = SelectLight(scene, launch, SHADE_LIGHT_SELECTION_DIMENSION);
    if (selected.HasSample == false)
        return float3(0);
    float3 shapeSample = float3(SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST),
                                SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 1),
                                SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 2));
    LightSample light = SampleIncidentLight(scene, selected, receiver, shapeSample);
    float lightDensity = light.SelectionPmf * light.ConditionalPdf;
    if (light.HasSample == false || lightDensity <= 0 || any(light.IncidentCoefficient > 0) == false)
        return float3(0);
    float cosSurface = dot(shadingNormalWorld, light.DirectionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, bsdfValue, bsdfDensity);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false)
        return float3(0);
    float weight = light.IsDiscrete == true ? 1 : ComputeMisWeight(lightDensity, bsdfDensity);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (weight <= 0 || TestVisibility(scene, queries, receiver, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface * weight / lightDensity);
}

/// BSDF strategy: one BSDF-sampled ray; the emitter or environment it reaches is MIS-weighted.
float3 SampleBsdfStrategy(SceneContext scene,
                          RayLaunch launch,
                          SurfaceHit receiver,
                          MaterialSample material,
                          float3 shadingNormalWorld,
                          float3 viewDirectionWorld,
                          inout RayQueryContext queries)
{
    float3 directionSample = float3(SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST),
                                    SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST + 1),
                                    SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST + 2));
    float3 sampledDirectionWorld = SampleStandardBsdf(material, viewDirectionWorld, directionSample);
    float sampledLengthSquared = dot(sampledDirectionWorld, sampledDirectionWorld);
    // A zero direction is the interface's ordinary rejection.
    if (all(isfinite(sampledDirectionWorld)) == false || sampledLengthSquared <= 0)
        return float3(0);
    float3 directionWorld = sampledDirectionWorld * rsqrt(sampledLengthSquared);
    float cosSurface = dot(shadingNormalWorld, directionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(material, viewDirectionWorld, directionWorld, bsdfValue, bsdfDensity);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false || isfinite(bsdfDensity) == false || bsdfDensity <= 0)
        return float3(0);
    SurfaceHit endpoint;
    uint status = TraceClosest(scene,
                               queries,
                               SpawnSurfaceRay(receiver, directionWorld, SHADE_MAXIMUM_TRACE_DISTANCE_WORLD),
                               endpoint);
    float3 incidentRadiance = float3(0);
    float lightDensity = 0;
    if (status == SCENE_QUERY_MISS)
    {
        incidentRadiance = EvaluateEnvironment(scene, directionWorld);
        lightDensity = EvaluateEnvironmentPdfSolidAngle(scene, directionWorld);
    }
    else if (status == SCENE_QUERY_SURFACE)
    {
        // Measured from the receiver itself, as SampleIncidentLight measures its proposals.
        incidentRadiance = EvaluateEmission(scene, endpoint, -directionWorld);
        lightDensity = EvaluateLightPdfSolidAngle(scene, receiver.PositionWorld, endpoint);
    }
    // A failed query has latched queries.HasFailed; zero radiance adds nothing.
    if (any(incidentRadiance > 0) == false)
        return float3(0);
    float weight = ComputeMisWeight(bsdfDensity, lightDensity);
    return bsdfValue * incidentRadiance * (cosSurface * weight / bsdfDensity);
}

float3 ShadeRecordedHit(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    // The primary pass traced this exact ray: same launch, camera and stored jitter.
    Ray cameraRay = MakeCameraRay(launch, record.JitterPixel);
    if (record.IsHit == false)
        return EvaluateEnvironment(scene, cameraRay.DirectionWorld);
    SurfaceHit hit = GetSurfaceHit(scene, record.TriangleIndex, record.Barycentric);
    if (hit.IsValid == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    // GetSurfaceHit issues no ray; restore what TraceClosest reported, as LoadVisibilitySurface does.
    hit.IncomingRay = cameraRay;
    hit.DistanceWorld = record.DistanceWorld;
    hit.IsFrontFace = record.IsFrontFace;
    float3 viewDirectionWorld = -cameraRay.DirectionWorld;
    MaterialSample material = EvaluateMaterial(scene, hit);
    float normalLengthSquared = dot(material.ShadingNormalWorld, material.ShadingNormalWorld);
    if (material.IsValid == false || normalLengthSquared <= 0)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    float3 shadingNormalWorld = material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    float3 radiance = EvaluateEmission(scene, hit, viewDirectionWorld);
    // Complementary MIS estimates are summed, not averaged; each runs whatever the other found.
    radiance += SampleLightStrategy(scene, launch, hit, material, shadingNormalWorld, viewDirectionWorld, queries);
    radiance += SampleBsdfStrategy(scene, launch, hit, material, shadingNormalWorld, viewDirectionWorld, queries);
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    return radiance;
}
