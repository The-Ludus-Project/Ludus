float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint frame = GraphBuffer0[pixel].x + 1;
    GraphBuffer0[pixel] = uint4(frame, frame, 0, 0);
    return float3(0);
}
