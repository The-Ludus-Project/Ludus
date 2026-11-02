#pragma once
#include "reservoir.slang"

// Thanks to Chris Wyman and Alexey Panteleev, "Rearchitecting Spatiotemporal Resampling for Production",
// High-Performance Graphics 2021, sections 5.2, 6.2 and 7, https://doi.org/10.2312/hpg.20211281 .
// Adapted tiled presampling, previous-frame-only reuse and decoupled shading. Only the fresh candidate's
// visibility enters resampling; cached/retested temporal and spatial visibility affect shading alone.
// This is the paper's biased direct-light estimator, not an unbiased reference transport profile.
static const uint PRODUCTION_POOL_COUNT = 128;
static const uint PRODUCTION_POOL_SAMPLES = 1024;
static const uint PRODUCTION_POOL_RECORD_COUNT = PRODUCTION_POOL_COUNT * PRODUCTION_POOL_SAMPLES;
static const uint PRODUCTION_TILE_WIDTH = 8;
static const uint PRODUCTION_HISTORY_BANK_COUNT = 2;
static const uint PRODUCTION_LAYOUT_TAG = 0x00525031u;
static const uint PRODUCTION_VISIBLE_BIT = 0x01000000u;
static const uint PRODUCTION_LAYOUT_MASK = 0x00ffffffu;
static const uint PRODUCTION_EXTENT_COMPONENT_BITS = 16;
static const uint PRODUCTION_MAXIMUM_EXTENT_PIXEL = (1u << PRODUCTION_EXTENT_COMPONENT_BITS) - 1;
static const uint PRODUCTION_IDENTITY_WORD = 3;
static const uint PRODUCTION_CANDIDATE_DIMENSION_FIRST = 1;
static const uint PRODUCTION_CANDIDATE_DIMENSION_COUNT = 2;
static const uint PRODUCTION_NEIGHBOR_DIMENSION_FIRST =
    PRODUCTION_CANDIDATE_DIMENSION_FIRST + PRODUCTION_CANDIDATE_DIMENSION_COUNT * RESTIR_INITIAL_CANDIDATE_COUNT;
static const uint PRODUCTION_MERGE_DIMENSION_FIRST =
    PRODUCTION_NEIGHBOR_DIMENSION_FIRST + 2 * RESTIR_PRODUCTION_SPATIAL_NEIGHBORS;

struct ProductionReservoir
{
    Reservoir Value;
    uint SampleIndex;
    bool IsVisible;
    float3 VisibilityReceiverWorld; // Local selection metadata; not serialized.
};

bool HasProductionCapacity(RayLaunch launch)
{
    uint poolWords;
    uint poolStride;
    uint historyWords;
    uint historyStride;
    GraphBuffer0.GetDimensions(poolWords, poolStride);
    GraphBuffer1.GetDimensions(historyWords, historyStride);
    const uint historyCapacity = historyWords / (RESERVOIR_WORD_COUNT * PRODUCTION_HISTORY_BANK_COUNT);
    return poolWords >= PRODUCTION_POOL_RECORD_COUNT && launch.ExtentPixel.x > 0 && launch.ExtentPixel.y > 0 &&
           all(launch.ExtentPixel <= PRODUCTION_MAXIMUM_EXTENT_PIXEL) == true &&
           launch.ExtentPixel.x <= historyCapacity / launch.ExtentPixel.y;
}

uint GetProductionHistoryAddress(RayLaunch launch, uint pixel, bool isPrevious)
{
    const uint bank = (launch.SampleIndex + (isPrevious == true ? 1u : 0u)) % PRODUCTION_HISTORY_BANK_COUNT;
    return (bank * launch.ExtentPixel.x * launch.ExtentPixel.y + pixel) * RESERVOIR_WORD_COUNT;
}

