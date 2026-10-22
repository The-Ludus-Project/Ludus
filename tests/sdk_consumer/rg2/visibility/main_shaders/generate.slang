float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    // Exercise the two user storage slots together with the visibility textures.
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    VisibilityHistory first = HistoryA[pixel];
    VisibilityHistory second = HistoryB[pixel];
    first.Sample += 1;
    second.Sample = first.Sample;
    HistoryA[pixel] = first;
    HistoryB[pixel] = second;
    if (first.Sample.x < 1 || second.Sample.x < 1)
        return float3(-1);
    SurfaceHit hit;
    uint surfaceQueries = inoutQueries.SurfaceQueries;
    uint visibilityQueries = inoutQueries.VisibilityQueries;
    uint status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    if (status == SCENE_QUERY_MISS)
        return float3(0, 0, 1);
    if (status != SCENE_QUERY_SURFACE)
        return float3(-1);
    static const float RECONSTRUCTION_POSITION_TOLERANCE_WORLD = 0.0001;
    float3 cameraOffsetWorld = hit.PositionWorld - launch.CameraRay.OriginWorld;
    if (length(cross(cameraOffsetWorld, launch.CameraRay.DirectionWorld)) > RECONSTRUCTION_POSITION_TOLERANCE_WORLD ||
        inoutQueries.SurfaceQueries != surfaceQueries || inoutQueries.VisibilityQueries != visibilityQueries)
        return float3(-1);
    MaterialSample material = EvaluateMaterial(scene, hit);
    // A secondary ray toward the viewer leaves this fixture's plane; any self-hit is a regression.
    Ray secondary = SpawnSurfaceRay(hit, -launch.CameraRay.DirectionWorld, launch.CameraRay.MaxDistanceWorld);
    SurfaceHit secondaryHit;
    if (TraceClosest(scene, inoutQueries, secondary, secondaryHit) != SCENE_QUERY_MISS)
        return float3(-1);
    return material.BaseColor;
}
