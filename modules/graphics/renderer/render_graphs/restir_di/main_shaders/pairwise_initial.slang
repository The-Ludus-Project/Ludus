#include "initial_reservoir.slang"

float3 InitializePairwiseReservoir(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasReservoirCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, launch.CameraRay, hit);
    if (status == SCENE_QUERY_SURFACE)
        GenerateInitialReservoir(scene, hit, launch, queries);
    else if (status == SCENE_QUERY_MISS)
    {
        Reservoir empty = (Reservoir)0;
        StoreReservoir(queries.RandomPixel, false, empty);
    }
    return float3(0);
}
