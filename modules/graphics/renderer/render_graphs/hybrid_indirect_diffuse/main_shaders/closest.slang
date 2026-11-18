void ShadeClosest(SceneContext scene,
                  SurfaceHit hit,
                  inout RayQueryContext inoutQueries,
                  inout ScenePayload inoutPayload)
{
    // The graph shades secondary hits through the typed surface query.
    inoutPayload.Radiance = EvaluateEmission(scene, hit, -hit.IncomingRay.DirectionWorld);
}
