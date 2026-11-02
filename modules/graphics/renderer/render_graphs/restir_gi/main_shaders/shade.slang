#include "shading.slang"

// Pass 4 of 4 (scene compute): the final shading of section 4.2 from the pixel's spatial reservoir (`spatial`,
// GraphBuffer0). The visible point returns its emission, direct lighting with light and BSDF sampling combined by the
// power heuristic, and indirect lighting by the RIS estimator of Equation 8: the reservoir's weight W times
// f(z) = BSDF * cos * L_o of its sample. The reservoir's sample was visible when it was traced or merged (always for
// the unbiased variant), so no further ray connects it. A camera miss returns the environment.
// Queries: the camera ray, one BSDF ray and one shadow ray.
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
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, launch.CameraRay, hit);
    if (status == SCENE_QUERY_MISS)
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    ShadingPoint shadingPoint;
    if (status != SCENE_QUERY_SURFACE ||
        TryMakeShadingPoint(scene, hit, -launch.CameraRay.DirectionWorld, queries, shadingPoint) == false)
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
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}
