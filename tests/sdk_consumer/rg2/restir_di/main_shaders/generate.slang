#include "reservoir.slang"

float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasReservoirCapacity(launch) == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    ScenePayload payload = (ScenePayload)0;
    payload.Launch = launch;
    TraceAndShade(scene, queries, launch.CameraRay, payload);
    return payload.Radiance;
}
