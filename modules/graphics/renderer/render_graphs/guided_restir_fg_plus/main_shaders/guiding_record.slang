#include "path_evaluation.slang"

float3 RecordPathGuiding(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasCurrentFgPool(scene, launch, queries) == false)
        return float3(0);
    FgPathReservoir reservoir = LoadFgPath(true, GetFgPixel(launch));
    if (IsCurrentFgPath(reservoir, scene, launch, queries) == true && reservoir.Weight > 0)
    {
        FgPoint receiver = LoadFgReceiver(scene, reservoir, queries);
        RecordFgGuiding(reservoir.Photon,
                        EvaluateFgPath(scene, reservoir, receiver, false, queries) * reservoir.Weight);
    }
    RecordFgPoolFailure(queries);
    return float3(0);
}
