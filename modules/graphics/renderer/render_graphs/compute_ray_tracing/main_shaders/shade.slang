#include "hit_record.slang"

// Pass 2 of 2 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting.
// Radiance = surface emission + one light sample: SampleLight chooses a light uniformly and proposes a point
// or direction on it, TestVisibility traces the shadow ray
// and EvaluateStandardBsdf weights the result. A miss returns the environment. There is no
// indirect bounce. One visibility query. The surface comes from the record (GetSurfaceHit), not
// from tracing the camera ray again; when it is smooth glass, FollowSpecularChain continues
// through it (up to SCENE_SPECULAR_CHAIN_EVENTS surface queries) and the first surface it can
// shade is lit instead.

/// One-sample direct lighting at a reconstructed receiver, with the light proposal's own PDF.
float3 SampleDirectLighting(SceneContext scene,
                            RayLaunch launch,
                            SurfaceHit receiver,
                            float3 viewDirectionWorld,
                            inout RayQueryContext queries)
{
    MaterialSample material = EvaluateMaterial(scene, receiver);
    float normalLengthSquared = dot(material.ShadingNormalWorld, material.ShadingNormalWorld);
    if (material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    SceneLightSample sample = SampleLight(scene,
                                         receiver,
                                         launch,
                                         SHADE_RANDOM_DIMENSION_FIRST,
                                         SHADE_INCIDENT_DIMENSION_FIRST,
                                         LightSelectionPolicy::Uniform,
                                         queries);
    IncidentLightSample light = sample.Incident;
    float probability = sample.Selection.SelectionPmf * light.ConditionalProbability;
    if (light.HasSample == false || probability <= 0 || any(light.IncidentCoefficient > 0) == false)
        return float3(0);
    // Zero for glass, whose delta lobes no light sample reaches.
    float cosSurface =
        ComputeStandardBsdfCosine(material, viewDirectionWorld, light.DirectionWorld) * rsqrt(normalLengthSquared);
    float3 bsdfValue;
    float bsdfPdfSolidAngle;
    EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, bsdfValue, bsdfPdfSolidAngle);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false)
        return float3(0);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (TestVisibility(scene, queries, receiver, light) != SceneQuery::Miss)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface / probability);
}

float3 ShadeRecordedHit(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries,
                       HasHitRecordCapacity(launch) == false ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY
                                                             : LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    // The primary pass traced this exact ray: same launch, camera and stored jitter.
    Ray cameraRay = MakeCameraRay(launch, record.JitterPixel);
    if (record.IsHit == false)
        return EvaluateEnvironment(scene, cameraRay.DirectionWorld);
    SurfaceHit hit = GetSurfaceHit(scene, record.TriangleIndex, record.Barycentric);
    if (hit.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    // GetSurfaceHit issues no ray; restore what TraceClosest reported, as LoadVisibilitySurface does.
    hit.IncomingRay = cameraRay;
    hit.DistanceWorld = record.DistanceWorld;
    hit.IsFrontFace = record.IsFrontFace;
    SpecularChain chain = FollowSpecularChain(scene, launch, queries, hit, SPECULAR_CHAIN_RANDOM_DIMENSION_FIRST);
    float3 radiance = chain.Radiance;
    if (chain.IsSurface == true)
        radiance +=
            chain.Throughput * (EvaluateEmission(scene, chain.Hit, chain.ViewDirectionWorld) +
                                SampleDirectLighting(scene, launch, chain.Hit, chain.ViewDirectionWorld, queries));
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}
