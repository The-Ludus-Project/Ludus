#include "reservoir.slang"

float3 ResampleSpatial(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasCapacity(launch, true) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    Reservoir current = LoadReservoir(GetPixelIndex(launch), false);
    Reservoir result = current;
    result.CandidateCount = 0;
    result.Weight = 0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    if (IsCurrentRecord(current, scene, launch, queries) == true)
    {
        Prefix support = LoadSupport(scene, current);
        float massSum = 0;
        MergeReservoir(scene, launch, support, current, 0, result, massSum, queries);
        // Read only the completed temporal buffer; never read another invocation's spatial output.
        // Screen offsets and record identity are independent of suffixes. No radiance/visibility pruning.
        uint count = IS_CRIS_SPATIAL_ENABLED == true ? min(CRIS_SPATIAL_NEIGHBORS, MAXIMUM_NEIGHBORS) : 0;
        [loop] for (uint i = 0; i < count; ++i)
        {
            uint dimension = 1 + 2 * i;
            float radiusPixels = NEIGHBOR_RADIUS_PIXELS * sqrt(SamplePassRandom(launch, dimension));
            float angle = 2 * PI * SamplePassRandom(launch, dimension + 1);
            int2 offsetPixel = int2(round(radiusPixels * float2(cos(angle), sin(angle))));
            int2 positionPixel = int2(launch.PositionPixel) + offsetPixel;
            if (all(offsetPixel == 0) == true || any(positionPixel < 0) == true ||
                any(positionPixel >= int2(launch.ExtentPixel)) == true)
                continue;
            uint pixel = uint(positionPixel.y) * launch.ExtentPixel.x + uint(positionPixel.x);
            Reservoir neighbor = LoadReservoir(pixel, false);
            if (HasIdentity(neighbor, scene, launch) == false || neighbor.SampleIndex != launch.SampleIndex)
                continue;
            uint coinDimension = 1 + 2 * MAXIMUM_NEIGHBORS + i;
            MergeReservoir(scene, launch, support, neighbor, coinDimension, result, massSum, queries);
        }
        FinalizeReservoir(scene, support, massSum, result, queries);
    }
    StoreReservoir(launch, true, result, queries);
    return float3(0);
}
