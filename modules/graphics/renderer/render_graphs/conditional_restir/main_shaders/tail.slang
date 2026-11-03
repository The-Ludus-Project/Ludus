#pragma once
#include "reservoir.slang"

// Thanks to Kettunen et al., "Conditional Resampled Importance Sampling and ReSTIR", SIGGRAPH Asia
// 2023, sections 4-7, https://doi.org/10.1145/3610548.3618245 . We adapt suffix conditioning and
// independent final gathering to a common marked area domain instead of the prototype's hybrid shift.
// A uniform length selects one term of the finite scattering series. A suffix's first point and
// first outgoing proposal do not depend on the supporting prefix; later edges use ordinary BSDF
// transport. Only the first suffix BSDF changes on reconnection. Terminal NEE owns emission at the
// selected length, so intermediate emitter hits must NOT also add emission. No denoising is used.

static const uint TAIL_DIMENSION_FIRST = 256;
static const uint TAIL_TERMINAL_DIMENSION_FIRST = 320;
static const uint TAIL_TERMINAL_MATERIAL_EVENT = 32;
static const uint BARYCENTRIC_DIMENSION_OFFSET = 3;
static const float SPHERE_SOLID_ANGLE = 4 * PI;

float2 SampleAreaBarycentric(RayLaunch launch, uint dimension)
{
    float root = sqrt(SamplePassRandom(launch, dimension));
    float split = SamplePassRandom(launch, dimension + 1);
    return float2(root * (1 - split), root * split);
}

/// Full-support mesh emission proposal. Degenerate draws and empty light sets contribute zero.
bool TrySampleMeshEndpoint(SceneContext scene,
                           RayLaunch launch,
                           uint dimension,
                           inout RayQueryContext queries,
                           out SurfaceHit outEndpoint,
                           out float outInverseDensityArea)
{
    outEndpoint = (SurfaceHit)0;
    outInverseDensityArea = 0;
    uint emitterCount = GetEmitterCount(scene);
    if (emitterCount == 0)
        return false;
    uint emitter = SamplePassIndex(launch, dimension, emitterCount, queries);
    uint triangleCount = uint(GetEmitter(scene, emitter).y);
    if (triangleCount == 0)
        return false;
    TriangleData triangle =
        GetEmitterTriangle(scene, emitter, SamplePassIndex(launch, dimension + 1, triangleCount, queries));
    if (triangle.IsValid == false || triangle.AreaWorld <= 0)
        return false;
    outEndpoint = GetSurfaceHit(scene, triangle.TriangleIndex, SampleAreaBarycentric(launch, dimension + 2));
    outInverseDensityArea = float(emitterCount) * float(triangleCount) * triangle.AreaWorld;
    return outEndpoint.IsValid;
}

