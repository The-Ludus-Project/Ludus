// The shade pass's TraceAndShade reaches this callback through pass-index dispatch, although
// a scene compute pass occupies another index of the same sequence.
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = float3(1);
}
