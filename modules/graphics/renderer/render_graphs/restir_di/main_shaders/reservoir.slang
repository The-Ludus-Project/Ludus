#pragma once
#include "emitter_sampling.slang"

// Thanks to Bitterli, Wyman, Pharr, Shirley, Lefohn and Jarosz,
// "Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting",
// ACM TOG 39(4), 2020, and supplemental reservoir-combination algorithms:
// https://doi.org/10.1145/3386569.3392481
// https://benedikt-bitterli.me/restir/bitterli20restir-supplemental.pdf
// Adapted streaming RIS and receiver-relative reservoir combination. This example
// uses a strictly positive, visibility-free area target, finite history, and
// conservative same-pixel temporal validation instead of motion reprojection.
static const uint RESERVOIR_WORD_COUNT = 4;
static const uint RESERVOIR_LAYOUT_TAG = 0x52444931u;
static const float MINIMUM_TARGET_RADIANCE_PER_AREA = 0.000001f;
static const uint INITIAL_PASS_INDEX = 0;
static const uint TEMPORAL_PASS_INDEX = 1;
static const uint SPATIAL_PASS_INDEX = 2;

// Four uint4 words per pixel, no native struct-layout or float quantization dependency.
// Weight is finalized W, not the streaming weight sum. Count is effective M.
struct Reservoir
{
    uint TriangleIndex;
    float2 Barycentric;
    float Weight;
    uint CandidateCount;
    uint ReceiverTriangleIndex;
    float2 ReceiverBarycentric;
    float3 ViewDirectionWorld;
    uint LayoutTag;
    uint2 SceneGeneration;
    uint2 ExtentPixel;
};

bool HasReservoirCapacity(RayLaunch launch)
{
    uint historyWords;
    uint historyStride;
    uint scratchWords;
    uint scratchStride;
    GraphBuffer0.GetDimensions(historyWords, historyStride);
    GraphBuffer1.GetDimensions(scratchWords, scratchStride);
    uint capacity = min(historyWords, scratchWords) / RESERVOIR_WORD_COUNT;
    return launch.ExtentPixel.y > 0 && launch.ExtentPixel.x > 0 &&
           launch.ExtentPixel.x <= capacity / launch.ExtentPixel.y;
}

Reservoir MakeReceiverReservoir(SceneContext scene, RayLaunch launch, SurfaceHit hit)
{
    Reservoir result = (Reservoir)0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    result.ReceiverTriangleIndex = hit.TriangleIndex;
    result.ReceiverBarycentric = hit.Barycentric;
    result.ViewDirectionWorld = -normalize(hit.IncomingRay.DirectionWorld);
    result.LayoutTag = RESERVOIR_LAYOUT_TAG;
    result.SceneGeneration = scene.Generation;
    result.ExtentPixel = launch.ExtentPixel;
    return result;
}

Reservoir LoadReservoir(uint pixel, bool isHistory)
{
    uint address = pixel * RESERVOIR_WORD_COUNT;
    uint4 sample = isHistory == true ? GraphBuffer0[address] : GraphBuffer1[address];
    uint4 receiver = isHistory == true ? GraphBuffer0[address + 1] : GraphBuffer1[address + 1];
    uint4 view = isHistory == true ? GraphBuffer0[address + 2] : GraphBuffer1[address + 2];
    const uint IDENTITY_WORD = 3;
    uint4 identity = isHistory == true ? GraphBuffer0[address + IDENTITY_WORD] : GraphBuffer1[address + IDENTITY_WORD];
    Reservoir result;
    result.TriangleIndex = sample.x;
    result.Barycentric = asfloat(sample.yz);
    result.Weight = asfloat(sample.w);
    result.CandidateCount = receiver.x;
    result.ReceiverTriangleIndex = receiver.y;
    result.ReceiverBarycentric = asfloat(receiver.zw);
    result.ViewDirectionWorld = asfloat(view.xyz);
    result.LayoutTag = view.w;
    result.SceneGeneration = identity.xy;
    result.ExtentPixel = identity.zw;
    return result;
}