/// Terminal light edge without its receiver's BSDF. The direction, cosine, light density and
/// visibility depend only on this suffix, so they can be cached before the gather prefix exists.
float3 SampleTerminalCoefficient(SceneContext scene,
                                 RayLaunch launch,
                                 SurfaceHit receiver,
                                 inout RayQueryContext queries,
                                 out float3 outDirectionWorld)
{
    outDirectionWorld = float3(0);
    SurfaceHit endpoint;
    float inverseDensityArea;
    if (TrySampleMeshEndpoint(scene, launch, TAIL_TERMINAL_DIMENSION_FIRST, queries, endpoint, inverseDensityArea) ==
        false)
        return float3(0);
    float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
    float distanceSquaredWorld = dot(offsetWorld, offsetWorld);
    if (distanceSquaredWorld <= 0)
        return float3(0);
    outDirectionWorld = offsetWorld * rsqrt(distanceSquaredWorld);
    MaterialSample material = EvaluateMaterial(scene, receiver);
    if (material.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    float cosineReceiver = max(dot(NormalizeOrZero(material.ShadingNormalWorld), outDirectionWorld), 0);
    float cosineEmitter = abs(dot(NormalizeOrZero(endpoint.GeometricNormalWorld), -outDirectionWorld));
    float3 coefficient = EvaluateEmission(scene, endpoint, -outDirectionWorld) *
                         (cosineReceiver * cosineEmitter * inverseDensityArea / distanceSquaredWorld);
    if (any(coefficient > 0) == false)
        return float3(0);
    LightSample light = (LightSample)0;
    light.TriangleIndex = endpoint.TriangleIndex;
    light.Barycentric = endpoint.Barycentric;
    light.PositionWorld = endpoint.PositionWorld;
    light.DistanceWorld = sqrt(distanceSquaredWorld);
    light.DirectionWorld = outDirectionWorld;
    light.HasFiniteEndpoint = true;
    light.HasSample = true;
    return TestVisibility(scene, queries, receiver, light) == SceneQuery::Miss ? coefficient : float3(0);
}

/// Generate a tail of exactly suffixBounces scattering vertices. The first vertex is sampled
/// uniformly among all scene triangles and in its area. Its scattering is deferred to the gather.
/// A zero tail remains a legitimate marked sample; visibility or BSDF rejection cannot remove support.
Reservoir GenerateScatteringSuffix(SceneContext scene,
                                   RayLaunch launch,
                                   uint suffixBounces,
                                   inout RayQueryContext queries,
                                   out float outInverseDensityArea)
{
    Reservoir result = MakeEmptyReservoir(scene, launch);
    result.SuffixBounces = suffixBounces;
    outInverseDensityArea = 0;
    uint triangleCount = GetSceneInfo(scene).TriangleCount;
    if (triangleCount == 0)
        return result;
    TriangleData triangle = GetTriangle(scene, SamplePassIndex(launch, TAIL_DIMENSION_FIRST, triangleCount, queries));
    if (triangle.IsValid == false || triangle.AreaWorld <= 0)
        return result;
    result.TriangleIndex = triangle.TriangleIndex;
    result.Barycentric = SampleAreaBarycentric(launch, TAIL_DIMENSION_FIRST + 1);
    outInverseDensityArea = float(triangleCount) * triangle.AreaWorld;
    SurfaceHit first = GetSurfaceHit(scene, result.TriangleIndex, result.Barycentric);
    if (suffixBounces == 1)
    {
        result.TailCoefficient = SampleTerminalCoefficient(scene, launch, first, queries, result.DirectionWorld);
        return result;
    }
    // A sphere, rather than a view-conditioned BSDF, keeps the first outgoing direction independent
    // of ALL supporting and integration prefixes, including their shading-normal hemispheres.
    float z = 1 - 2 * SamplePassRandom(launch, TAIL_DIMENSION_FIRST + BARYCENTRIC_DIMENSION_OFFSET);
    const uint AZIMUTH_DIMENSION_OFFSET = 4;
    float angle = 2 * PI * SamplePassRandom(launch, TAIL_DIMENSION_FIRST + AZIMUTH_DIMENSION_OFFSET);
    float radius = sqrt(max(1 - z * z, 0));
    result.DirectionWorld = float3(radius * cos(angle), radius * sin(angle), z);
    MaterialSample firstMaterial = EvaluateMaterial(scene, first);
    if (firstMaterial.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return result;
    }
    float3 throughput = float3(SPHERE_SOLID_ANGLE *
                               max(dot(NormalizeOrZero(firstMaterial.ShadingNormalWorld), result.DirectionWorld), 0));
    SurfaceHit vertex = first;
    float3 directionWorld = result.DirectionWorld;
    [loop] for (uint depth = 1; depth < suffixBounces; ++depth)
    {
        // At most six continuation queries, plus the camera and supporting-prefix queries: eight.
        if (any(throughput > 0) == false)
            return result;
        Ray ray = SpawnSurfaceRay(vertex, directionWorld, MAXIMUM_TRACE_DISTANCE_WORLD);
        if (TraceClosest(scene, queries, ray, vertex) != SceneQuery::Surface)
            return result;
        MaterialSample material = EvaluateMaterial(scene, vertex);
        if (material.IsValid == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
            return result;
        }
        float3 viewWorld = -directionWorld;
        if (depth + 1 == suffixBounces)
        {
            float3 terminalDirectionWorld;
            float3 terminal = SampleTerminalCoefficient(scene, launch, vertex, queries, terminalDirectionWorld);
            if (any(terminal > 0) == false)
                return result;
            float3 bsdf = EvaluateSuffixBsdf(material,
                                             viewWorld,
                                             terminalDirectionWorld,
                                             MakeMaterialRandom(launch, TAIL_TERMINAL_MATERIAL_EVENT),
                                             true);
            result.TailCoefficient = throughput * bsdf * terminal;
            return result;
        }
        const uint CONTINUATION_DIMENSION_OFFSET = 5;
        uint dimension = TAIL_DIMENSION_FIRST + CONTINUATION_DIMENSION_OFFSET + depth * TAIL_VERTEX_DIMENSIONS;
        float3 random = float3(SamplePassRandom(launch, dimension),
                               SamplePassRandom(launch, dimension + 1),
                               SamplePassRandom(launch, dimension + 2));
        StandardBsdfTransportSample step =
            SampleSuffixBsdf(material,
                             viewWorld,
                             random,
                             MakeMaterialRandom(launch, TAIL_MATERIAL_EVENT_FIRST + depth));
        if (all(isfinite(step.Weight)) == false || any(step.Weight < 0) == true ||
            all(isfinite(step.DirectionWorld)) == false || isfinite(step.Pdf) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            return result;
        }
        if (step.HasSample == false || any(step.Weight > 0) == false)
            return result;
        throughput *= step.Weight;
        directionWorld = NormalizeOrZero(step.DirectionWorld);
        if (all(isfinite(throughput)) == false || all(directionWorld == 0) == true)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            return result;
        }
    }
    return result;
}
