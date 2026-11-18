// Reconstruct the identified raster surface once and evaluate unoccluded direct lighting.
float3 ResolveOpaqueMaterial(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
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
    const uint lightCount = GetLightCount(scene);
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        const LightInfo source = GetLightInfo(scene, lightIndex);
        if (source.Kind != SceneLightKind::Directional && source.Kind != SceneLightKind::Point &&
            source.Kind != SceneLightKind::Spot)
            break;
        const IncidentLightSample light = SampleIncidentLight(scene, lightIndex, hit, launch, 0, inoutQueries);
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
        radiance += bsdfValue * light.IncidentCoefficient * cosSurface;
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}
