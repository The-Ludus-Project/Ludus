// Traversal parity: the same callbacks run with TriangleLoop, NativeQuery and ComputeBvh. Each
// pixel encodes exact query outcomes, not shading, so the three images can be compared directly:
// red is the camera ray's packed triangle plus one, green is a secondary surface query's triangle
// plus one, and blue is a visibility query's outcome. Zero means a miss in red and green.
static const float3 SECONDARY_DIRECTION_WORLD = float3(0.267261, 0.534522, 0.801784);
static const float3 VISIBILITY_DIRECTION_WORLD = float3(-0.301511, 0.301511, 0.904534);
static const float SECONDARY_DISTANCE_WORLD = 100;
static const uint IDENTITY_MASK = 255;
static const float CHANNEL_MAXIMUM = 255;
static const float VISIBLE_LEVEL = 255;
static const float OCCLUDED_LEVEL = 128;

float EncodeIdentity(uint status, SurfaceHit hit)
{
    return status == SCENE_QUERY_SURFACE ? float((hit.TriangleIndex + 1) & IDENTITY_MASK) / CHANNEL_MAXIMUM : 0;
}
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, MakeCameraRay(launch, float2(0.5)), payload);
    return payload.Radiance;
}
void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = float3(0);
}
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    SurfaceHit secondary;
    uint secondaryStatus = TraceClosest(scene,
                                        queries,
                                        SpawnSurfaceRay(hit, SECONDARY_DIRECTION_WORLD, SECONDARY_DISTANCE_WORLD),
                                        secondary);
    LightSample light = (LightSample)0;
    light.HasSample = true;
    light.DirectionWorld = VISIBILITY_DIRECTION_WORLD;
    uint visibility = TestVisibility(scene, queries, hit, light);
    payload.Radiance = float3(EncodeIdentity(SCENE_QUERY_SURFACE, hit),
                              EncodeIdentity(secondaryStatus, secondary),
                              (visibility == SCENE_QUERY_MISS ? VISIBLE_LEVEL : OCCLUDED_LEVEL) / CHANNEL_MAXIMUM);
}
