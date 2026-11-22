float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries, GenerateRayResources resources)
{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint frame = resources.History.Load(pixel).x + 1;
    resources.History.Store(pixel, uint4(frame, frame, 0, 0));
    return float3(0);
}
