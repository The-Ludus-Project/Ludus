#include "reservoir.slang"

float3 ResampleTemporal(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasCapacity(launch, true) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    Reservoir current = LoadReservoir(GetPixelIndex(launch), false);
    Reservoir history = LoadReservoir(GetPixelIndex(launch), true);
    Reservoir result = current;
    result.CandidateCount = 0;
    result.Weight = 0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    if (IsCurrentRecord(current, scene, launch, queries) == true)
    {
        Prefix support = LoadSupport(scene, current);
        float massSum = 0;
        MergeReservoir(scene, launch, support, current, 0, result, massSum, queries);
        // Consecutive frames only. Scene generation, extent and layout reject stale or resized storage.
        if (IS_CRIS_TEMPORAL_ENABLED == true && HasIdentity(history, scene, launch) == true && launch.SampleIndex > 0 &&
            history.SampleIndex == launch.SampleIndex - 1)
            MergeReservoir(scene, launch, support, history, 1, result, massSum, queries);
        FinalizeReservoir(scene, support, massSum, result, queries);
    }
    StoreReservoir(launch, true, result, queries);
    return float3(0);
}
