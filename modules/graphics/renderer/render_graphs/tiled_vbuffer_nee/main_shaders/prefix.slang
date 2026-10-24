#include "tile_table.slang"

// Pass 2 of 3: the tile's first invocation turns the scored weights into an inclusive CDF
// in place, applies the relative floor, fills the overflow bin and publishes the header.
// Invalid weights publish an untagged header, so the shade pass selects uniformly.
float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    const TileLocation tile = GetTileLocation(launch);
    if (tile.LocalIndex != 0 || HasTileCapacity(tile.Index) == false)
    {
        return float3(0);
    }
    const uint emitterCount = GetEmitterCount(scene);
    const uint binCount = GetTileBinCount(emitterCount);
    const uint scoredBinCount = GetScoredBinCount(emitterCount);
    const uint wordCount = (binCount + TILED_NEE_BINS_PER_WORD - 1) / TILED_NEE_BINS_PER_WORD;
    float sum = 0.0f;
    bool isValid = scoredBinCount > 0;
    [loop] for (uint bin = 0; bin < scoredBinCount; ++bin)
    {
        const float weight = LoadTileBin(tile.Index, bin);
        isValid = isValid && isfinite(weight) == true && weight >= 0;
        sum += weight;
    }
    isValid = isValid && isfinite(sum) == true && sum > 0;
    const float minimumWeight = isValid == true ? TILED_NEE_MINIMUM_BIN_FRACTION * sum / float(scoredBinCount) : 0.0f;
    float cumulative = 0.0f;
    [loop] for (uint word = 0; isValid == true && word < wordCount; ++word)
    {
        const uint address = GetTileWord(tile.Index, TILED_NEE_HEADER_WORDS + word);
        uint4 bins = GraphBuffer0[address];
        [unroll] for (uint component = 0; component < TILED_NEE_BINS_PER_WORD; ++component)
        {
            const uint bin = word * TILED_NEE_BINS_PER_WORD + component;
            float weight = 0.0f;
            if (bin < scoredBinCount)
            {
                weight = asfloat(bins[component]);
                weight = weight > 0 ? max(weight, minimumWeight) : 0.0f;
            }
            else if (bin < binCount)
            {
                // Overflow: the remaining emitters share one bin at the mean scored weight.
                weight = cumulative / float(scoredBinCount) * float(emitterCount - scoredBinCount);
            }
            cumulative += weight;
            bins[component] = asuint(cumulative);
        }
        GraphBuffer0[address] = bins;
    }
    isValid = isValid && isfinite(cumulative) == true && cumulative > 0;
    GraphBuffer0[GetTileWord(tile.Index, 1)] = uint4(scene.Generation, 0, 0);
    GraphBuffer0[GetTileWord(tile.Index, 0)] =
        uint4(binCount, emitterCount, asuint(cumulative), isValid == true ? TILED_NEE_LAYOUT_TAG : 0u);
    return float3(0);
}
