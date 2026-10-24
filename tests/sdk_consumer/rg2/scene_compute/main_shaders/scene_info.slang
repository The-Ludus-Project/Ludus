// Scene compute kernel (compute ray-tracing spec, C03): one authored function traces the World
// with native ray queries and returns linear radiance, with no hit or miss callbacks.
// Red: the sample number (SampleIndex + 1), so Accumulate's mean and stop limit are observable.
// Green: 1 where the camera ray hits a triangle, 0 on a miss. Blue: GetSceneInfo's triangle count.
static const uint FAILURE_COLUMN = 0;

float3 ShadeSceneInfo(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (launch.PositionPixel.x == FAILURE_COLUMN)
    {
        // A compute pass has no hit or miss callback, so TraceAndShade must latch a failure
        // and the pixel must show the error color instead of this payload.
        ScenePayload payload = (ScenePayload)0;
        payload.Radiance = float3(1);
        TraceAndShade(scene, queries, launch.CameraRay, payload);
        return payload.Radiance;
    }
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, launch.CameraRay, hit);
    SceneInfo info = GetSceneInfo(scene);
    return float3(float(launch.SampleIndex + 1), status == SCENE_QUERY_SURFACE ? 1 : 0, float(info.TriangleCount));
}
