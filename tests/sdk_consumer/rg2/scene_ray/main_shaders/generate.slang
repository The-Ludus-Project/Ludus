float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    payload.Launch = launch;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    return payload.Radiance;
}
