#include "shading.slang"

// Pass 4 of 4 (scene compute): the final shading of section 4.2 from the pixel's spatial reservoir (`spatial`,
// GraphBuffer0). The visible point returns its emission, direct lighting with light and BSDF sampling combined by the
// power heuristic, and indirect lighting by the RIS estimator of Equation 8: the reservoir's weight W times
// f(z) = BSDF * cos * L_o of its sample. The reservoir's sample was visible when it was traced or merged (always for
// the unbiased variant), so no further ray connects it. A camera miss returns the environment. The visible point lies
// past any smooth glass the camera sees through (TryTraceReceiver), weighted by the glass chain.
// Queries: the camera ray, the glass chain, one BSDF ray and one shadow ray.
float3 ShadeRestirGi(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRecordCapacity(launch, RESERVOIR_RECORD_WORDS, 0) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    Reservoir reservoir = LoadReservoir(FIRST_BUFFER, GetPixelIndex(launch));
    if (IsWrittenThisFrame(reservoir, scene, launch, RESTIR_GI_SPATIAL_MAX_M) == false)
    {
        FailFromStoredRecord(launch, reservoir.SampleIndex, reservoir.HasFailed, reservoir.FailureReason, queries);
        return float3(0);
    }
    Receiver receiver;
    if (TryTraceReceiver(scene, launch, queries, receiver) == false)
        return queries.HasFailed == false ? receiver.Radiance : float3(0);
    SurfaceHit hit = receiver.Hit;
    ShadingPoint shadingPoint;
    if (TryMakeShadingPoint(scene, hit, -hit.IncomingRay.DirectionWorld, queries, shadingPoint) == false)
        return float3(0);
    if (reservoir.ReceiverTriangleIndex != hit.TriangleIndex ||
        all(reservoir.ReceiverBarycentric == hit.Barycentric) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    float3 radiance = EvaluateEmission(scene, hit, shadingPoint.ViewDirectionWorld);
    radiance += EstimateDirectLighting(scene, launch, shadingPoint, SHADE_DIRECT_DIMENSION_FIRST, queries);
    if (reservoir.Sample.TriangleIndex != SCENE_INVALID_INDEX && reservoir.Weight > 0)
    {
        radiance += EvaluateGiIndirectContribution(scene, shadingPoint, reservoir.Sample, queries) * reservoir.Weight;
    }
    radiance = receiver.Radiance + receiver.Throughput * radiance;
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}