ProductionReservoir LoadProductionHistory(RayLaunch launch, uint pixel)
{
    const uint address = GetProductionHistoryAddress(launch, pixel, true);
    const uint4 sample = GraphBuffer1[address];
    const uint4 receiver = GraphBuffer1[address + 1];
    const uint4 view = GraphBuffer1[address + 2];
    const uint4 identity = GraphBuffer1[address + PRODUCTION_IDENTITY_WORD];
    ProductionReservoir result = (ProductionReservoir)0;
    result.Value.TriangleIndex = sample.x;
    result.Value.Barycentric = asfloat(sample.yz);
    result.Value.Weight = asfloat(sample.w);
    result.Value.CandidateCount = receiver.x;
    result.Value.ReceiverTriangleIndex = receiver.y;
    result.Value.ReceiverBarycentric = asfloat(receiver.zw);
    result.Value.ViewDirectionWorld = asfloat(view.xyz);
    result.Value.LayoutTag = view.w & PRODUCTION_LAYOUT_MASK;
    result.IsVisible = (view.w & PRODUCTION_VISIBLE_BIT) != 0;
    result.Value.SceneGeneration = identity.xy;
    result.Value.ExtentPixel =
        uint2(identity.z & PRODUCTION_MAXIMUM_EXTENT_PIXEL, identity.z >> PRODUCTION_EXTENT_COMPONENT_BITS);
    result.SampleIndex = identity.w;
    return result;
}

bool HasProductionHistory(ProductionReservoir source, SceneContext scene, RayLaunch launch)
{
    return launch.SampleIndex > 0 && source.SampleIndex == launch.SampleIndex - 1 &&
           source.Value.LayoutTag == PRODUCTION_LAYOUT_TAG &&
           all(source.Value.SceneGeneration == scene.Generation) == true &&
           all(source.Value.ExtentPixel == launch.ExtentPixel) == true && source.Value.CandidateCount > 0 &&
           source.Value.CandidateCount <= RESTIR_HISTORY_CANDIDATES && isfinite(source.Value.Weight) == true &&
           source.Value.Weight >= 0;
}

void StoreProductionHistory(RayLaunch launch, ProductionReservoir result, RayQueryContext queries)
{
    const uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    const uint address = GetProductionHistoryAddress(launch, pixel, false);
    Reservoir value = result.Value;
    GraphBuffer1[address] = uint4(value.TriangleIndex, asuint(value.Barycentric), asuint(value.Weight));
    GraphBuffer1[address + 1] =
        uint4(value.CandidateCount, value.ReceiverTriangleIndex, asuint(value.ReceiverBarycentric));
    const uint tag = queries.HasFailed == true ? 0 : PRODUCTION_LAYOUT_TAG;
    GraphBuffer1[address + 2] =
        uint4(asuint(value.ViewDirectionWorld), tag | (result.IsVisible == true ? PRODUCTION_VISIBLE_BIT : 0));
    const uint extent = launch.ExtentPixel.x | (launch.ExtentPixel.y << PRODUCTION_EXTENT_COMPONENT_BITS);
    GraphBuffer1[address + PRODUCTION_IDENTITY_WORD] = uint4(value.SceneGeneration, extent, launch.SampleIndex);
}

bool TestProductionVisibility(SceneContext scene,
                              SurfaceHit receiver,
                              Reservoir reservoir,
                              inout RayQueryContext queries)
{
    if (reservoir.TriangleIndex == SCENE_INVALID_INDEX || reservoir.Weight <= 0)
        return false;
    SurfaceHit endpoint = GetSurfaceHit(scene, reservoir.TriangleIndex, reservoir.Barycentric);
    if (endpoint.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    const float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
    const float distanceWorld = length(offsetWorld);
    if (distanceWorld <= 0)
        return false;
    LightSample sample = (LightSample)0;
    sample.TriangleIndex = reservoir.TriangleIndex;
    sample.Barycentric = reservoir.Barycentric;
    sample.PositionWorld = endpoint.PositionWorld;
    sample.DirectionWorld = offsetWorld / distanceWorld;
    sample.DistanceWorld = distanceWorld;
    sample.HasFiniteEndpoint = true;
    sample.HasSample = true;
    return TestVisibility(scene, queries, receiver, sample) == SCENE_QUERY_MISS;
}

float3 EvaluateProductionShading(SceneContext scene, SurfaceHit receiver, ProductionReservoir reservoir)
{
    if (reservoir.IsVisible == false || reservoir.Value.TriangleIndex == SCENE_INVALID_INDEX ||
        reservoir.Value.Weight <= 0)
        return float3(0);
    return EvaluateSampleContributionArea(scene, receiver, reservoir.Value.TriangleIndex, reservoir.Value.Barycentric) *
           reservoir.Value.Weight;
}

uint SampleProductionIndex(RayLaunch launch, RandomAddress address, uint bound, inout RayQueryContext queries)
{
    uint value;
    if (TrySampleRandomBounded(launch.Seed, address, bound, value) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED);
    return value;
}
