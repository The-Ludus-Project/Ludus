#include "math.slang"
#include "pairwise_weights.slang"
#include "raster_bindings.slang"
#include "scene_shader_layout.h"

LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Results;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint4> GraphBuffer0;
static const uint RESERVOIR_WORD_COUNT = 4;
static const uint INVOCATION_COUNT = 6912;
static const uint ITERATION_COUNT = 128;
static const uint MODE_COUNT = 3;
static const uint GROUP_WIDTH = 64;
struct RayLaunch
{
    uint2 PositionPixel;
    uint2 ExtentPixel;
    uint2 Seed;
    uint SampleIndex;
};
struct RayQueryContext
{
    bool HasFailed;
};
uint GetGraphPassIndex()
{
    return 2;
}
void FailSceneQuery(inout RayQueryContext inoutQueries, uint reason)
{
    inoutQueries.HasFailed = true;
}
float SampleReuseRandom(RayLaunch launch, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    RandomAddress address = MakeRandomAddress(uint2(pixel, GetGraphPassIndex()), launch.SampleIndex, dimension);
    return SampleRandomFloat01(launch.Seed, address);
}
#include "pairwise_distribution.slang"

float GetTarget(uint source, uint sample)
{
    if (source == 0)
        return sample == 0 ? 1 : sample == 1 ? 2 : 4;
    if (source == 1)
        return sample == 0 ? 4 : sample == 1 ? 0.001f : 1;
    return sample == 0 ? 0 : sample == 1 ? 3 : 2;
}
float GetIntegrand(uint sample)
{
    return sample == 0 ? 2 : sample == 1 ? 0 : 5;
}
uint GetSource(uint slot, uint canonical)
{
    if (slot == canonical)
        return 0;
    if (slot == (canonical + 1) % PAIRWISE_TILE_WIDTH)
        return 1;
    if (slot == (canonical + 2) % PAIRWISE_TILE_WIDTH)
        return 2;
    return MODE_COUNT;
}
float GetConfidence(uint source)
{
    return source == 0 ? 2 : source == 1 ? 3 : source == 2 ? 5 : 0;
}
uint GetSample(uint source, uint scenario)
{
    return source == 0 ? scenario % 3 : source == 1 ? (scenario / 3) % 3 : scenario / 9;
}

