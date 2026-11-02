#include "pairwise_common.slang"

// One invocation owns each tile. All other invocations return without barriers,
// including partial edge tiles. O(pixels) cached target work precedes all reuse.
// Record: inclusive selection CDF, source target, total confidence, total CDF.
[noinline] float3 BuildPairwiseCells(SceneContext scene, RayLaunch launch, inout RayQueryContext queries) {
    if (HasReservoirCapacity(launch) == false || HasPairwiseCellCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    if (any(launch.PositionPixel % PAIRWISE_TILE_WIDTH != 0) == true)
        return float3(0);
    uint2 extentPixel = GetPairwiseTileExtent(launch);
    uint count = extentPixel.x * extentPixel.y;
    float prefix = 0;
    uint confidenceSum = 0;
    [loop] for (uint slot = 0; slot < count; ++slot)
    {
        uint pixel = GetPairwisePixel(launch, slot);
        Reservoir source = LoadReservoir(pixel, false);
        float target = 0;
        if (HasMatchingIdentity(source, scene, launch) == true && source.CandidateCount > 0 &&
            source.TriangleIndex != SCENE_INVALID_INDEX)
        {
            SurfaceHit receiver = GetPairwiseReceiver(scene, source);
            if (receiver.IsValid == false)
            {
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
                return float3(0);
            }
            target = EvaluateSampleTarget(scene, receiver, source.TriangleIndex, source.Barycentric);
            prefix += float(source.CandidateCount) * target * source.Weight;
            confidenceSum += source.CandidateCount;
        }
        if (isfinite(prefix) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            return float3(0);
        }
        StorePairwiseCell(launch, pixel, uint4(asuint(prefix), asuint(target), 0, 0));
    }
    [loop] for (uint slot = 0; slot < count; ++slot)
    {
        uint pixel = GetPairwisePixel(launch, slot);
        uint4 record = LoadPairwiseCell(launch, pixel);
        StorePairwiseCell(launch, pixel, uint4(record.xy, confidenceSum, asuint(prefix)));
    }
    return float3(0);
}
