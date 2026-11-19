void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext inoutQueries, inout ScenePayload inoutPayload)
{
    inoutPayload.Radiance = EvaluateEnvironment(scene, ray.DirectionWorld);
}

void ShadeClosest(SceneContext scene,
                  SurfaceHit hit,
                  inout RayQueryContext inoutQueries,
                  inout ScenePayload inoutPayload)
{
    inoutPayload.Radiance = EvaluateMaterial(scene, hit).BaseColor;
}
