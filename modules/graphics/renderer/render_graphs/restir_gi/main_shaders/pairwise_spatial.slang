#include "pairwise_common.slang"

float3 ResamplePairwiseGi(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasGiPairwiseCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    uint pixel = GetPixelIndex(launch);
    Reservoir current = LoadReservoir(FIRST_BUFFER, pixel);
    Reservoir result = MakeEmptyReservoir(scene, launch);
    if (IsWrittenThisFrame(current, scene, launch, 1) == false)
    {
        FailFromStoredRecord(launch, current.SampleIndex, current.HasFailed, current.FailureReason, queries);
        FinishReservoir(SECOND_BUFFER, launch, result, queries);
        return float3(0);
    }
    Receiver receiver;
    TargetContext target;
    if (TryTraceReceiver(scene, launch, queries, receiver) == true &&
        TryMakeTargetContext(scene, receiver, queries, target) == true)
    {
        uint4 cell = LoadGiPairwiseCell(launch, pixel);
        if (AreSameReceiver(current, receiver) == false || cell.z < current.CandidateCount ||
            IsPairwiseFinite(asfloat(cell.w)) == false || asfloat(cell.w) < 0)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        result.ReceiverTriangleIndex = receiver.Hit.TriangleIndex;
        result.ReceiverBarycentric = receiver.Hit.Barycentric;
        uint2 extentPixel = GetReuseCellExtent(launch);
        uint neighborCount = extentPixel.x * extentPixel.y - 1;
        uint canonicalSlot = GetReuseCanonicalSlot(launch);
        float scale = 0;
        if (TryComputePairwiseConfidenceScale(RESTIR_PAIRWISE_SAMPLES, neighborCount, scale) == false)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        float confidenceSum = IS_RESTIR_GI_SPATIAL_ENABLED == true ? float(cell.z - current.CandidateCount) * scale : 0;
        float canonicalConfidence = float(current.CandidateCount);
        float canonicalTarget = asfloat(cell.y);
        float weightSum = 0;
        uint selected = CANDIDATE_NONE;
        if (current.Sample.TriangleIndex != SCENE_INVALID_INDEX && canonicalTarget > 0)
        {
            float mis = 0;
            if (TryComputePairwiseCanonicalBase(confidenceSum, canonicalConfidence, mis) == false)
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            SurfaceHit samplePoint = LoadGiPairwiseSamplePoint(scene, current.Sample);
            [loop] for (uint draw = 0; confidenceSum > 0 && draw < RESTIR_PAIRWISE_CANONICAL_SAMPLES; ++draw)
            {
                uint slot = SampleReuseCellIndex(launch, GetGraphPassIndex(), draw, neighborCount, queries);
                slot += slot >= canonicalSlot ? 1 : 0;
                Reservoir neighbor = LoadReservoir(FIRST_BUFFER, GetReuseCellPixel(launch, slot));
                if (neighbor.CandidateCount == 0)
                    continue;
                Receiver other = LoadGiPairwiseReceiver(scene, launch, neighbor);
                TargetContext source;
                if (TryMakeTargetContext(scene, other, queries, source) == false)
                    continue;
                float proxy = ComputeGiPairwiseProxy(scene, source, receiver, current.Sample, samplePoint, queries);
                float term = 0;
                float corrected = 0;
                if (TryComputePairwiseCanonicalTerm(float(neighbor.CandidateCount) * scale,
                                                    confidenceSum,
                                                    canonicalConfidence,
                                                    proxy,
                                                    canonicalTarget,
                                                    term) == false ||
                    TryComputeStochasticMisWeight(term,
                                                  1 / float(neighborCount),
                                                  RESTIR_PAIRWISE_CANONICAL_SAMPLES,
                                                  corrected) == false)
                    FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
                mis += corrected;
            }
            float weight = 0;
            if (TryComputePairwiseResamplingWeight(mis, canonicalTarget, current.Weight, 1, weight) == false)
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            const uint CANONICAL_COIN_DIMENSION = 32;
            StreamCandidate(current.Sample,
                            weight,
                            0,
                            CANDIDATE_INITIAL,
                            SampleReuseCellRandom(launch, GetGraphPassIndex(), CANONICAL_COIN_DIMENSION),
                            result,
                            weightSum,
                            selected,
                            queries);
        }
        GiPairwiseCellReader reader = MakeGiPairwiseCellReader(launch);
        float totalMass = max(asfloat(cell.w) - ComputeReuseCellMass(launch, reader, canonicalSlot), 0);
        const uint NEIGHBOR_RANDOM_BASE = 64;
        const uint DIMENSIONS_PER_DRAW = 4;
        [loop] for (uint draw = 0; confidenceSum > 0 && draw < RESTIR_PAIRWISE_SAMPLES; ++draw)
        {
            uint dimension = NEIGHBOR_RANDOM_BASE + draw * DIMENSIONS_PER_DRAW;
            float probability;
            uint slot = SampleReuseCellNeighbor(launch,
                                                reader,
                                                GetGraphPassIndex(),
                                                dimension,
                                                canonicalSlot,
                                                totalMass,
                                                probability,
                                                queries);
            uint neighborPixel = GetReuseCellPixel(launch, slot);
            Reservoir neighbor = LoadReservoir(FIRST_BUFFER, neighborPixel);
            if (neighbor.CandidateCount == 0 || neighbor.Sample.TriangleIndex == SCENE_INVALID_INDEX ||
                neighbor.Weight == 0)
                continue;
            Receiver other = LoadGiPairwiseReceiver(scene, launch, neighbor);
            SurfaceHit samplePoint = LoadGiPairwiseSamplePoint(scene, neighbor.Sample);
            float destinationTarget = EvaluateGiPairwiseTarget(target, neighbor.Sample, samplePoint);
            float jacobian =
                ComputeReconnectionJacobian(other.Hit.PositionWorld, receiver.Hit.PositionWorld, samplePoint);
            if (destinationTarget <= 0 || jacobian <= 0 ||
                CanGenerateSample(scene, receiver, samplePoint, true, queries) == false)
                continue;
            float proxy = 0;
            float mis = 0;
            float corrected = 0;
            float weight = 0;
            if (TryComputePairwiseShiftedTarget(asfloat(LoadGiPairwiseCell(launch, neighborPixel).y),
                                                jacobian,
                                                proxy) == false ||
                TryComputePairwiseNeighborMis(float(neighbor.CandidateCount) * scale,
                                              confidenceSum,
                                              canonicalConfidence,
                                              proxy,
                                              destinationTarget,
                                              mis) == false ||
                TryComputeStochasticMisWeight(mis, probability, RESTIR_PAIRWISE_SAMPLES, corrected) == false ||
                TryComputePairwiseResamplingWeight(corrected, destinationTarget, neighbor.Weight, jacobian, weight) ==
                    false)
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            StreamCandidate(neighbor.Sample,
                            weight,
                            0,
                            CANDIDATE_FIRST_NEIGHBOR,
                            SampleReuseCellRandom(launch, GetGraphPassIndex(), dimension + DIMENSIONS_PER_DRAW - 1),
                            result,
                            weightSum,
                            selected,
                            queries);
        }
        result.CandidateCount = current.CandidateCount;
        if (result.Sample.TriangleIndex != SCENE_INVALID_INDEX)
        {
            float selectedTarget =
                EvaluateGiPairwiseTarget(target, result.Sample, LoadGiPairwiseSamplePoint(scene, result.Sample));
            if (TryComputePairwiseContributionWeight(weightSum, selectedTarget, result.Weight) == false)
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        }
    }
    FinishReservoir(SECOND_BUFFER, launch, result, queries);
    return float3(0);
}
