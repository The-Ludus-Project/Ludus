float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    uint status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint4 initial = GraphBuffer0[pixel];
    bool isSameHit = initial.x == status && (status != SCENE_QUERY_SURFACE || initial.y == hit.TriangleIndex);
    GraphBuffer1[pixel] = uint4(isSameHit == true ? 1 : 0, 0, 0, 0);
    return float3(0);
}
