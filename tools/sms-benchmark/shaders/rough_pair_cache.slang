// The same-seed map comes from Shlomi Steinberg's Slice Sampling draft,
// Section 6.5. Each receiver uses its fresh draw and one cached draw from
// its paired neighbor with Veach's multi-sample balance heuristic.
#include "rough_pair.slang"

struct ReceiverLane
{
    uint SampleLane;
    uint NeighborReceiver;
    float2 PositionWorld;
    bool IsActive;
    bool HasNeighbor;
};

static const uint CACHE_SLOT_COUNT = 2;
static const uint CACHE_RECORD_COUNT = CACHE_SLOT_COUNT * GROUP_SIZE;

groupshared float4 CachedPositionsWorld[CACHE_RECORD_COUNT];
groupshared float4 CachedStatistics[CACHE_RECORD_COUNT];

ReceiverLane GetReceiverLane(uint invocation)
{
    uint pairsPerRow = (Parameters.ReceiverGridWidth + 1) / 2;
    uint pairIndex = (invocation / 2) / Parameters.ReplicasPerReceiver;
    uint replica = (invocation / 2) % Parameters.ReplicasPerReceiver;
    uint row = pairIndex / pairsPerRow;
    uint column = (pairIndex % pairsPerRow) * 2 + invocation % 2;
    uint receiver = row * Parameters.ReceiverGridWidth + column;
    uint neighborColumn = column ^ 1;
    ReceiverLane lane = {};
    lane.IsActive = row < Parameters.ReceiverGridWidth && column < Parameters.ReceiverGridWidth;
    if (lane.IsActive == true)
    {
        lane.SampleLane = receiver * Parameters.ReplicasPerReceiver + replica;
        lane.PositionWorld = Receivers[receiver];
        lane.HasNeighbor = neighborColumn < Parameters.ReceiverGridWidth;
        lane.NeighborReceiver = row * Parameters.ReceiverGridWidth + neighborColumn;
    }
    return lane;
}

CachedPairSample SampleCachedPair(float4 uniforms, ReceiverLane lane, uint workgroupLane, uint slot)
{
    CachedPairSample result = {};
    if (lane.IsActive == true)
    {
        result.Fresh = SamplePair(true, uniforms, 0, lane.PositionWorld);
    }
    // Alternate slots. The next barrier completes all reads of the previous
    // slot before any lane can overwrite that slot two iterations later.
    uint cacheIndex = slot * GROUP_SIZE + workgroupLane;
    CachedPositionsWorld[cacheIndex] = float4(result.Fresh.FirstPositionWorld, result.Fresh.SecondPositionWorld);
    CachedStatistics[cacheIndex] = float4(result.Fresh.Density, result.Fresh.HasFailed, 0, 0);
    GroupMemoryBarrierWithGroupSync();

    result.FreshWeight = 1;
    result.HasFailed = result.Fresh.HasFailed;
    if (lane.HasNeighbor == true)
    {
        uint neighborIndex = cacheIndex ^ 1;
        float4 positionsWorld = CachedPositionsWorld[neighborIndex];
        float4 statistics = CachedStatistics[neighborIndex];
        PairResult source = {};
        source.FirstPositionWorld = positionsWorld.xy;
        source.SecondPositionWorld = positionsWorld.zw;
        source.Density = statistics.x;
        source.HasFailed = statistics.y;
        float2 sourceWorld = Receivers[lane.NeighborReceiver];
        return CombineCachedRoughPair(result.Fresh, source, lane.PositionWorld, sourceWorld);
    }
    result.Radiance = result.Fresh.Radiance;
    return result;
}

[numthreads(GROUP_SIZE, 1, 1)] void RenderRoughPairCache(uint3 invocation : SV_DispatchThreadID,
                                                         uint workgroupLane : SV_GroupIndex) {
    ReceiverLane lane = GetReceiverLane(invocation.x);
    float4 total = 0;
    for (uint index = 0; index < Parameters.Batch; ++index)
    {
        uint sampleIndex = Parameters.Offset + index;
        float4 uniforms = float4(DrawUniform(lane.SampleLane, sampleIndex, 0),
                                 DrawUniform(lane.SampleLane, sampleIndex, 1),
                                 DrawUniform(lane.SampleLane, sampleIndex, 2),
                                 DrawUniform(lane.SampleLane, sampleIndex, 3));
        CachedPairSample sample = SampleCachedPair(uniforms, lane, workgroupLane, index % CACHE_SLOT_COUNT);
        total +=
            float4(sample.Radiance, sample.Radiance * sample.Radiance, sample.Radiance > 0 ? 0 : 1, sample.HasFailed);
    }
    if (lane.IsActive == true)
    {
        Accumulation[lane.SampleLane] += total;
    }
}

    [numthreads(GROUP_SIZE, 1, 1)] void ProbeRoughPairCache(uint3 invocation : SV_DispatchThreadID,
                                                            uint workgroupLane : SV_GroupIndex)
{
    ReceiverLane lane = GetReceiverLane(invocation.x);
    uint receiver = lane.SampleLane / Parameters.ReplicasPerReceiver;
    uint replica = lane.SampleLane % Parameters.ReplicasPerReceiver;
    for (uint index = 0; index < Parameters.Batch; ++index)
    {
        uint drawIndex = (receiver * Parameters.Batch + index) * Parameters.ReplicasPerReceiver + replica;
        float4 uniforms = lane.IsActive ? ProbeDraws[drawIndex] : 0;
        CachedPairSample sample = SampleCachedPair(uniforms, lane, workgroupLane, index % CACHE_SLOT_COUNT);
        if (lane.IsActive == true)
        {
            const uint recordsPerProbe = 5;
            uint record = recordsPerProbe * drawIndex;
            WriteProbe(record, sample.Fresh);
            WriteProbe(record + 2, sample.Shifted);
            Probes[record + 4] = float4(sample.FreshWeight, sample.Radiance, sample.HasFailed, 0);
        }
    }
}
