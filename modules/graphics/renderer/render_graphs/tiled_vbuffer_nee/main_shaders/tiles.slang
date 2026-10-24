#include "tile_table.slang"

// Pass 1 of 3: score this tile's emitters. The tile's invocations split its bins: each
// writes whole uint4 words (four bins), so no two invocations store to the same word and
// no atomics are needed. Words are unnormalized weights until the prefix pass.
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    const TileLocation tile = GetTileLocation(launch);
    const uint emitterCount = GetEmitterCount(scene);
    const uint scoredBinCount = GetScoredBinCount(emitterCount);
    const uint wordCount = (scoredBinCount + TILED_NEE_BINS_PER_WORD - 1) / TILED_NEE_BINS_PER_WORD;
    if (tile.LocalIndex >= wordCount || HasTileCapacity(tile.Index) == false)
    {
        return float3(0);
    }
    TileReceiver receivers[TILED_NEE_REPRESENTATIVE_COUNT];
    uint receiverCount;
    LoadTileReceivers(scene, launch, tile, inoutQueries, receivers, receiverCount);
    const uint invocationCount = tile.ExtentPixel.x * tile.ExtentPixel.y;
    [loop] for (uint word = tile.LocalIndex; word < wordCount; word += invocationCount)
    {
        uint4 weights = uint4(0);
        [unroll] for (uint component = 0; component < TILED_NEE_BINS_PER_WORD; ++component)
        {
            const uint bin = word * TILED_NEE_BINS_PER_WORD + component;
            if (bin < scoredBinCount)
            {
                weights[component] = asuint(ScoreTileEmitter(scene, bin, receivers, receiverCount));
            }
        }
        GraphBuffer0[GetTileWord(tile.Index, TILED_NEE_HEADER_WORDS + word)] = weights;
    }
    // This pass only builds the table; the shade pass writes the color target.
    return float3(0);
}
