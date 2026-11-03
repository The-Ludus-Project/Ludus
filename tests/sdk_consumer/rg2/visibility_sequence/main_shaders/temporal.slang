float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    SceneQuery status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint4 initial = GraphBuffer0[pixel];
    bool isSameHit = initial.x == status && (status != SceneQuery::Surface || initial.y == hit.TriangleIndex);
    GraphBuffer1[pixel] = uint4(isSameHit == true ? 1 : 0, 0, 0, 0);
    return float3(0);
}
