#include "reservoir.slang"

// Pass 2 of 4 (scene compute), the paper's Algorithm 3: merge this frame's initial sample (`samples`, GraphBuffer0)
// into the pixel's temporal reservoir (`temporal`, GraphBuffer1). Every invocation reads and writes only its own
// records, so no invocation races another. Without motion vectors the history is the same pixel's reservoir; it is
// kept when its visible point passes the similarity test and is shifted to this frame's visible point with the
// Jacobian of Equation 11 (exactly one for a still camera). Queries: the camera ray and, for the unbiased variant
// after the visible point moved, one visibility ray for the shift and one for the count Z.
float3 ResampleTemporal(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRecordCapacity(launch, SAMPLE_RECORD_WORDS, RESERVOIR_RECORD_WORDS) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    const uint pixel = GetPixelIndex(launch);
    SampleRecord initial = LoadSampleRecord(pixel);
    Reservoir result = MakeEmptyReservoir(scene, launch);
    if (initial.SampleIndex != launch.SampleIndex || initial.HasFailed == true)
    {
        FailFromStoredRecord(launch, initial.SampleIndex, initial.HasFailed, initial.FailureReason, queries);
        FinishReservoir(SECOND_BUFFER, launch, result, queries);
        return float3(0);
    }
    Reservoir history = LoadReservoir(SECOND_BUFFER, pixel);
    Receiver receiver;
    TargetContext target;
    if (TryTraceReceiver(scene, launch, queries, receiver) == true &&
        TryMakeTargetContext(scene, receiver, queries, target) == true)
    {
        result.ReceiverTriangleIndex = receiver.Hit.TriangleIndex;
        result.ReceiverBarycentric = receiver.Hit.Barycentric;
        float weightSum = 0;
        uint selected = CANDIDATE_NONE;
        // Algorithm 3 line 3: R <- TemporalReservoirBuffer[q].
        Receiver historyReceiver = LoadReceiver(scene, history.ReceiverTriangleIndex, history.ReceiverBarycentric);
        const bool hasHistory = IS_RESTIR_GI_TEMPORAL_ENABLED == true &&
                                HasMatchingIdentity(history, scene, launch, RESTIR_GI_TEMPORAL_MAX_M) == true &&
                                history.CandidateCount > 0 &&
                                AreReceiversSimilar(launch, receiver, historyReceiver) == true;
        const bool isSameReceiver = AreSameReceiver(history, receiver);
        if (hasHistory == true)
        {
            float weight = ComputeShiftedWeight(scene, target, historyReceiver, history, isSameReceiver, queries);
            StreamCandidate(history.Sample,
                            weight,
                            history.CandidateCount,
                            CANDIDATE_HISTORY,
                            SampleGiRandom(launch, RESAMPLING_COIN_DIMENSION_FIRST + CANDIDATE_HISTORY),
                            result,
                            weightSum,
                            selected,
                            queries);
        }
        // Lines 4-5: w <- p_q(S) / p_q(S) with the source PDF of the sampled direction.
        float initialWeight = 0;
        if (initial.Sample.TriangleIndex != SCENE_INVALID_INDEX)
        {
            if (isfinite(initial.SourcePdf) == false || initial.SourcePdf <= 0)
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
            else
                initialWeight =
                    EvaluateTarget(target,
                                   initial.Sample,
                                   GetSurfaceHit(scene, initial.Sample.TriangleIndex, initial.Sample.Barycentric)) /
                    initial.SourcePdf;
        }
        StreamCandidate(initial.Sample,
                        initialWeight,
                        1,
                        CANDIDATE_INITIAL,
                        SampleGiRandom(launch, RESAMPLING_COIN_DIMENSION_FIRST + CANDIDATE_INITIAL),
                        result,
                        weightSum,
                        selected,
                        queries);
        // Line 6 with Algorithm 4's count Z: the history counts when its own visible point could have proposed z.
        uint normalizationCount = result.CandidateCount;
        if (IS_RESTIR_GI_UNBIASED == true && hasHistory == true && isSameReceiver == false &&
            selected == CANDIDATE_INITIAL &&
            CanGenerateSample(scene,
                              historyReceiver,
                              GetSurfaceHit(scene, result.Sample.TriangleIndex, result.Sample.Barycentric),
                              true,
                              queries) == false)
        {
            normalizationCount -= history.CandidateCount;
        }
        FinalizeReservoir(scene, target, weightSum, normalizationCount, RESTIR_GI_TEMPORAL_MAX_M, result, queries);
    }
    // Line 7; a camera miss stores an empty reservoir without a visible point.
    FinishReservoir(SECOND_BUFFER, launch, result, queries);
    return float3(0);
}
