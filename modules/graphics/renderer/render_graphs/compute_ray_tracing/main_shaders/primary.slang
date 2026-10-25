#include "hit_record.slang"

// Pass 1 of 2 (scene compute): trace one jittered camera ray per pixel and record what it hit.
// The color written here is an intermediate; the shade pass replaces it. One surface query.
float3 TracePrimaryHit(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasHitRecordCapacity(launch) == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    HitRecord record = (HitRecord)0;
    record.JitterPixel = float2(SampleSceneRandom(launch, PRIMARY_RANDOM_DIMENSION_FIRST),
                                SampleSceneRandom(launch, PRIMARY_RANDOM_DIMENSION_FIRST + 1));
    record.SampleIndex = launch.SampleIndex;
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, MakeCameraRay(launch, record.JitterPixel), hit);
    // A failed query is recorded too, so the shade pass shows the error color for this sample.
    record.HasFailed = status == SCENE_QUERY_FAILURE;
    record.IsHit = status == SCENE_QUERY_SURFACE;
    if (record.IsHit == true)
    {
        record.TriangleIndex = hit.TriangleIndex;
        record.DistanceWorld = hit.DistanceWorld;
        record.Barycentric = hit.Barycentric;
        record.IsFrontFace = hit.IsFrontFace;
    }
    StoreHitRecord(launch, record);
    return float3(0);
}
