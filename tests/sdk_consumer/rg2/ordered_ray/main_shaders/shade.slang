float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint frame = GraphBuffer0[pixel].x;
    uint sum = GraphBuffer1[pixel].y;
    bool isValid = frame == launch.SampleIndex + 1 && sum == frame * (frame + 1) / 2;
    return isValid == true ? float3(frame, 0, 0) : float3(0, 1, 0);
}
