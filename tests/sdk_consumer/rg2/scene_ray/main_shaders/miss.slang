void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = EvaluateEnvironment(scene, ray.DirectionWorld);
}
