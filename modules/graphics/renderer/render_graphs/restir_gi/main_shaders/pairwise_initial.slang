#include "initial_sampling.slang"

// Fresh UCW in the source receiver's solid-angle measure, including empty outcomes.
// No temporal history is used; null initial endpoints still represent one proposal.
float3 InitializePairwiseGi(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRecordCapacity(launch, RESERVOIR_RECORD_WORDS, 0) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    Reservoir result = MakeEmptyReservoir(scene, launch);
    Receiver receiver;
    if (TryTraceReceiver(scene, launch, queries, receiver) == true)
    {
        SampleRecord initial = MakeInitialGiSample(scene, launch, receiver, queries);
        result.ReceiverTriangleIndex = receiver.Hit.TriangleIndex;
        result.ReceiverBarycentric = receiver.Hit.Barycentric;
        result.CandidateCount = 1;
        result.Sample = initial.Sample;
        if (initial.HasFailed == true)
            FailSceneQuery(queries, initial.FailureReason);
        else if (initial.Sample.TriangleIndex != SCENE_INVALID_INDEX &&
                 TryComputePairwiseContributionWeight(1, initial.SourcePdf, result.Weight) == false)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
    }
    FinishReservoir(FIRST_BUFFER, launch, result, queries);
    return float3(0);
}
