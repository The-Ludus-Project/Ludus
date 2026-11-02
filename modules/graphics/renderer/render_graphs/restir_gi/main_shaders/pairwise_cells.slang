#include "pairwise_common.slang"

float3 BuildPairwiseGiCells(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasGiPairwiseCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    if (any(launch.PositionPixel % REUSE_CELL_WIDTH != 0) == true)
        return float3(0);
    uint2 extentPixel = GetReuseCellExtent(launch);
    uint count = extentPixel.x * extentPixel.y;
    float prefix = 0;
    uint confidenceSum = 0;
    [loop] for (uint slot = 0; slot < count; ++slot)
    {
        uint pixel = GetReuseCellPixel(launch, slot);
        Reservoir source = LoadReservoir(FIRST_BUFFER, pixel);
        float target = 0;
        if (IsWrittenThisFrame(source, scene, launch, 1) == false)
            FailFromStoredRecord(launch, source.SampleIndex, source.HasFailed, source.FailureReason, queries);
        else if (source.CandidateCount > 0)
        {
            confidenceSum += source.CandidateCount;
            if (source.Sample.TriangleIndex != SCENE_INVALID_INDEX && source.Weight > 0)
            {
                Receiver receiver = LoadGiPairwiseReceiver(scene, launch, source);
                TargetContext context;
                if (TryMakeTargetContext(scene, receiver, queries, context) == true)
                    target = EvaluateGiPairwiseTarget(context,
                                                      source.Sample,
                                                      LoadGiPairwiseSamplePoint(scene, source.Sample));
                prefix += float(source.CandidateCount) * target * source.Weight;
            }
        }
        if (IsPairwiseFinite(prefix) == false || prefix < 0)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        StoreGiPairwiseCell(launch, pixel, uint4(asuint(prefix), asuint(target), 0, 0));
    }
    [loop] for (uint slot = 0; slot < count; ++slot)
    {
        uint pixel = GetReuseCellPixel(launch, slot);
        uint4 record = LoadGiPairwiseCell(launch, pixel);
        StoreGiPairwiseCell(
            launch,
            pixel,
            uint4(record.xy, confidenceSum, queries.HasFailed == true ? asuint(-1.0f) : asuint(prefix)));
    }
    return float3(0);
}
