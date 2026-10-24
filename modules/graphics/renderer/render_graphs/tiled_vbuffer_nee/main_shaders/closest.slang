void ShadeClosest(SceneContext scene,
                  SurfaceHit hit,
                  inout RayQueryContext inoutQueries,
                  inout ScenePayload inoutPayload)
{
    // The ray pipeline requires this callback, but the passes query the scene directly
    // (LoadVisibilitySurface, TraceClosest, TestVisibility) and never invoke it.
    inoutPayload.Radiance = EvaluateEmission(scene, hit, -hit.IncomingRay.DirectionWorld);
}
