#include "reservoir.slang"

float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasReservoirCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    ScenePayload payload = (ScenePayload)0;
    payload.Launch = launch;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    return payload.Radiance;
}
