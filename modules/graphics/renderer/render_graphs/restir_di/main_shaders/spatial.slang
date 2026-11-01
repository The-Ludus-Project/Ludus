#include "reservoir.slang"

void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    Reservoir current = LoadReservoir(queries.RandomPixel, false);
    Reservoir result = MakeReceiverReservoir(scene, payload.Launch, hit);
    if (HasMatchingIdentity(current, scene, payload.Launch) == false ||
        HasMatchingTemporalReceiver(current, result) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return;
    }
    float3 surfaceEmission = EvaluateEmission(scene, hit, result.ViewDirectionWorld);
    payload.Radiance = surfaceEmission;
    if (any(surfaceEmission > 0) == false && current.CandidateCount > 0)
    {
        float weightSum = 0;
        MergeReservoir(scene, hit, current, SampleReuseRandom(payload.Launch, 0), result, weightSum, queries);
        [loop] for (uint i = 0; i < RESTIR_SPATIAL_NEIGHBOR_COUNT; ++i)
        {
            int2 offsetPixel = i < 2 ? int2(i == 0 ? -1 : 1, 0) : int2(0, i == 2 ? -1 : 1);
            int2 positionPixel = int2(payload.Launch.PositionPixel) + offsetPixel;
            if (any(positionPixel < 0) == true || any(positionPixel >= int2(payload.Launch.ExtentPixel)) == true)
            {
                continue;
            }
            uint pixel = uint(positionPixel.y) * payload.Launch.ExtentPixel.x + uint(positionPixel.x);
            Reservoir neighbor = LoadReservoir(pixel, false);
            if (HasMatchingIdentity(neighbor, scene, payload.Launch) == true && neighbor.CandidateCount > 0 &&
                CanReuseSpatialReceiver(scene, hit, neighbor) == true)
            {
                MergeReservoir(scene,
                               hit,
                               neighbor,
                               SampleReuseRandom(payload.Launch, i + 1),
                               result,
                               weightSum,
                               queries);
            }
        }
        FinalizeReservoir(scene, hit, weightSum, result, queries);
        payload.Radiance = ShadeReservoir(scene, hit, result, queries);
    }
    if (queries.HasFailed == false)
    {
        // All neighbor reads use scratch; no invocation reads the history being written.
        StoreReservoir(queries.RandomPixel, true, result);
    }
}
