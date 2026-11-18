// Encode closest hit and shadow occlusion along the same camera ray as independent channels.
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    Ray ray = MakeCameraRay(launch, float2(0.5));
    SurfaceHit hit;
    SceneQuery primary = TraceClosest(scene, queries, ray, hit);
    SurfaceHit receiver = (SurfaceHit)0;
    receiver.PositionWorld = ray.OriginWorld;
    receiver.TriangleIndex = SCENE_INVALID_INDEX;
    LightSample light = (LightSample)0;
    light.HasSample = true;
    light.DirectionWorld = ray.DirectionWorld;
    SceneQuery shadow = TestVisibility(scene, queries, receiver, light);
    return float3(primary == SceneQuery::Surface ? 1 : 0, shadow == SceneQuery::Surface ? 1 : 0, 0);
}

void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload) {}

void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload) {}
