#pragma once

// DI adapter: immutable CDF tail follows four history words per pixel.
static const uint PAIRWISE_TILE_WIDTH = REUSE_CELL_WIDTH;
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

struct DiPairwiseCellReader : IPairwiseCellReader
{
    uint Offset;
    float ReadPrefix(uint pixel)
    {
        return asfloat(GraphBuffer0[Offset + pixel].x);
    }
};
DiPairwiseCellReader MakeDiPairwiseCellReader(RayLaunch launch)
{
    DiPairwiseCellReader reader;
    reader.Offset = launch.ExtentPixel.x * launch.ExtentPixel.y * RESERVOIR_WORD_COUNT;
    return reader;
}
uint2 GetPairwiseTileExtent(RayLaunch launch)
{
    return GetReuseCellExtent(launch);
}
uint GetPairwisePixel(RayLaunch launch, uint slot)
{
    return GetReuseCellPixel(launch, slot);
}
uint GetPairwiseCanonicalSlot(RayLaunch launch)
{
    return GetReuseCanonicalSlot(launch);
}
float GetPairwiseMass(RayLaunch launch, uint slot)
{
    return ComputeReuseCellMass(launch, MakeDiPairwiseCellReader(launch), slot);
}
uint SamplePairwiseIndex(RayLaunch launch, uint dimension, uint bound, inout RayQueryContext queries)
{
    return SampleReuseCellIndex(launch, GetGraphPassIndex(), dimension, bound, queries);
}
uint SamplePairwiseNeighbor(RayLaunch launch,
                            uint dimension,
                            uint canonicalSlot,
                            float totalMass,
                            out float outProbability,
                            inout RayQueryContext queries)
{
    return SampleReuseCellNeighbor(launch,
                                   MakeDiPairwiseCellReader(launch),
                                   GetGraphPassIndex(),
                                   dimension,
                                   canonicalSlot,
                                   totalMass,
                                   outProbability,
                                   queries);
}
