#include "work_item_record.slang"

// Each invocation traces a different ray. The resolve reads both rows of work assigned to its pixel.
static const float3 MISS_ORIGIN_WORLD = float3(2, 0, 0);
static const float3 RAY_DIRECTION_WORLD = float3(0, 0, 1);
static const uint TEST_SOURCE_ID = 1;
static const uint EXPECTED_WORK_ITEMS = 7001;

float3 TraceWorkItems(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint index = launch.WorkItemIndex;
    uint capacity;
    uint stride;
    GraphBuffer0.GetDimensions(capacity, stride);
    if (index >= capacity)
        return float3(0);

    Ray ray;
    ray.OriginWorld = index % 2 == 0 ? float3(0) : MISS_ORIGIN_WORLD;
    ray.DirectionWorld = RAY_DIRECTION_WORLD;
    ray.MinDistanceWorld = 0;
    ray.MaxDistanceWorld = 1;
    SurfaceHit hit;
    SceneQuery result = TraceClosest(scene, queries, ray, hit);
    WorkItemRecord record;
    record.Fields = uint4(index + 1, uint(result), launch.WorkItemCount, hit.SourceId.x);
    GraphBuffer0[index] = record.Fields;
    return float3(0);
}

float3 ResolveWorkItems(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint count = GraphBuffer0[0].z;
    uint pixelCount = launch.ExtentPixel.x * launch.ExtentPixel.y;
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint capacity;
    uint stride;
    GraphBuffer0.GetDimensions(capacity, stride);
    if (count != EXPECTED_WORK_ITEMS || count >= capacity || count <= pixelCount)
        return float3(1, 0, 0);

    bool isValid = all(GraphBuffer0[count] == uint4(0));
    for (uint index = pixel; index < count; index += pixelCount)
    {
        bool shouldHit = index % 2 == 0;
        SceneQuery expected = shouldHit == true ? SceneQuery::Surface : SceneQuery::Miss;
        uint source = shouldHit == true ? TEST_SOURCE_ID : 0;
        isValid = isValid == true && all(GraphBuffer0[index] == uint4(index + 1, uint(expected), count, source));
    }
    return isValid == true ? float3(0, 1, 0) : float3(1, 0, 0);
}
