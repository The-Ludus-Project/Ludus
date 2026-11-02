#include "reservoir.slang"

// Pass 3 of 4 (scene compute), the paper's Algorithm 4: update the pixel's spatial reservoir (`spatial`,
// GraphBuffer1) with temporal reservoirs (`temporal`, GraphBuffer0) of similar nearby pixels. This pass only reads
// the temporal reservoirs, which the previous pass finished, and writes only its own spatial record, so no
// double-buffering is needed. The candidates are the pixel's previous spatial reservoir (line 2), its own temporal
// reservoir, then neighbours chosen uniformly in a disk whose radius adapts as in section 5. Each neighbour's sample is
// shifted with the Jacobian of Equation 11 and, for the unbiased variant, tested for visibility (lines 12-13); the
// count Z (lines 16-19) then keeps only candidates whose visible points could have proposed the selected sample.
float3 ResampleSpatial(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRecordCapacity(launch, RESERVOIR_RECORD_WORDS, RESERVOIR_RECORD_WORDS) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    const uint pixel = GetPixelIndex(launch);
    Reservoir temporal = LoadReservoir(FIRST_BUFFER, pixel);
    Reservoir result = MakeEmptyReservoir(scene, launch);
    if (IsWrittenThisFrame(temporal, scene, launch, RESTIR_GI_TEMPORAL_MAX_M) == false)
    {
        FailFromStoredRecord(launch, temporal.SampleIndex, temporal.HasFailed, temporal.FailureReason, queries);
        FinishReservoir(SECOND_BUFFER, launch, result, queries);
        return float3(0);
    }
    Reservoir previous = LoadReservoir(SECOND_BUFFER, pixel);
    Receiver receiver;
    TargetContext target;
    if (TryTraceReceiver(scene, launch, queries, receiver) == true &&
        TryMakeTargetContext(scene, receiver, queries, target) == true)
    {
        if (AreSameReceiver(temporal, receiver) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
            FinishReservoir(SECOND_BUFFER, launch, result, queries);
            return float3(0);
        }
        result.ReceiverTriangleIndex = receiver.Hit.TriangleIndex;
        result.ReceiverBarycentric = receiver.Hit.Barycentric;
        float weightSum = 0;
        uint selected = CANDIDATE_NONE;
        Receiver previousReceiver = LoadReceiver(scene, previous.ReceiverTriangleIndex, previous.ReceiverBarycentric);
        const bool hasPrevious = IS_RESTIR_GI_SPATIAL_ENABLED == true &&
                                 HasMatchingIdentity(previous, scene, launch, RESTIR_GI_SPATIAL_MAX_M) == true &&
                                 previous.CandidateCount > 0 &&
                                 AreReceiversSimilar(launch, receiver, previousReceiver) == true;
        const bool isPreviousSameReceiver = AreSameReceiver(previous, receiver);
        if (hasPrevious == true)
        {
            float weight =
                ComputeShiftedWeight(scene, target, previousReceiver, previous, isPreviousSameReceiver, queries);
            StreamCandidate(previous.Sample,
                            weight,
                            previous.CandidateCount,
                            CANDIDATE_HISTORY,
                            SampleGiRandom(launch, RESAMPLING_COIN_DIMENSION_FIRST + CANDIDATE_HISTORY),
                            result,
                            weightSum,
                            selected,
                            queries);
        }
        // The pixel's own temporal reservoir belongs to this very visible point: no shift and no visibility ray.
        StreamCandidate(temporal.Sample,
                        ComputeShiftedWeight(scene, target, receiver, temporal, true, queries),
                        temporal.CandidateCount,
                        CANDIDATE_OWN_TEMPORAL,
                        SampleGiRandom(launch, RESAMPLING_COIN_DIMENSION_FIRST + CANDIDATE_OWN_TEMPORAL),
                        result,
                        weightSum,
                        selected,
                        queries);
        // Section 5: more neighbours while the spatial reservoir has seen fewer than half its maximum M.
        uint neighborLimit = 0;
        if (IS_RESTIR_GI_SPATIAL_ENABLED == true)
        {
            const bool isSparse = hasPrevious == false || previous.CandidateCount * 2 < RESTIR_GI_SPATIAL_MAX_M;
            neighborLimit = min(isSparse == true ? RESTIR_GI_SPATIAL_SPARSE_NEIGHBORS : RESTIR_GI_SPATIAL_NEIGHBORS,
                                SPATIAL_NEIGHBOR_CAPACITY);
        }
        uint neighborPixels[SPATIAL_NEIGHBOR_CAPACITY];
        [unroll] for (uint i = 0; i < SPATIAL_NEIGHBOR_CAPACITY; ++i)
        {
            neighborPixels[i] = 0;
        }
        uint neighborCount = 0;
        float radiusPixels =
            max(SPATIAL_INITIAL_RADIUS_FRACTION * float(max(launch.ExtentPixel.x, launch.ExtentPixel.y)),
                SPATIAL_MINIMUM_RADIUS_PIXELS);
        [loop] for (uint i = 0; i < neighborLimit; ++i)
        {
            const uint offsetDimension = SPATIAL_OFFSET_DIMENSION_FIRST + 2 * i;
            float distancePixels = radiusPixels * sqrt(SampleGiRandom(launch, offsetDimension));
            float angle = 2 * PI * SampleGiRandom(launch, offsetDimension + 1);
            int2 offsetPixel = int2(round(float2(cos(angle), sin(angle)) * distancePixels));
            int2 positionPixel = int2(launch.PositionPixel) + offsetPixel;
            if (all(offsetPixel == 0) == true || any(positionPixel < 0) == true ||
                any(positionPixel >= int2(launch.ExtentPixel)) == true)
            {
                continue;
            }
            const uint neighborPixel = uint(positionPixel.y) * launch.ExtentPixel.x + uint(positionPixel.x);
            Reservoir neighbor = LoadReservoir(FIRST_BUFFER, neighborPixel);
            Receiver neighborReceiver =
                LoadReceiver(scene, neighbor.ReceiverTriangleIndex, neighbor.ReceiverBarycentric);
            // Lines 6-8: the similarity test. A neighbour whose temporal pass failed is skipped, not propagated.
            const bool isSimilar = IsWrittenThisFrame(neighbor, scene, launch, RESTIR_GI_TEMPORAL_MAX_M) == true &&
                                   AreReceiversSimilar(launch, receiver, neighborReceiver) == true;
            // Lines 9-13.
            float weight =
                isSimilar == true ? ComputeShiftedWeight(scene, target, neighborReceiver, neighbor, false, queries) : 0;
            // Section 5: halve the radius when the neighbour provides no sample.
            if (weight <= 0)
                radiusPixels = max(radiusPixels * 0.5f, SPATIAL_MINIMUM_RADIUS_PIXELS);
            if (isSimilar == false)
                continue;
            // Line 14 MERGE, line 15 Q <- Q + q_n.
            StreamCandidate(neighbor.Sample,
                            weight,
                            neighbor.CandidateCount,
                            CANDIDATE_FIRST_NEIGHBOR + neighborCount,
                            SampleGiRandom(launch, RESAMPLING_COIN_DIMENSION_FIRST + CANDIDATE_FIRST_NEIGHBOR + i),
                            result,
                            weightSum,
                            selected,
                            queries);
            neighborPixels[neighborCount] = neighborPixel;
            ++neighborCount;
        }
        // Lines 16-19: Z sums the M of candidates whose visible points could have proposed the selected sample. The
        // biased variant normalizes by every merged M.
        uint normalizationCount = result.CandidateCount;
        if (IS_RESTIR_GI_UNBIASED == true && result.Sample.TriangleIndex != SCENE_INVALID_INDEX)
        {
            SurfaceHit samplePoint = GetSurfaceHit(scene, result.Sample.TriangleIndex, result.Sample.Barycentric);
            if (hasPrevious == true && isPreviousSameReceiver == false && selected != CANDIDATE_HISTORY &&
                CanGenerateSample(scene, previousReceiver, samplePoint, true, queries) == false)
            {
                normalizationCount -= previous.CandidateCount;
            }
            [loop] for (uint k = 0; k < neighborCount; ++k)
            {
                if (selected == CANDIDATE_FIRST_NEIGHBOR + k)
                    continue;
                Reservoir neighbor = LoadReservoir(FIRST_BUFFER, neighborPixels[k]);
                Receiver neighborReceiver =
                    LoadReceiver(scene, neighbor.ReceiverTriangleIndex, neighbor.ReceiverBarycentric);
                if (CanGenerateSample(scene, neighborReceiver, samplePoint, true, queries) == false)
                    normalizationCount -= neighbor.CandidateCount;
            }
        }
        // Lines 20-21.
        FinalizeReservoir(scene, target, weightSum, normalizationCount, RESTIR_GI_SPATIAL_MAX_M, result, queries);
    }
    FinishReservoir(SECOND_BUFFER, launch, result, queries);
    return float3(0);
}
