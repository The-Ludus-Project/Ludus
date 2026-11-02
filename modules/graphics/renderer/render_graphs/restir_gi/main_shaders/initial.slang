#include "initial_sampling.slang"

// Pass 1 of 4 (scene compute), the paper's Algorithm 2: trace the camera ray to the visible point, sample a direction
// from the source distribution, trace it to the sample point and estimate the radiance L_o that the sample point
// scatters back (transport.slang). The record goes to the `samples` Buffer (GraphBuffer0). A sample ray that misses
// or reaches the environment records no sample: the shade pass estimates direct lighting, including the environment,
// at the visible point itself. The color is an intermediate; the shade pass replaces it.
// Queries: the camera ray, the sample ray, then one shadow ray and one continuation per path vertex.
float3 GenerateInitialSample(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRecordCapacity(launch, SAMPLE_RECORD_WORDS, 0) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    SampleRecord record = (SampleRecord)0;
    record.Sample.TriangleIndex = SCENE_INVALID_INDEX;
    record.SampleIndex = launch.SampleIndex;
    Receiver receiver;
    if (TryTraceReceiver(scene, launch, queries, receiver) == true)
    {
        record = MakeInitialGiSample(scene, launch, receiver, queries);
    }
    record.HasFailed = queries.HasFailed;
    record.FailureReason = queries.FailureReason;
    StoreSampleRecord(GetPixelIndex(launch), record);
    return float3(0);
}
