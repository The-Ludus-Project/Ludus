void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    if (SampleSceneRandom(queries) != payload.ExpectedMissRandom)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    if (SampleSceneRandom(queries, 1, 2) != 1)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_AUTHORED_FIRST);
    payload.Radiance = EvaluateEnvironment(scene, ray.DirectionWorld);
}
