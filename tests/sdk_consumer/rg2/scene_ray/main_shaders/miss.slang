void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    if (SampleSceneRandom(queries) != payload.ExpectedMissRandom)
        queries.HasFailed = true;
    payload.Radiance = EvaluateEnvironment(scene, ray.DirectionWorld);
}