void StoreReservoir(uint pixel, bool isHistory, Reservoir reservoir)
{
    uint address = pixel * RESERVOIR_WORD_COUNT;
    uint4 sample = uint4(reservoir.TriangleIndex, asuint(reservoir.Barycentric), asuint(reservoir.Weight));
    uint4 receiver =
        uint4(reservoir.CandidateCount, reservoir.ReceiverTriangleIndex, asuint(reservoir.ReceiverBarycentric));
    uint4 view = uint4(asuint(reservoir.ViewDirectionWorld), reservoir.LayoutTag);
    uint4 identity = uint4(reservoir.SceneGeneration, reservoir.ExtentPixel);
    const uint IDENTITY_WORD = 3;
    if (isHistory == true)
    {
        GraphBuffer0[address] = sample;
        GraphBuffer0[address + 1] = receiver;
        GraphBuffer0[address + 2] = view;
        GraphBuffer0[address + IDENTITY_WORD] = identity;
    }
    else
    {
        GraphBuffer1[address] = sample;
        GraphBuffer1[address + 1] = receiver;
        GraphBuffer1[address + 2] = view;
        GraphBuffer1[address + IDENTITY_WORD] = identity;
    }
}

bool HasMatchingIdentity(Reservoir source, SceneContext scene, RayLaunch launch)
{
    return source.LayoutTag == RESERVOIR_LAYOUT_TAG && all(source.SceneGeneration == scene.Generation) == true &&
           all(source.ExtentPixel == launch.ExtentPixel) == true &&
           source.CandidateCount <= RESTIR_HISTORY_CANDIDATES && isfinite(source.Weight) == true && source.Weight >= 0;
}

bool HasMatchingTemporalReceiver(Reservoir source, Reservoir current)
{
    // Exact receiver/view matches deliberately reject camera motion and disocclusion.
    // Reusing launch.SampleIndex alone would accept stale buffers after job replacement.
    return source.ReceiverTriangleIndex == current.ReceiverTriangleIndex &&
           all(source.ReceiverBarycentric == current.ReceiverBarycentric) == true &&
           all(source.ViewDirectionWorld == current.ViewDirectionWorld) == true;
}

// Thanks to the Slang authors, core module noinline attribute reference:
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html
// Keep repeated BSDF evaluation below the existing generated-artifact size limit.
[noinline] float3
EvaluateSampleContributionArea(SceneContext scene, SurfaceHit receiver, uint triangleIndex, float2 barycentric) {
    SurfaceHit endpoint = GetSurfaceHit(scene, triangleIndex, barycentric);
    if (endpoint.IsValid == false)
    {
        return float3(0);
    }
    float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
    float distanceSquaredWorld = dot(offsetWorld, offsetWorld);
    if (distanceSquaredWorld <= 0)
    {
        return float3(0);
    }
    float3 directionWorld = offsetWorld * rsqrt(distanceSquaredWorld);
    MaterialSample material = EvaluateMaterial(scene, receiver);
    if (material.IsValid == false)
    {
        return float3(0);
    }
    float cosSurface = max(dot(normalize(material.ShadingNormalWorld), directionWorld), 0);
    float cosEmitter = abs(dot(normalize(endpoint.GeometricNormalWorld), -directionWorld));
    float3 bsdfValue;
    float bsdfPdfSolidAngle;
    EvaluateStandardBsdf(material,
                         -normalize(receiver.IncomingRay.DirectionWorld),
                         directionWorld,
                         bsdfValue,
                         bsdfPdfSolidAngle);
    // EvaluateEmission applies the authored emitter-sidedness contract.
    return EvaluateEmission(scene, endpoint, -directionWorld) * bsdfValue *
           (cosSurface * cosEmitter / distanceSquaredWorld);
}

float EvaluateSampleTarget(SceneContext scene, SurfaceHit receiver, uint triangleIndex, float2 barycentric)
{
    float3 contribution = EvaluateSampleContributionArea(scene, receiver, triangleIndex, barycentric);
    const float3 LUMINANCE_WEIGHTS = float3(0.2126f, 0.7152f, 0.0722f);
    // Positive support, including zero-contribution proposals, avoids discarding
    // samples that may contribute at a different receiving surface during reuse.
    return max(dot(contribution, LUMINANCE_WEIGHTS), MINIMUM_TARGET_RADIANCE_PER_AREA);
}

// Reserve a distinct addressed-random scope for each reuse pass. Initial RIS
// uses the scene API scope (pixel, 0); reuse must not repeat its selection coins.
float SampleReuseRandom(RayLaunch launch, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    RandomAddress address = MakeRandomAddress(uint2(pixel, GetGraphPassIndex()), launch.SampleIndex, dimension);
    return SampleRandomFloat01(launch.Seed, address);
}

