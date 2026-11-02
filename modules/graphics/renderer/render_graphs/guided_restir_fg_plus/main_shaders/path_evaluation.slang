#pragma once
#include "storage.slang"

static const uint FG_CAMERA_DEPTH_SHIFT = 8;
static const uint FG_FAILURE_SHIFT = 16;
uint GetFgCameraDepth(FgPathReservoir reservoir) { return reservoir.CameraDepth & FG_DEPTH_MASK; }
uint GetFgDomainPixel(FgPathReservoir reservoir) { return reservoir.CameraDepth >> FG_CAMERA_DEPTH_SHIFT; }
void SetFgReceiver(FgPoint point, RayLaunch launch, inout FgPathReservoir inoutReservoir)
{
    inoutReservoir.ReceiverTriangle = point.Hit.TriangleIndex;
    inoutReservoir.ReceiverBarycentric = point.Hit.Barycentric;
    inoutReservoir.ViewDirectionWorld = point.ViewDirectionWorld;
    inoutReservoir.Throughput = point.Throughput;
    inoutReservoir.CameraDepth = point.Depth | (GetFgPixel(launch) << FG_CAMERA_DEPTH_SHIFT);
}
[noinline] FgPoint LoadFgReceiver(SceneContext scene, FgPathReservoir reservoir, inout RayQueryContext queries)
{
    FgPoint result = (FgPoint)0;
    if (reservoir.ReceiverTriangle == FG_INVALID) return result;
    SurfaceHit hit = GetSurfaceHit(scene, reservoir.ReceiverTriangle, reservoir.ReceiverBarycentric);
    if (TryMakeFgPoint(scene, hit, reservoir.ViewDirectionWorld, queries, result) == true)
    {
        result.Throughput = reservoir.Throughput;
        result.Depth = GetFgCameraDepth(reservoir);
    }
    return result;
}
[noinline] bool IsFgPhotonNear(SceneContext scene, FgPhoton photon, FgPoint point)
{
    if (photon.Triangle == FG_INVALID || point.IsValid == false) return false;
    SurfaceHit hit = GetSurfaceHit(scene, photon.Triangle, photon.Barycentric);
    float3 offsetWorld = hit.PositionWorld - point.Hit.PositionWorld;
    return hit.IsValid == true && dot(offsetWorld, offsetWorld) < FG_GATHER_RADIUS_WORLD * FG_GATHER_RADIUS_WORLD &&
           abs(dot(offsetWorld, point.Hit.GeometricNormalWorld)) <= FG_PLANE_RADIUS_FRACTION * FG_GATHER_RADIUS_WORLD &&
           dot(hit.GeometricNormalWorld, point.Hit.GeometricNormalWorld) >= FG_NORMAL_AGREEMENT &&
           dot(point.Material.ShadingNormalWorld, photon.IncomingWorld) > 0;
}
float3 EvaluateFgPathBody(SceneContext scene, FgPathReservoir reservoir, FgPoint receiver,
                       bool shouldTestVisibility, inout RayQueryContext queries)
{
    if (receiver.IsValid == false || reservoir.Photon.Triangle == FG_INVALID) return float3(0);
    if ((reservoir.Flags & FG_RADIANCE_FLAG) != 0)
    {
        if (receiver.Depth == 0 || IsFgPhotonNear(scene, reservoir.Photon, receiver) == false) return float3(0);
        return receiver.Throughput * EvaluateFgPhotonDensity(EvaluateFgBsdf(receiver, reservoir.Photon.IncomingWorld), reservoir.Photon.Flux);
    }
    SurfaceHit gatherHit = GetSurfaceHit(scene, reservoir.GatherTriangle, reservoir.GatherBarycentric);
    float3 offsetWorld = gatherHit.PositionWorld - receiver.Hit.PositionWorld;
    float squaredDistance = dot(offsetWorld, offsetWorld);
    if (gatherHit.IsValid == false || squaredDistance <= 0) return float3(0);
    float3 directionWorld = offsetWorld * rsqrt(squaredDistance);
    FgPoint gatherPoint;
    if (TryMakeFgPoint(scene, gatherHit, -directionWorld, queries, gatherPoint) == false ||
        IsFgDiffuse(gatherPoint.Material) == false || IsFgPhotonNear(scene, reservoir.Photon, gatherPoint) == false)
        return float3(0);
    float cosine = dot(receiver.Material.ShadingNormalWorld, directionWorld);
    if (cosine <= 0 || dot(gatherHit.GeometricNormalWorld, -directionWorld) <= 0) return float3(0);
    if (shouldTestVisibility == true)
    {
        LightSample connection = (LightSample)0;
        connection.DirectionWorld = directionWorld;
        connection.PositionWorld = gatherHit.PositionWorld;
        connection.DistanceWorld = sqrt(squaredDistance);
        connection.TriangleIndex = gatherHit.TriangleIndex;
        connection.HasFiniteEndpoint = true;
        connection.HasSample = true;
        if (TestVisibility(scene, queries, receiver.Hit, connection) != SCENE_QUERY_MISS) return float3(0);
    }
    return receiver.Throughput * EvaluateFgBsdf(receiver, directionWorld) * cosine *
           EvaluateFgPhotonDensity(EvaluateFgBsdf(gatherPoint, reservoir.Photon.IncomingWorld), reservoir.Photon.Flux);
}
// Same value/out boundary as TraceFgCameraOutOfLine: all resampling callers share
// one density/visibility evaluator instead of expanding it at every shift site.
[noinline] float3 EvaluateFgPathOutOfLine(SceneContext scene, FgPathReservoir reservoir, FgPoint receiver,
                                          bool shouldTestVisibility, RayQueryContext queriesIn,
                                          out RayQueryContext outQueries)
{
    RayQueryContext queries = queriesIn;
    float3 value = EvaluateFgPathBody(scene, reservoir, receiver, shouldTestVisibility, queries);
    outQueries = queries;
    return value;
}
float3 EvaluateFgPath(SceneContext scene, FgPathReservoir reservoir, FgPoint receiver,
                       bool shouldTestVisibility, inout RayQueryContext queries)
{
    RayQueryContext resultQueries;
    float3 value = EvaluateFgPathOutOfLine(scene, reservoir, receiver, shouldTestVisibility, queries, resultQueries);
    queries = resultQueries;
    return value;
}

