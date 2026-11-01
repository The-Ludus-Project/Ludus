// The same authored functions are linked into software compute and native-query configurations.
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    Ray ray = MakeCameraRay(launch, float2(0.5));
    TraceAndShade(scene, queries, ray, payload);
    return payload.Radiance;
}
void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = EvaluateEnvironment(scene, ray.DirectionWorld);
}
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    MaterialSample material = EvaluateMaterial(scene, hit);
    payload.Radiance = material.BaseColor + EvaluateEmission(scene, hit, -hit.IncomingRay.DirectionWorld);
    // A bounded secondary query and visibility query both use the selected traversal.
    SurfaceHit secondary;
    uint status = TraceClosest(scene, queries, hit.IncomingRay, secondary);
    if (status != SCENE_QUERY_SURFACE || secondary.TriangleIndex != hit.TriangleIndex)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    LightSample light = (LightSample)0;
    light.HasSample = true;
    light.DirectionWorld = -hit.IncomingRay.DirectionWorld;
    if (TestVisibility(scene, queries, hit, light) == SCENE_QUERY_FAILURE)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
}
