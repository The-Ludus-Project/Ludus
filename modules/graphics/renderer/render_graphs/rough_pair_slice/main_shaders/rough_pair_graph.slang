#include "rough_pair_transport.slang"

static const float RECEIVER_HALF_EXTENT_METERS = 0.9;
static const uint PATH_RECORDS_PER_PIXEL = 2;

float2 ComputeRoughPairReceiverWorld(uint2 positionPixel, uint2 extentPixel)
{
    float2 coordinatesUv = float2(positionPixel) / max(float2(extentPixel) - 1, 1);
    coordinatesUv = select(extentPixel == 1, 0.5, coordinatesUv);
    return (2 * coordinatesUv - 1) * RECEIVER_HALF_EXTENT_METERS;
}

float DrawRoughPairUniform(RayLaunch launch, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint word = SampleRandomUInt32(launch.Seed, PixelRandomAddress(pixel, launch.SampleIndex, dimension));
    return ComputeRoughPairOpenUniform(word);
}

PairResult SampleRoughPairReceiver(RayLaunch launch)
{
    float4 uniforms = float4(DrawRoughPairUniform(launch, 0),
                             DrawRoughPairUniform(launch, 1),
                             DrawRoughPairUniform(launch, 2),
                             DrawRoughPairUniform(launch, 3));
    return SamplePair(true, uniforms, 0, ComputeRoughPairReceiverWorld(launch.PositionPixel, launch.ExtentPixel));
}

bool HasRoughPairCacheCapacity(uint2 extentPixel)
{
    uint count;
    uint stride;
    GraphBuffer0.GetDimensions(count, stride);
    return extentPixel.y > 0 && extentPixel.x <= (count / PATH_RECORDS_PER_PIXEL) / extentPixel.y;
}

uint GetRoughPairCacheBase(uint2 positionPixel, uint2 extentPixel)
{
    return (positionPixel.y * extentPixel.x + positionPixel.x) * PATH_RECORDS_PER_PIXEL;
}

PairResult LoadRoughPairCache(uint2 positionPixel, uint2 extentPixel)
{
    uint base = GetRoughPairCacheBase(positionPixel, extentPixel);
    float4 positionsWorld = asfloat(GraphBuffer0[base]);
    float4 statistics = asfloat(GraphBuffer0[base + 1]);
    PairResult result = {};
    result.FirstPositionWorld = positionsWorld.xy;
    result.SecondPositionWorld = positionsWorld.zw;
    result.Density = statistics.x;
    result.Radiance = statistics.y;
    result.HasFailed = statistics.z;
    return result;
}

float3 ShadeRoughPairFresh(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    PairResult fresh = SampleRoughPairReceiver(launch);
    if (fresh.HasFailed > 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
    }
    return float3(fresh.Radiance);
}

float3 StoreRoughPairSources(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRoughPairCacheCapacity(launch.ExtentPixel) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    PairResult fresh = SampleRoughPairReceiver(launch);
    uint base = GetRoughPairCacheBase(launch.PositionPixel, launch.ExtentPixel);
    GraphBuffer0[base] = asuint(float4(fresh.FirstPositionWorld, fresh.SecondPositionWorld));
    GraphBuffer0[base + 1] = asuint(float4(fresh.Density, fresh.Radiance, fresh.HasFailed, 0));
    return float3(0);
}

float3 ShadeRoughPairCached(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRoughPairCacheCapacity(launch.ExtentPixel) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    PairResult fresh = LoadRoughPairCache(launch.PositionPixel, launch.ExtentPixel);
    uint2 neighborPixel = uint2(launch.PositionPixel.x ^ 1, launch.PositionPixel.y);
    float radiance = fresh.Radiance;
    float hasFailed = fresh.HasFailed;
    if (neighborPixel.x < launch.ExtentPixel.x)
    {
        PairResult source = LoadRoughPairCache(neighborPixel, launch.ExtentPixel);
        CachedPairSample reused =
            CombineCachedRoughPair(fresh,
                                   source,
                                   ComputeRoughPairReceiverWorld(launch.PositionPixel, launch.ExtentPixel),
                                   ComputeRoughPairReceiverWorld(neighborPixel, launch.ExtentPixel));
        radiance = reused.Radiance;
        hasFailed = reused.HasFailed;
    }
    if (hasFailed > 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
    }
    return float3(radiance);
}
