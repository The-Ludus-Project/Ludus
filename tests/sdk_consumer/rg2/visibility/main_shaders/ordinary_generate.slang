float3 GenerateOrdinaryRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, inoutQueries, launch.CameraRay, payload);
    return payload.Radiance;
}
