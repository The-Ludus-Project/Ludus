#pragma once
#include "reservoir.slang"

// Ouyang et al., Algorithm 2 (attribution in reservoir.slang). The original initial pass and the production
// kernel share this source distribution and path estimator. The caller has already traced the visible point;
// the production path retains the returned sample locally instead of publishing an intermediate record.
SampleRecord MakeInitialGiSample(SceneContext scene, RayLaunch launch, Receiver receiver, inout RayQueryContext queries)
{
    SampleRecord record = (SampleRecord)0;
    record.Sample.TriangleIndex = SCENE_INVALID_INDEX;
    record.SampleIndex = launch.SampleIndex;
    float3 directionWorld;
    float sourcePdf = SampleSourceDirection(receiver,
                                            float2(SampleGiRandom(launch, INITIAL_SOURCE_DIMENSION_FIRST),
                                                   SampleGiRandom(launch, INITIAL_SOURCE_DIMENSION_FIRST + 1)),
                                            directionWorld);
    SurfaceHit samplePoint;
    if (sourcePdf > 0 &&
        TraceClosest(scene,
                     queries,
                     SpawnSurfaceRay(receiver.Hit, directionWorld, TRANSPORT_MAXIMUM_TRACE_DISTANCE_WORLD),
                     samplePoint) == SCENE_QUERY_SURFACE)
    {
        record.Sample.TriangleIndex = samplePoint.TriangleIndex;
        record.Sample.Barycentric = samplePoint.Barycentric;
        record.Sample.RadianceOut =
            EstimateRadianceOut(scene, launch, samplePoint, INITIAL_PATH_DIMENSION_FIRST, queries);
        record.SourcePdf = sourcePdf;
        if (all(isfinite(record.Sample.RadianceOut)) == false || any(record.Sample.RadianceOut < 0) == true)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    }
    record.HasFailed = queries.HasFailed;
    record.FailureReason = queries.FailureReason;
    return record;
}
