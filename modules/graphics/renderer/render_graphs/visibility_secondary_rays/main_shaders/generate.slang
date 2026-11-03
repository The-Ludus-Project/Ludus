float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    SceneQuery status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    if (status == SceneQuery::Miss)
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    if (status != SceneQuery::Surface)
        return float3(0);

    MaterialSample material = EvaluateMaterial(scene, hit);
    if (material.IsValid == false)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    float3 emission = EvaluateEmission(scene, hit, -launch.CameraRay.DirectionWorld);
    if (IsDielectricMaterial(material) == true)
    {
        // Glass reflects or refracts by its Fresnel probability; the weight carries 1/eta^2 across refraction.
        float3 samples =
            float3(SampleSceneRandom(inoutQueries), SampleSceneRandom(inoutQueries), SampleSceneRandom(inoutQueries));
        StandardBsdfTransportSample sampled =
            SampleDielectricTransport(material, -launch.CameraRay.DirectionWorld, samples);
        if (sampled.HasSample == false)
            return emission;
        ScenePayload transmitted = (ScenePayload)0;
        TraceAndShade(scene,
                      inoutQueries,
                      SpawnSurfaceRay(hit, sampled.DirectionWorld, launch.CameraRay.MaxDistanceWorld),
                      transmitted);
        return emission + sampled.Weight * transmitted.Radiance;
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
    return emission + material.BaseColor * payload.Radiance;
}
