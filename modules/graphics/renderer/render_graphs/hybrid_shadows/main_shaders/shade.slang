// RT2 hybrid shadows: the raster visibility buffer supplies each pixel's first surface, and one
// shadow ray per contributing analytic light replaces a shadow-map lookup. Radiance is the surface
// emission plus direct light from the scene's directional, point and spot lights, each weighted by
// the standard BSDF, the receiver cosine and its ray visibility. Lights are visited in catalog
// order. The first MAXIMUM_SHADOW_RAYS lights that can reach the receiver are
// ray shadowed; any further contributing light is added unshadowed, which the authoring guide
// declares. Each light is counted once. Mesh emitters and the environment light no surface here,
// and a miss shows the environment. Hard shadows from punctual lights are deterministic, so the
// raw signal needs no reconstruction filter.
// Thanks to Franklin C. Crow, "Shadow Algorithms for Computer Graphics", SIGGRAPH 1977, pp. 242-248,
// for separating the light-space shadow boundary from viewer-space hidden surface removal. His
// Fig. 4 clips a shadow volume to the field of view; rays from raster first hits instead reach
// blockers outside it.
// https://www.cs.rpi.edu/~cutler/classes/advancedgraphics/S10/papers/crow77_shadows.pdf

/// Shadow rays one pixel may trace: the technique's scene_interface.visibility_queries.
static const uint MAXIMUM_SHADOW_RAYS = 16;
/// Analytic lights need no random dimensions; the index only satisfies the addressed API.
static const uint SHADE_INCIDENT_DIMENSION_FIRST = 0;

/// True for the directional, point and spot kinds, which the catalog lists before mesh emitters.
bool IsAnalyticLight(LightInfo light)
{
    return light.Kind == SceneLightKind::Directional || light.Kind == SceneLightKind::Point ||
           light.Kind == SceneLightKind::Spot;
}

float3 ShadeShadowedDirect(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    const SceneQuery status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    if (status == SceneQuery::Miss)
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    if (status != SceneQuery::Surface)
        return float3(0);

    const MaterialSample material = EvaluateMaterial(scene, hit);
    const float normalLengthSquared = dot(material.ShadingNormalWorld, material.ShadingNormalWorld);
    if (material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    const float3 viewDirectionWorld = -launch.CameraRay.DirectionWorld;
    float3 radiance = EvaluateEmission(scene, hit, viewDirectionWorld);
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
        const float cosSurface =
            ComputeStandardBsdfCosine(material, viewDirectionWorld, light.DirectionWorld) * rsqrt(normalLengthSquared);
        float3 bsdfValue;
        float bsdfPdfSolidAngle;
        EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, bsdfValue, bsdfPdfSolidAngle);
        if (cosSurface <= 0 || any(bsdfValue > 0) == false)
            continue;
        if (shadowRays < MAXIMUM_SHADOW_RAYS)
        {
            shadowRays += 1;
            const SceneQuery visibility = TestVisibility(scene, inoutQueries, hit, light);
            if (visibility == SceneQuery::Failure)
                return float3(0);
            if (visibility != SceneQuery::Miss)
                continue;
        }
        radiance += bsdfValue * light.IncidentCoefficient * cosSurface;
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}
