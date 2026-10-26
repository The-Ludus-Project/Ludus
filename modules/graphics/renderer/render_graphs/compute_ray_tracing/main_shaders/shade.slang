#include "hit_record.slang"

// Pass 2 of 2 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting.
// Radiance = surface emission + one light sample: SelectLight chooses a light uniformly,
// SampleIncidentLight proposes a point or direction on it, TestVisibility traces the shadow ray
// and EvaluateStandardBsdf weights the result. A miss returns the environment. There is no
// indirect bounce. One visibility query; no surface query, because the surface comes from the
// record (GetSurfaceHit), not from tracing the camera ray again.

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
        queries.HasFailed = true;
        return float3(0);
    }
    SelectedLight selected = SelectLight(scene, launch, SHADE_RANDOM_DIMENSION_FIRST);
    if (selected.HasSample == false)
        return float3(0);
    float3 shapeSample = float3(SampleSceneRandom(launch, SHADE_RANDOM_DIMENSION_FIRST + 1),
                                SampleSceneRandom(launch, SHADE_RANDOM_DIMENSION_FIRST + 2),
                                SampleSceneRandom(launch, SHADE_RANDOM_DIMENSION_FIRST + 3));
    LightSample light = SampleIncidentLight(scene, selected, receiver, shapeSample);
    float probability = light.SelectionPmf * light.ConditionalPdf;
    if (light.HasSample == false || probability <= 0 || any(light.IncidentCoefficient > 0) == false)
        return float3(0);
    float cosSurface = dot(material.ShadingNormalWorld * rsqrt(normalLengthSquared), light.DirectionWorld);
    float3 bsdfValue;
    float bsdfPdfSolidAngle;
    EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, bsdfValue, bsdfPdfSolidAngle);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false)
        return float3(0);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (TestVisibility(scene, queries, receiver, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface / probability);
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
    float3 radiance = EvaluateEmission(scene, hit, viewDirectionWorld);
    radiance += SampleDirectLighting(scene, launch, hit, viewDirectionWorld, queries);
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    return radiance;
}