void StreamReservoir(uint triangleIndex,
                     float2 barycentric,
                     float weight,
                     uint candidateCount,
                     float randomValue,
                     inout Reservoir inoutReservoir,
                     inout float inoutWeightSum,
                     inout RayQueryContext queries)
{
    // Zero-weight proposals still contribute to M.
    inoutReservoir.CandidateCount += candidateCount;
    if (isfinite(weight) == false || weight < 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        return;
    }
    if (weight == 0)
    {
        return;
    }
    inoutWeightSum += weight;
    if (isfinite(inoutWeightSum) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        return;
    }
    if (randomValue < weight / inoutWeightSum)
    {
        inoutReservoir.TriangleIndex = triangleIndex;
        inoutReservoir.Barycentric = barycentric;
    }
}

void FinalizeReservoir(SceneContext scene,
                       SurfaceHit receiver,
                       float weightSum,
                       inout Reservoir inoutReservoir,
                       inout RayQueryContext queries)
{
    if (inoutReservoir.CandidateCount == 0 || inoutReservoir.TriangleIndex == SCENE_INVALID_INDEX)
    {
        inoutReservoir.Weight = 0;
        return;
    }
    float target = EvaluateSampleTarget(scene, receiver, inoutReservoir.TriangleIndex, inoutReservoir.Barycentric);
    inoutReservoir.Weight = weightSum / (float(inoutReservoir.CandidateCount) * target);
    // Cap represented history AFTER normalization; W must retain its finalized scale.
    inoutReservoir.CandidateCount = min(inoutReservoir.CandidateCount, RESTIR_HISTORY_CANDIDATES);
    if (isfinite(inoutReservoir.Weight) == false || inoutReservoir.Weight < 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
    }
}

void MergeReservoir(SceneContext scene,
                    SurfaceHit receiver,
                    Reservoir source,
                    float randomValue,
                    inout Reservoir inoutReservoir,
                    inout float inoutWeightSum,
                    inout RayQueryContext queries)
{
    float weight = 0;
    if (source.TriangleIndex != SCENE_INVALID_INDEX && source.CandidateCount > 0)
    {
        weight = EvaluateSampleTarget(scene, receiver, source.TriangleIndex, source.Barycentric) * source.Weight *
                 float(source.CandidateCount);
    }
    StreamReservoir(source.TriangleIndex,
                    source.Barycentric,
                    weight,
                    source.CandidateCount,
                    randomValue,
                    inoutReservoir,
                    inoutWeightSum,
                    queries);
}

bool CanReuseSpatialReceiver(SceneContext scene, SurfaceHit receiver, Reservoir source)
{
    SurfaceHit neighbor = GetSurfaceHit(scene, source.ReceiverTriangleIndex, source.ReceiverBarycentric);
    if (neighbor.IsValid == false || neighbor.MaterialIndex != receiver.MaterialIndex)
    {
        return false;
    }
    float3 differenceWorld = neighbor.PositionWorld - receiver.PositionWorld;
    return dot(normalize(neighbor.ShadingNormalWorld), normalize(receiver.ShadingNormalWorld)) >=
               RESTIR_SPATIAL_NORMAL_COSINE &&
           dot(differenceWorld, differenceWorld) <= RESTIR_SPATIAL_DISTANCE_WORLD * RESTIR_SPATIAL_DISTANCE_WORLD;
}

float3 ShadeReservoir(SceneContext scene, SurfaceHit receiver, Reservoir reservoir, inout RayQueryContext queries)
{
    if (reservoir.TriangleIndex == SCENE_INVALID_INDEX || reservoir.Weight <= 0)
    {
        return float3(0);
    }
    SurfaceHit endpoint = GetSurfaceHit(scene, reservoir.TriangleIndex, reservoir.Barycentric);
    if (endpoint.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
    float distanceWorld = length(offsetWorld);
    if (distanceWorld <= 0)
    {
        return float3(0);
    }
    LightSample lightSample = (LightSample)0;
    lightSample.TriangleIndex = reservoir.TriangleIndex;
    lightSample.Barycentric = reservoir.Barycentric;
    lightSample.PositionWorld = endpoint.PositionWorld;
    lightSample.DirectionWorld = offsetWorld / distanceWorld;
    lightSample.DistanceWorld = distanceWorld;
    lightSample.HasFiniteEndpoint = true;
    lightSample.HasSample = true;
    float3 contribution =
        EvaluateSampleContributionArea(scene, receiver, reservoir.TriangleIndex, reservoir.Barycentric);
    if (any(contribution > 0) == false)
    {
        return float3(0);
    }
    if (TestVisibility(scene, queries, receiver, lightSample) != SCENE_QUERY_MISS)
    {
        return float3(0);
    }
    return contribution * reservoir.Weight;
}
