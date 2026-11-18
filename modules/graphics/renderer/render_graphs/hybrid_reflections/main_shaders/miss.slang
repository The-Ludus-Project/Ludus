void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext inoutQueries, inout ScenePayload inoutPayload)
{
    inoutPayload.Radiance = EvaluateEnvironment(scene, ray.DirectionWorld);
}