void FinishFgPath(bool isSecond, SceneContext scene, RayLaunch launch,
                   FgPathReservoir reservoir, inout RayQueryContext queries)
{
    if (isfinite(reservoir.Weight) == false || reservoir.Weight < 0 || all(isfinite(reservoir.Throughput)) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    if (queries.HasFailed == true)
    {
        reservoir.Weight = 0;
        reservoir.Flags = (queries.FailureReason + 1) << FG_FAILURE_SHIFT;
    }
    reservoir.Identity = MakeFgIdentity(scene, launch);
    StoreFgPath(isSecond, GetFgPixel(launch), reservoir);
}
bool IsCurrentFgPath(FgPathReservoir reservoir, SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgIdentity(reservoir.Identity, scene, launch, false) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return false;
    }
    if ((reservoir.Flags >> FG_FAILURE_SHIFT) != 0)
    {
        FailSceneQuery(queries, (reservoir.Flags >> FG_FAILURE_SHIFT) - 1);
        return false;
    }
    return true;
}
void RecordFgGuiding(FgPhoton photon, float3 contribution)
{
    if (photon.Triangle == FG_INVALID || photon.Light >= FG_MAXIMUM_LIGHTS || photon.Cell >= FG_DIRECTION_CELLS) return;
    float luminance = ComputeFgLuminance(contribution);
    if (luminance <= 0 || isfinite(luminance) == false) return;
    uint discrete = uint(clamp(luminance * FG_GUIDING_DISCRETIZATION, 1, float(FG_MAXIMUM_GUIDING_CONTRIBUTION)));
    uint previous;
    InterlockedAdd(GraphBuffer0[GetFgLightAddress(photon.Light) + 1 + photon.Cell].y, discrete, previous);
}
