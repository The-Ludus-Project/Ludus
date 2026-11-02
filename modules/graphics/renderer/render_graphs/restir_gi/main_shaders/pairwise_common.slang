#pragma once
#include "reservoir.slang"

// Thanks to Hedstrom, Kettunen, Lin, Wyman and Li, "Stochastic Pairwise MIS for
// Unbiased Large-Kernel Reuse in Real-Time", CGF 45(2), 2026, equations 10/14/16/18/19:
// https://doi.org/10.1111/cgf.70391 . Scalar MIS and CDF sampling are engine shader
// APIs. GI owns solid-angle targets, inverse-proposal visibility and reconnection.
static const uint PAIRWISE_GI_OUTPUT_WORDS = RESERVOIR_RECORD_WORDS + 1;

bool HasGiPairwiseCapacity(RayLaunch launch)
{
    return HasRecordCapacity(launch, RESERVOIR_RECORD_WORDS, PAIRWISE_GI_OUTPUT_WORDS);
}
uint GetGiPairwiseCellOffset(RayLaunch launch)
{
    return launch.ExtentPixel.x * launch.ExtentPixel.y * RESERVOIR_RECORD_WORDS;
}
uint4 LoadGiPairwiseCell(RayLaunch launch, uint pixel)
{
    return GraphBuffer1[GetGiPairwiseCellOffset(launch) + pixel];
}
void StoreGiPairwiseCell(RayLaunch launch, uint pixel, uint4 value)
{
    GraphBuffer1[GetGiPairwiseCellOffset(launch) + pixel] = value;
}
struct GiPairwiseCellReader : IPairwiseCellReader
{
    uint Offset;
    float ReadPrefix(uint pixel)
    {
        return asfloat(GraphBuffer1[Offset + pixel].x);
    }
};
GiPairwiseCellReader MakeGiPairwiseCellReader(RayLaunch launch)
{
    GiPairwiseCellReader reader;
    reader.Offset = GetGiPairwiseCellOffset(launch);
    return reader;
}
// Native intersection barycentrics can lie a few FP32 ulps outside a triangle
// edge. Reconstructed receivers/endpoints use the same bounded projection. Leave
// larger/nonfinite errors unchanged so GetSurfaceHit rejects corrupted records.
static const float PAIRWISE_BARYCENTRIC_ROUNDING_TOLERANCE = 1e-6f;
float2 ClampGiPairwiseBarycentric(float2 barycentric)
{
    if (any(isfinite(barycentric) == false) == true ||
        any(barycentric < -PAIRWISE_BARYCENTRIC_ROUNDING_TOLERANCE) == true ||
        barycentric.x + barycentric.y > 1 + PAIRWISE_BARYCENTRIC_ROUNDING_TOLERANCE)
        return barycentric;
    float2 bounded = saturate(barycentric);
    bounded.y = min(bounded.y, 1 - bounded.x);
    return bounded;
}
[noinline] Receiver LoadGiPairwiseReceiver(SceneContext scene, RayLaunch launch, Reservoir source) {
    Receiver receiver =
        LoadReceiver(scene, source.ReceiverTriangleIndex, ClampGiPairwiseBarycentric(source.ReceiverBarycentric));
    if (receiver.IsValid == true)
        receiver.Hit.IncomingRay.DirectionWorld =
            launch.IsOrthographic == true ? launch.CameraRay.DirectionWorld
                                          : NormalizeOrZero(receiver.Hit.PositionWorld - launch.CameraPositionWorld);
    return receiver;
}
    // At a canonical sample, a neighbor's proxy is zero when its original hemisphere
    // and first-hit source distribution could not have proposed that point. Merely
    // testing the forward connection cannot establish this inverse support.
    [noinline] float EvaluateGiPairwiseTarget(TargetContext target, GiSample sample, SurfaceHit samplePoint)
{
    return EvaluateTarget(target, sample, samplePoint);
}
float ComputeGiPairwiseProxy(SceneContext scene,
                             TargetContext source,
                             Receiver destination,
                             GiSample sample,
                             SurfaceHit samplePoint,
                             inout RayQueryContext queries)
{
    float jacobian =
        ComputeReconnectionJacobian(source.Point.Hit.PositionWorld, destination.Hit.PositionWorld, samplePoint);
    if (jacobian <= 0 || CanGenerateSample(scene, source.Point, samplePoint, true, queries) == false)
        return 0;
    float proxy = 0;
    if (TryComputePairwiseShiftedTarget(EvaluateGiPairwiseTarget(source, sample, samplePoint), jacobian, proxy) ==
        false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
    return proxy;
}

// Keep repeated endpoint reconstruction below the shared shader artifact budget.
[noinline] SurfaceHit LoadGiPairwiseSamplePoint(SceneContext scene, GiSample sample) {
    return GetSurfaceHit(scene, sample.TriangleIndex, ClampGiPairwiseBarycentric(sample.Barycentric));
}
