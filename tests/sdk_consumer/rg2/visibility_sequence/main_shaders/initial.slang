float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    // Record the raster first hit this pass reconstructs; later passes must reconstruct the same one.
    SurfaceHit hit;
    uint status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    GraphBuffer0[pixel] = uint4(status, status == SCENE_QUERY_SURFACE ? hit.TriangleIndex : 0, 0, 0);
    return float3(0);
}
