void ShadeClosest(SceneContext scene,
                  SurfaceHit hit,
                  inout RayQueryContext inoutQueries,
                  inout ScenePayload inoutPayload)
{
    // Shading traces only visibility rays; the pipeline still needs a closest-hit callback.
    inoutPayload.Radiance = EvaluateEmission(scene, hit, -hit.IncomingRay.DirectionWorld);
}
