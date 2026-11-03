#include "initial_reservoir.slang"

float3 InitializePairwiseReservoir(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasReservoirCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    SurfaceHit hit;
    SceneQuery status = TraceClosest(scene, queries, launch.CameraRay, hit);
    if (status == SceneQuery::Surface)
    {
        SpecularChain chain = FollowReceiverGlass(scene, launch, queries, hit);
        hit = chain.Hit;
        status = chain.IsSurface == true     ? SceneQuery::Surface
                 : queries.HasFailed == true ? SceneQuery::Failure
                                             : SceneQuery::Miss;
    }
    if (status == SceneQuery::Surface)
        GenerateInitialReservoir(scene, hit, launch, queries);
    else if (status == SceneQuery::Miss)
    {
        Reservoir empty = (Reservoir)0;
        StoreReservoir(queries.RandomPixel, false, empty);
    }
    return float3(0);
}
