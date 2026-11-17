// RT2 hybrid reflections: the raster visibility buffer supplies each pixel's first surface. Its radiance is the
// surface emission, direct light from the scene's directional, point and spot lights with one shadow ray each, and one
// reflection ray drawn from the GGX specular lobe's visible normals, so the reflection follows the surface roughness.
// The reflection ray's hit is shaded with its emission and the same ray-shadowed analytic light; a miss returns the
// environment. Each contribution is assigned once:
// - analytic lights reach the first surface only through its shadow rays, with the full standard BSDF;
// - emission, the environment and analytic light at the hit reach it only through the specular lobe.
// The diffuse lobe traces no indirect ray, so mesh emitters and the environment do not light it here, and neither
// do they light the reflected surface. One sample per pixel per frame is the raw signal; the graph's Accumulate
// node is its reconstruction.
// Thanks to Eric Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018, pp. 1-13, for the
// visible-normal proposal whose weight f cos / pdf stays at most the Fresnel term times G2 / G1.
// https://jcgt.org/published/0007/04/01/

/// Shadow rays each shaded surface may trace. The first surface and the reflection hit share the technique's
/// scene_interface.visibility_queries equally.
static const uint MAXIMUM_SHADOW_RAYS_PER_SURFACE = 8;
/// Analytic lights need no random dimensions; the index only satisfies the addressed API.
static const uint SHADE_INCIDENT_DIMENSION_FIRST = 0;
/// Reflection rays are not clipped to the camera range: a reflected surface may lie behind the camera.
static const float REFLECTION_MAXIMUM_DISTANCE_WORLD = 3.402823466e38f;

/// True for the directional, point and spot kinds, which the catalog lists before mesh emitters.
bool IsAnalyticLight(LightInfo light)
{
    return light.Kind == SceneLightKind::Directional || light.Kind == SceneLightKind::Point ||
           light.Kind == SceneLightKind::Spot;
}

/// Direct light from the analytic lights in catalog order. The first MAXIMUM_SHADOW_RAYS_PER_SURFACE lights that can
/// reach the surface are ray shadowed; any further contributing light is added unshadowed. Returns false after a
/// query failure.
bool TryShadeAnalyticLights(SceneContext scene,
                            RayLaunch launch,
                            SurfaceHit hit,
                            MaterialSample material,
                            float3 viewDirectionWorld,
                            inout RayQueryContext inoutQueries,
                            out float3 outRadiance)
{
    outRadiance = float3(0);
    uint shadowRays = 0;
    const uint lightCount = GetLightCount(scene);
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        if (IsAnalyticLight(GetLightInfo(scene, lightIndex)) == false)
            break;
        const IncidentLightSample light =
            SampleIncidentLight(scene, lightIndex, hit, launch, SHADE_INCIDENT_DIMENSION_FIRST, inoutQueries);
        if (light.HasSample == false || any(light.IncidentCoefficient > 0) == false)
            continue;
        // Zero for glass, whose delta lobes no analytic light reaches.
        const float cosSurface = ComputeStandardBsdfCosine(material, viewDirectionWorld, light.DirectionWorld);
        float3 bsdfValue;
        float bsdfPdfSolidAngle;
        EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, bsdfValue, bsdfPdfSolidAngle);
        if (cosSurface <= 0 || any(bsdfValue > 0) == false)
            continue;
        if (shadowRays < MAXIMUM_SHADOW_RAYS_PER_SURFACE)
        {
            shadowRays += 1;
            const SceneQuery visibility = TestVisibility(scene, inoutQueries, hit, light);
            if (visibility == SceneQuery::Failure)
                return false;
            if (visibility != SceneQuery::Miss)
                continue;
        }
        outRadiance += bsdfValue * light.IncidentCoefficient * cosSurface;
    }
    return true;
}

/// The surface's material with a unit shading normal, so lobe sampling, evaluation and cosines agree. Returns false
/// and latches the failure for invalid scene data.
bool TryEvaluateUnitMaterial(SceneContext scene,
                             SurfaceHit hit,
                             inout RayQueryContext inoutQueries,
                             out MaterialSample outMaterial)
{
    outMaterial = EvaluateMaterial(scene, hit);
    const float normalLengthSquared = dot(outMaterial.ShadingNormalWorld, outMaterial.ShadingNormalWorld);
    if (outMaterial.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outMaterial.ShadingNormalWorld *= rsqrt(normalLengthSquared);
    return true;
}

/// Radiance arriving along the reflection ray: the environment for a miss, otherwise the hit's emission and its
/// ray-shadowed analytic light. Returns false after a query failure.
bool TryShadeReflection(SceneContext scene,
                        RayLaunch launch,
                        SurfaceHit receiver,
                        float3 directionWorld,
                        inout RayQueryContext inoutQueries,
                        out float3 outRadiance)
{
    outRadiance = float3(0);
    const Ray ray = SpawnSurfaceRay(receiver, directionWorld, REFLECTION_MAXIMUM_DISTANCE_WORLD);
    SurfaceHit hit;
    const SceneQuery status = TraceClosest(scene, inoutQueries, ray, hit);
    if (status == SceneQuery::Miss)
    {
        outRadiance = EvaluateEnvironment(scene, directionWorld);
        return true;
    }
    if (status != SceneQuery::Surface)
        return false;
    MaterialSample material;
    if (TryEvaluateUnitMaterial(scene, hit, inoutQueries, material) == false)
        return false;
    const float3 viewDirectionWorld = -directionWorld;
    float3 direct;
    if (TryShadeAnalyticLights(scene, launch, hit, material, viewDirectionWorld, inoutQueries, direct) == false)
        return false;
    outRadiance = EvaluateEmission(scene, hit, viewDirectionWorld) + direct;
    return true;
}

float3 ShadeReflections(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    const SceneQuery status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    if (status == SceneQuery::Miss)
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    if (status != SceneQuery::Surface)
        return float3(0);

    MaterialSample material;
    if (TryEvaluateUnitMaterial(scene, hit, inoutQueries, material) == false)
        return float3(0);
    const float3 viewDirectionWorld = -launch.CameraRay.DirectionWorld;
    float3 direct;
    if (TryShadeAnalyticLights(scene, launch, hit, material, viewDirectionWorld, inoutQueries, direct) == false)
        return float3(0);
    float3 radiance = EvaluateEmission(scene, hit, viewDirectionWorld) + direct;

    // Both draws are taken for every surface, so a pixel's stream addresses do not depend on its material.
    // Separate statements fix the order of the two draws.
    const float radiusSample = SampleSceneRandom(inoutQueries);
    const float angleSample = SampleSceneRandom(inoutQueries);
    const float3 directionWorld =
        SampleStandardSpecularLobe(material, viewDirectionWorld, float2(radiusSample, angleSample));
    // A zero direction is the ordinary rejection: no specular lobe, or a reflection below the surface.
    if (inoutQueries.HasFailed == false && any(directionWorld != 0) == true)
    {
        float3 lobeValue;
        float lobePdfSolidAngle;
        EvaluateStandardSpecularLobe(material, viewDirectionWorld, directionWorld, lobeValue, lobePdfSolidAngle);
        const float cosSurface = dot(material.ShadingNormalWorld, directionWorld);
        if (lobePdfSolidAngle > 0 && cosSurface > 0 && any(lobeValue > 0) == true)
        {
            float3 reflected;
            if (TryShadeReflection(scene, launch, hit, directionWorld, inoutQueries, reflected) == false)
                return float3(0);
            radiance += reflected * lobeValue * (cosSurface / lobePdfSolidAngle);
        }
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}
