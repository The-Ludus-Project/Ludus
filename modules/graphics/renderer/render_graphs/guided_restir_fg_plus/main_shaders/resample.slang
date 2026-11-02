#include "path_evaluation.slang"

// GRIS binary balance MIS (paper Eq. 8-10), with reverse support evaluation.
// A merge is two complete estimator samples, not two photon-count normalizations.
// Original photon UV/flux remain unchanged throughout camera replay and reconnection.
[noinline] FgPathReservoir ShiftFgPath(SceneContext scene, RayLaunch launch, FgPathReservoir source,
                            inout RayQueryContext queries, out float outJacobian, out float outTarget)
{
    FgPathReservoir shifted = source;
    outJacobian = 0;
    outTarget = 0;
    if (source.Photon.Triangle == FG_INVALID || source.Weight <= 0) return shifted;
    float3 emission;
    FgPoint receiver = TraceFgCamera(scene, launch, source.RandomKey, queries, emission);
    if (receiver.IsValid == false || receiver.Depth != GetFgCameraDepth(source)) return shifted;
    FgPoint oldReceiver = LoadFgReceiver(scene, source, queries);
    if (oldReceiver.IsValid == false) return shifted;
    outJacobian = 1;
    if ((source.Flags & FG_RADIANCE_FLAG) == 0)
    {
        SurfaceHit gather = GetSurfaceHit(scene, source.GatherTriangle, source.GatherBarycentric);
        outJacobian = ComputeFgReconnectJacobian(gather.PositionWorld - oldReceiver.Hit.PositionWorld,
                                                gather.PositionWorld - receiver.Hit.PositionWorld,
                                                gather.GeometricNormalWorld);
    }
    SetFgReceiver(receiver, launch, shifted);
    outTarget = ComputeFgLuminance(EvaluateFgPath(scene, shifted, receiver, true, queries));
    if (outJacobian <= 0 || isfinite(outJacobian) == false || outTarget <= 0)
    {
        outJacobian = 0;
        outTarget = 0;
    }
    return shifted;
}
FgPathReservoir MergeFgPathsBody(SceneContext scene, RayLaunch launch, FgPathReservoir own,
                             FgPathReservoir other, float coin, inout RayQueryContext queries)
{
    if (other.Confidence == 0 || own.Confidence == 0) return own;
    FgPoint ownReceiver = LoadFgReceiver(scene, own, queries);
    FgPoint otherReceiver = LoadFgReceiver(scene, other, queries);
    float ownTarget = ComputeFgLuminance(EvaluateFgPath(scene, own, ownReceiver, false, queries));
    float otherTarget = ComputeFgLuminance(EvaluateFgPath(scene, other, otherReceiver, false, queries));
    float otherJacobian;
    float shiftedOtherTarget;
    FgPathReservoir shiftedOther = ShiftFgPath(scene, launch, other, queries, otherJacobian, shiftedOtherTarget);
    RayLaunch otherLaunch = launch;
    uint pixel = GetFgDomainPixel(other);
    otherLaunch.PositionPixel = uint2(pixel % launch.ExtentPixel.x, pixel / launch.ExtentPixel.x);
    float ownJacobian;
    float shiftedOwnTarget;
    FgPathReservoir shiftedOwn = ShiftFgPath(scene, otherLaunch, own, queries, ownJacobian, shiftedOwnTarget);
    float ownConfidence = float(min(own.Confidence, FG_HISTORY_LIMIT));
    float otherConfidence = float(min(other.Confidence, FG_HISTORY_LIMIT));
    float ownMis = ComputeFgPairWeight(ownConfidence, otherConfidence, ownTarget, shiftedOwnTarget * ownJacobian);
    float otherMis = ComputeFgPairWeight(otherConfidence, ownConfidence, otherTarget, shiftedOtherTarget * otherJacobian);
    float ownWeight = ownMis * ownTarget * own.Weight;
    float otherWeight = otherMis * shiftedOtherTarget * other.Weight * otherJacobian;
    float sum = ownWeight + otherWeight;
    FgPathReservoir result = own;
    float target = ownTarget;
    if (otherWeight > 0 && coin * sum < otherWeight)
    {
        result = shiftedOther;
        target = shiftedOtherTarget;
    }
    result.Weight = target > 0 ? sum / target : 0;
    result.Confidence = min(own.Confidence + other.Confidence, FG_HISTORY_LIMIT);
    return result;
}
// Temporal and spatial passes share one merge implementation. A value/out query
// boundary preserves Slang's noinline behavior; see TraceFgCameraOutOfLine.
[noinline] FgPathReservoir MergeFgPathsOutOfLine(SceneContext scene, RayLaunch launch, FgPathReservoir own,
                                                FgPathReservoir other, float coin, RayQueryContext queriesIn,
                                                out RayQueryContext outQueries)
{
    RayQueryContext queries = queriesIn;
    FgPathReservoir result = MergeFgPathsBody(scene, launch, own, other, coin, queries);
    outQueries = queries;
    return result;
}
FgPathReservoir MergeFgPaths(SceneContext scene, RayLaunch launch, FgPathReservoir own,
                              FgPathReservoir other, float coin, inout RayQueryContext queries)
{
    RayQueryContext resultQueries;
    FgPathReservoir result = MergeFgPathsOutOfLine(scene, launch, own, other, coin, queries, resultQueries);
    queries = resultQueries;
    return result;
}

float3 ResamplePathTemporal(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgPathCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    uint pixel = GetFgPixel(launch);
    FgPathReservoir result = LoadFgPath(false, pixel);
    if (IsCurrentFgPath(result, scene, launch, queries) == true && IS_FG_TEMPORAL_ENABLED == true)
    {
        FgPathReservoir history = LoadFgPath(true, pixel);
        if (HasFgIdentity(history.Identity, scene, launch, true) == true && (history.Flags >> FG_FAILURE_SHIFT) == 0)
            result = MergeFgPaths(scene, launch, result, history, SampleFgRandom(launch, FG_RESAMPLE_FIRST_DIMENSION), queries);
    }
    FinishFgPath(true, scene, launch, result, queries);
    return float3(0);
}
float3 ResamplePathSpatial(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgPathCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    uint pixel = GetFgPixel(launch);
    FgPathReservoir result = LoadFgPath(false, pixel);
    if (IsCurrentFgPath(result, scene, launch, queries) == true && IS_FG_SPATIAL_ENABLED == true)
    {
        static const float NEIGHBOR_RADIUS_PIXEL = 8;
        float2 random = SampleFgRandomPair(launch.Seed, pixel, launch.SampleIndex, FG_RESAMPLE_FIRST_DIMENSION);
        int2 offsetPixel = int2(round((random * 2 - 1) * NEIGHBOR_RADIUS_PIXEL));
        int2 neighborPixel = int2(launch.PositionPixel) + offsetPixel;
        if (any(offsetPixel != 0) == true && all(neighborPixel >= 0) == true && all(neighborPixel < int2(launch.ExtentPixel)) == true)
        {
            uint neighbor = uint(neighborPixel.y) * launch.ExtentPixel.x + uint(neighborPixel.x);
            FgPathReservoir candidate = LoadFgPath(false, neighbor);
            if (IsCurrentFgPath(candidate, scene, launch, queries) == true)
                result = MergeFgPaths(scene, launch, result, candidate,
                                      SampleFgRandom(launch, FG_RESAMPLE_FIRST_DIMENSION + 2), queries);
        }
    }
    FinishFgPath(true, scene, launch, result, queries);
    return float3(0);
}
