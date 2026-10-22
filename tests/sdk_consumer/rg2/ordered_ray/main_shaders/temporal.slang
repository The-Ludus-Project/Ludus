float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint frame = GraphBuffer0[pixel].x;
    GraphBuffer1[pixel] = uint4(frame, GraphBuffer1[pixel].y + frame, 0, 0);
    return float3(0);
}
