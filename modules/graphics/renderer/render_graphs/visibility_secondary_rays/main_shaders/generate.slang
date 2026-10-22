float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    uint status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    if (status == SCENE_QUERY_MISS)
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    if (status != SCENE_QUERY_SURFACE)
        return float3(0);

    MaterialSample material = EvaluateMaterial(scene, hit);
    if (material.IsValid == false)
    {
        inoutQueries.HasFailed = true;
        return float3(0);
    }
    // An editable, base-color-tinted ideal reflection; no roughness/metallic BRDF is evaluated.
    float3 geometricNormalWorld =
        faceforward(hit.GeometricNormalWorld, launch.CameraRay.DirectionWorld, hit.GeometricNormalWorld);
    float3 shadingNormalWorld =
        faceforward(material.ShadingNormalWorld, launch.CameraRay.DirectionWorld, material.ShadingNormalWorld);
    float3 directionWorld = reflect(launch.CameraRay.DirectionWorld, shadingNormalWorld);
    if (dot(directionWorld, geometricNormalWorld) <= 0)
        directionWorld = reflect(launch.CameraRay.DirectionWorld, geometricNormalWorld);

    Ray secondary = SpawnSurfaceRay(hit, directionWorld, launch.CameraRay.MaxDistanceWorld);
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, inoutQueries, secondary, payload);
    float3 emission = EvaluateEmission(scene, hit, -launch.CameraRay.DirectionWorld);
    return emission + material.BaseColor * payload.Radiance;
}
