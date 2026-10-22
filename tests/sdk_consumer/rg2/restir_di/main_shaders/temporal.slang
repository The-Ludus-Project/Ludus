#include "reservoir.slang"

void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    Reservoir current = LoadReservoir(queries.RandomPixel, false);
    Reservoir history = LoadReservoir(queries.RandomPixel, true);
    Reservoir result = MakeReceiverReservoir(scene, payload.Launch, hit);
    if (HasMatchingIdentity(current, scene, payload.Launch) == false ||
        HasMatchingTemporalReceiver(current, result) == false)
    {
        queries.HasFailed = true;
        return;
    }
    float weightSum = 0;
    MergeReservoir(scene, hit, current, SampleReuseRandom(payload.Launch, 0), result, weightSum, queries);
    if (HasMatchingIdentity(history, scene, payload.Launch) == true &&
        HasMatchingTemporalReceiver(history, result) == true)
    {
        MergeReservoir(scene, hit, history, SampleReuseRandom(payload.Launch, 1), result, weightSum, queries);
    }
    FinalizeReservoir(scene, hit, weightSum, result, queries);
    if (queries.HasFailed == false)
    {
        // Each invocation updates only its own scratch record. History stays immutable.
        StoreReservoir(queries.RandomPixel, false, result);
    }
}
