#pragma once

static const uint PAIRWISE_TILE_WIDTH = 8;
// A uniform mixture preserves positive selection probability even when a tiny
// positive mass disappears during floating-point prefix accumulation.
static const float PAIRWISE_UNIFORM_PROBABILITY = 0.05f;

uint2 GetPairwiseTileExtent(RayLaunch launch)
{
    uint2 originPixel = launch.PositionPixel / PAIRWISE_TILE_WIDTH * PAIRWISE_TILE_WIDTH;
    return min(uint2(PAIRWISE_TILE_WIDTH), launch.ExtentPixel - originPixel);
}

uint GetPairwisePixel(RayLaunch launch, uint slot)
{
    uint2 originPixel = launch.PositionPixel / PAIRWISE_TILE_WIDTH * PAIRWISE_TILE_WIDTH;
    uint2 extentPixel = GetPairwiseTileExtent(launch);
    uint2 positionPixel = originPixel + uint2(slot % extentPixel.x, slot / extentPixel.x);
    return positionPixel.y * launch.ExtentPixel.x + positionPixel.x;
}

uint GetPairwiseCanonicalSlot(RayLaunch launch)
{
    uint2 extentPixel = GetPairwiseTileExtent(launch);
    uint2 localPixel = launch.PositionPixel % PAIRWISE_TILE_WIDTH;
    return localPixel.y * extentPixel.x + localPixel.x;
}

bool HasPairwiseCellCapacity(RayLaunch launch)
{
    uint words;
    uint stride;
    GraphBuffer0.GetDimensions(words, stride);
    const uint WORDS_PER_PIXEL = RESERVOIR_WORD_COUNT + 1;
    return launch.ExtentPixel.y > 0 && launch.ExtentPixel.x <= words / WORDS_PER_PIXEL / launch.ExtentPixel.y;
}

// Cell records follow the packed 64-byte history region. Reuse only writes
// that history region; the CDF tail stays immutable throughout the shade pass.
uint4 LoadPairwiseCell(RayLaunch launch, uint pixel)
{
    uint offset = launch.ExtentPixel.x * launch.ExtentPixel.y * RESERVOIR_WORD_COUNT;
    return GraphBuffer0[offset + pixel];
}

void StorePairwiseCell(RayLaunch launch, uint pixel, uint4 record)
{
    uint offset = launch.ExtentPixel.x * launch.ExtentPixel.y * RESERVOIR_WORD_COUNT;
    GraphBuffer0[offset + pixel] = record;
}

float GetPairwiseMass(RayLaunch launch, uint slot)
{
    float prefix = asfloat(LoadPairwiseCell(launch, GetPairwisePixel(launch, slot)).x);
    float previous = slot == 0 ? 0 : asfloat(LoadPairwiseCell(launch, GetPairwisePixel(launch, slot - 1)).x);
    return max(prefix - previous, 0);
}

uint SamplePairwiseIndex(RayLaunch launch, uint dimension, uint bound, inout RayQueryContext queries)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    RandomAddress address = MakeRandomAddress(uint2(pixel, GetGraphPassIndex()), launch.SampleIndex, dimension);
    uint result = 0;
    if (TrySampleRandomBounded(launch.Seed, address, bound, result) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED);
    return result;
}

// Draws with replacement; the canonical pixel is excluded from both proposals.
uint SamplePairwiseNeighbor(RayLaunch launch,
                            uint dimension,
                            uint canonicalSlot,
                            float totalMass,
                            out float outProbability,
                            inout RayQueryContext queries)
{
    uint2 extentPixel = GetPairwiseTileExtent(launch);
    uint count = extentPixel.x * extentPixel.y - 1;
    float uniformProbability = totalMass > 0 ? PAIRWISE_UNIFORM_PROBABILITY : 1;
    uint slot = SamplePairwiseIndex(launch, dimension + 1, count, queries);
    slot += slot >= canonicalSlot ? 1 : 0;
    if (SampleReuseRandom(launch, dimension) >= uniformProbability)
    {
        float position = SampleReuseRandom(launch, dimension + 2) * totalMass;
        // Scan only cached scalar masses, never shifted BSDFs or visibility.
        [loop] for (uint candidate = 0; candidate <= count; ++candidate)
        {
            if (candidate == canonicalSlot)
                continue;
            float mass = GetPairwiseMass(launch, candidate);
            if (mass > 0)
            {
                slot = candidate;
                if (position < mass)
                    break;
                position -= mass;
            }
        }
    }
    outProbability = uniformProbability / float(count);
    if (totalMass > 0)
        outProbability += (1 - uniformProbability) * GetPairwiseMass(launch, slot) / totalMass;
    return slot;
}