// Each invocation owns an independent 8x1 edge tile, including its canonical pixel.
// Exhaust all 3^3 independent uniform source samples; leave five empty records.
// Exercise the production CDF sampler and MIS weights, with separate canonical
// coins, repeated neighbor draws and zero targets, against a finite-domain CPU oracle.
[numthreads(GROUP_WIDTH, 1, 1)] void ComputeMain(uint3 invocation : SV_DispatchThreadID) {
    uint canonical = invocation.x % PAIRWISE_TILE_WIDTH;
    const uint SCENARIO_COUNT = 27;
    uint scenario = invocation.x % SCENARIO_COUNT;
    RayLaunch launch = (RayLaunch)0;
    launch.PositionPixel = uint2(invocation.x * PAIRWISE_TILE_WIDTH + canonical, 0);
    launch.ExtentPixel = uint2(INVOCATION_COUNT * PAIRWISE_TILE_WIDTH, 1);
    launch.Seed = uint2(123, 456);
    const float INVERSE_PROPOSAL_PDF = 3;
    float prefix = 0;
    [loop] for (uint slot = 0; slot < PAIRWISE_TILE_WIDTH; ++slot)
    {
        uint source = GetSource(slot, canonical);
        float target = source < MODE_COUNT ? GetTarget(source, GetSample(source, scenario)) : 0;
        prefix += GetConfidence(source) * target * INVERSE_PROPOSAL_PDF;
        StorePairwiseCell(launch, GetPairwisePixel(launch, slot), uint4(asuint(prefix), asuint(target), 10, 0));
    }
    [loop] for (uint slot = 0; slot < PAIRWISE_TILE_WIDTH; ++slot)
    {
        uint pixel = GetPairwisePixel(launch, slot);
        uint4 record = LoadPairwiseCell(launch, pixel);
        record.w = asuint(prefix);
        StorePairwiseCell(launch, pixel, record);
    }
    float totalMass = prefix - GetPairwiseMass(launch, canonical);
    float3 estimates = float3(0);
    RayQueryContext queries = (RayQueryContext)0;
    [loop] for (uint mode = 0; mode < MODE_COUNT; ++mode)
    {
        const uint MAXIMUM_DRAW_COUNT = 8;
        uint draws = mode == 0 ? 1 : mode == 1 ? 3 : MAXIMUM_DRAW_COUNT;
        float scale = min(float(draws) / float(PAIRWISE_TILE_WIDTH - 1), 1);
        const float CANONICAL_CONFIDENCE = 2;
        const float NEIGHBOR_CONFIDENCE = 8;
        float confidenceSum = NEIGHBOR_CONFIDENCE * scale;
        uint canonicalSample = GetSample(0, scenario);
        float canonicalTarget = GetTarget(0, canonicalSample);
        [loop] for (uint iteration = 0; iteration < ITERATION_COUNT; ++iteration)
        {
            launch.SampleIndex = mode * ITERATION_COUNT + iteration;
            uint slot = SamplePairwiseIndex(launch, 0, PAIRWISE_TILE_WIDTH - 1, queries);
            slot += slot >= canonical ? 1 : 0;
            uint source = GetSource(slot, canonical);
            float canonicalMis = CANONICAL_CONFIDENCE / (confidenceSum + CANONICAL_CONFIDENCE);
            if (source < MODE_COUNT)
                canonicalMis +=
                    float(PAIRWISE_TILE_WIDTH - 1) * EvaluatePairwiseCanonicalBeta(GetConfidence(source) * scale,
                                                                                   confidenceSum,
                                                                                   CANONICAL_CONFIDENCE,
                                                                                   GetTarget(source, canonicalSample),
                                                                                   canonicalTarget);
            float estimate = canonicalMis * GetIntegrand(canonicalSample) * INVERSE_PROPOSAL_PDF;
            [loop] for (uint draw = 0; draw < draws; ++draw)
            {
                const uint RANDOM_BASE = 64;
                const uint RANDOM_STRIDE = 4;
                float probability;
                slot = SamplePairwiseNeighbor(launch,
                                              RANDOM_BASE + draw * RANDOM_STRIDE,
                                              canonical,
                                              totalMass,
                                              probability,
                                              queries);
                source = GetSource(slot, canonical);
                if (source >= MODE_COUNT)
                    continue;
                uint sample = GetSample(source, scenario);
                float mis = EvaluatePairwiseNeighborMis(GetConfidence(source) * scale,
                                                        confidenceSum,
                                                        CANONICAL_CONFIDENCE,
                                                        GetTarget(source, sample),
                                                        GetTarget(0, sample));
                estimate += mis * GetIntegrand(sample) * INVERSE_PROPOSAL_PDF / (float(draws) * probability);
            }
            estimates[mode] += estimate / float(ITERATION_COUNT);
        }
    }
    // A zero total mass must select uniformly, still excluding the canonical pixel.
    launch.SampleIndex = 0;
    float probability;
    uint uniformSlot = SamplePairwiseNeighbor(launch, 0, canonical, 0, probability, queries);
    if (uniformSlot == canonical || abs(probability - 1.0f / float(PAIRWISE_TILE_WIDTH - 1)) > 0.000001f)
        queries.HasFailed = true;
    // The bottom-right pixel of a 9x9 image is a singleton tile, with no neighbors.
    const uint EDGE_EXTENT = PAIRWISE_TILE_WIDTH + 1;
    launch.ExtentPixel = uint2(EDGE_EXTENT);
    launch.PositionPixel = uint2(PAIRWISE_TILE_WIDTH);
    if (any(GetPairwiseTileExtent(launch) != uint2(1)) == true || GetPairwiseCanonicalSlot(launch) != 0 ||
        GetPairwisePixel(launch, 0) != EDGE_EXTENT * EDGE_EXTENT - 1)
        queries.HasFailed = true;
    Results[invocation.x] = uint4(asuint(estimates), queries.HasFailed == true ? 1 : 0);
}
