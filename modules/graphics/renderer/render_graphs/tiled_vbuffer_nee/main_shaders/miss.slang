void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext inoutQueries, inout ScenePayload inoutPayload)
{
    // Required by the ray pipeline; the passes query the scene directly and never invoke it.
    inoutPayload.Radiance = EvaluateEnvironment(scene, ray.DirectionWorld);
}
