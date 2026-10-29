// ComputeBvh edge barycentrics: every pixel traces one ray straight down onto the quad diagonal
// from (1, 0, 0) to (0, 1, 0), the edge opposite the first corner of both test triangles. Red
// is a surface hit and green is GetSurfaceHit accepting that hit's triangle and barycentrics,
// as passes that store hits and reload them later do.
static const float3 DIRECTION_WORLD = float3(0, 0, -1);
static const float ORIGIN_HEIGHT_WORLD = 1;
static const float MAXIMUM_DISTANCE_WORLD = 2;

float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint index = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint count = launch.ExtentPixel.x * launch.ExtentPixel.y;
    // A distinct x in [0.5, 1) per pixel; 1 - x is then exact, so the origin lies on x + y = 1.
    float positionX = 0.5 + (float(index) + 0.5) / float(2 * count);
    Ray ray;
    ray.OriginWorld = float3(positionX, 1 - positionX, ORIGIN_HEIGHT_WORLD);
    ray.MinDistanceWorld = 0;
    ray.DirectionWorld = DIRECTION_WORLD;
    ray.MaxDistanceWorld = MAXIMUM_DISTANCE_WORLD;
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, ray, hit);
    SurfaceHit reloaded = GetSurfaceHit(scene, hit.TriangleIndex, hit.Barycentric);
    return float3(status == SCENE_QUERY_SURFACE ? 1 : 0,
                  status == SCENE_QUERY_SURFACE && reloaded.IsValid == true ? 1 : 0,
                  0);
}
void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = float3(0);
}
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    payload.Radiance = float3(0);
}
