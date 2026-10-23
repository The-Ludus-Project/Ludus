float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    uint status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint4 initial = GraphBuffer0[pixel];
    bool isSameHit = GraphBuffer1[pixel].x == 1 && initial.x == status &&
                     (status != SCENE_QUERY_SURFACE || initial.y == hit.TriangleIndex);
    // Magenta marks a pass that saw a different raster image than the others.
    if (isSameHit == false)
        return float3(1, 0, 1);
    // Shade the same first hit through this pass's closest-hit or miss callback.
    ScenePayload payload = (ScenePayload)0;
    if (LoadVisibilityAndShade(scene, inoutQueries, launch, payload) != status)
        return float3(-1);
    if (status == SCENE_QUERY_SURFACE)
    {
        // A secondary ray toward the viewer leaves this fixture's plane; any self-hit is a regression.
        Ray secondary = SpawnSurfaceRay(hit, -launch.CameraRay.DirectionWorld, launch.CameraRay.MaxDistanceWorld);
        SurfaceHit secondaryHit;
        if (TraceClosest(scene, inoutQueries, secondary, secondaryHit) != SCENE_QUERY_MISS)
            return float3(-1);
    }
    return payload.Radiance;
}
