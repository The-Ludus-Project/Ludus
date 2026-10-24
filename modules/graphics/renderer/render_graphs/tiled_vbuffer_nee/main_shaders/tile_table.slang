#pragma once
// Tiled screen-space emitter selection shared by the tiles, prefix and shade passes.
// Thanks to Ola Olsson and Ulf Assarsson, "Tiled Shading", Journal of Graphics, GPU,
// and Game Tools 15(4), 2011, for per-screen-tile light work, and to Jan Boksanský,
// Paula Jukarainen and Chris Wyman, "Rendering Many Lights with Grid-Based Reservoirs",
// Ray Tracing Gems II, 2021, for amortizing light importance over a cell of receivers.
// Adapted here to one exact, normalized emitter CDF per 16 x 16 pixel tile.
//
// Every tile owns TILED_NEE_TILE_WORDS uint4 words of GraphBuffer0:
//   word 0: bin count, emitter count, total weight, layout tag (0 when invalid)
//   word 1: scene generation
//   words 2..: four binary32 bins per word, inclusive prefix sums after the prefix pass
// Bin b is emitter b. When a scene has more emitters than bins, the last bin is an
// overflow bucket whose emitters are chosen uniformly within it.
// Unbiasedness contract: the shade pass samples an emitter and evaluates the PMF of a
// BSDF-hit emitter from the SAME tile table within one frame. Every emitter with positive
// area keeps a positive weight, and an invalid or absent table falls back to uniform
// selection in both places.

static const uint TILED_NEE_TILE_SIZE_PIXELS = 16;
static const uint TILED_NEE_TILE_WORDS = 256;
static const uint TILED_NEE_HEADER_WORDS = 2;
static const uint TILED_NEE_BINS_PER_WORD = 4;
static const uint TILED_NEE_BIN_CAPACITY = (TILED_NEE_TILE_WORDS - TILED_NEE_HEADER_WORDS) * TILED_NEE_BINS_PER_WORD;
static const uint TILED_NEE_LAYOUT_TAG = 0x544e4531u;
// Representative receivers form a REPRESENTATIVE_GRID x REPRESENTATIVE_GRID lattice in the tile.
static const uint TILED_NEE_REPRESENTATIVE_GRID = 2;
static const uint TILED_NEE_REPRESENTATIVE_COUNT = TILED_NEE_REPRESENTATIVE_GRID * TILED_NEE_REPRESENTATIVE_GRID;
// Positive bins are raised to this fraction of the tile's mean bin weight, so emitters
// that score poorly at the representatives stay reachable from every pixel of the tile.
static const float TILED_NEE_MINIMUM_BIN_FRACTION = 0.01f;
// Clamp for the inverse-square falloff, in world units.
static const float TILED_NEE_MINIMUM_DISTANCE_WORLD = 0.001f;
// Minimum orientation factor of a triangle centroid: a centroid estimate must not exclude any region.
static const float TILED_NEE_ANGULAR_WEIGHT_FLOOR = 0.001f;

struct TileLocation
{
    uint Index;
    uint2 OriginPixel;
    uint2 ExtentPixel;
    uint LocalIndex; ///< Row-major invocation index inside the tile.
};

struct TileReceiver
{
    float3 PositionWorld;
    float3 NormalWorld;
};

struct TileEmitterSelection
{
    uint EmitterIndex;
    float SelectionPmf;
    bool HasSample;
};

TileLocation GetTileLocation(RayLaunch launch)
{
    const uint2 tileCount = (launch.ExtentPixel + (TILED_NEE_TILE_SIZE_PIXELS - 1)) / TILED_NEE_TILE_SIZE_PIXELS;
    const uint2 tile = launch.PositionPixel / TILED_NEE_TILE_SIZE_PIXELS;
    TileLocation result;
    result.Index = tile.y * tileCount.x + tile.x;
    result.OriginPixel = tile * TILED_NEE_TILE_SIZE_PIXELS;
    result.ExtentPixel = min(uint2(TILED_NEE_TILE_SIZE_PIXELS), launch.ExtentPixel - result.OriginPixel);
    const uint2 localPixel = launch.PositionPixel - result.OriginPixel;
    result.LocalIndex = localPixel.y * result.ExtentPixel.x + localPixel.x;
    return result;
}

/// True when the bound buffer holds this tile's complete record. Small images that cannot
/// hold every tile use uniform selection for the tiles that do not fit.
bool HasTileCapacity(uint tileIndex)
{
    uint wordCount;
    uint stride;
    GraphBuffer0.GetDimensions(wordCount, stride);
    return tileIndex < wordCount / TILED_NEE_TILE_WORDS;
}

uint GetTileWord(uint tileIndex, uint word)
{
    return tileIndex * TILED_NEE_TILE_WORDS + word;
}

uint GetTileBinCount(uint emitterCount)
{
    return min(emitterCount, TILED_NEE_BIN_CAPACITY);
}

/// Bins scored individually; with overflow, the last bin is filled by the prefix pass.
uint GetScoredBinCount(uint emitterCount)
{
    const uint binCount = GetTileBinCount(emitterCount);
    return emitterCount > binCount ? binCount - 1 : binCount;
}

float LoadTileBin(uint tileIndex, uint bin)
{
    const uint4 word = GraphBuffer0[GetTileWord(tileIndex, TILED_NEE_HEADER_WORDS + bin / TILED_NEE_BINS_PER_WORD)];
    return asfloat(word[bin % TILED_NEE_BINS_PER_WORD]);
}

/// Load the reconstructed surfaces at the tile's representative pixels. Pixels that miss or
/// cannot be reconstructed are skipped; outCount may be zero.
void LoadTileReceivers(SceneContext scene,
                       RayLaunch launch,
                       TileLocation tile,
                       RayQueryContext queries,
                       out TileReceiver outReceivers[TILED_NEE_REPRESENTATIVE_COUNT],
                       out uint outCount)
{
    outCount = 0;
    [unroll] for (uint i = 0; i < TILED_NEE_REPRESENTATIVE_COUNT; ++i)
    {
        outReceivers[i] = (TileReceiver)0;
        const uint2 cell = uint2(i % TILED_NEE_REPRESENTATIVE_GRID, i / TILED_NEE_REPRESENTATIVE_GRID);
        RayLaunch probe = launch;
        probe.PositionPixel =
            tile.OriginPixel + (tile.ExtentPixel * (2 * cell + 1)) / (2 * TILED_NEE_REPRESENTATIVE_GRID);
        probe.CameraRay = MakeCameraRay(probe, float2(0.5));
        // Reconstruction reads no query budget or random stream. A private copy keeps a
        // silhouette failure at another pixel from rejecting this invocation.
        RayQueryContext probeQueries = queries;
        probeQueries.HasFailed = false;
        SurfaceHit hit;
        if (LoadVisibilitySurface(scene, probe, probeQueries, hit) != SCENE_QUERY_SURFACE)
        {
            continue;
        }
        const float normalLengthSquared = dot(hit.ShadingNormalWorld, hit.ShadingNormalWorld);
        if (isfinite(normalLengthSquared) == false || normalLengthSquared <= 0)
        {
            continue;
        }
        outReceivers[outCount].PositionWorld = hit.PositionWorld;
        outReceivers[outCount].NormalWorld = hit.ShadingNormalWorld * rsqrt(normalLengthSquared);
        ++outCount;
    }
}

/// Distance/orientation importance of one emitter, averaged over the tile's receivers.
/// Positive for every emitter with positive area; zero only for emitters without area.
float ScoreTileEmitter(SceneContext scene,
                       uint emitterIndex,
                       TileReceiver receivers[TILED_NEE_REPRESENTATIVE_COUNT],
                       uint receiverCount)
{
    const float4 emitter = GetEmitter(scene, emitterIndex);
    if (receiverCount == 0)
    {
        // No surface in the tile to orient towards: every emitter with area is equally likely.
        return emitter.z > 0 ? 1.0f : 0.0f;
    }
    const float TRIANGLE_VERTEX_COUNT = 3.0f;
    const float minimumDistanceSquaredWorld = TILED_NEE_MINIMUM_DISTANCE_WORLD * TILED_NEE_MINIMUM_DISTANCE_WORLD;
    const uint triangleCount = uint(emitter.y);
    float weight = 0.0f;
    [loop] for (uint i = 0; i < triangleCount; ++i)
    {
        const TriangleData triangle = GetEmitterTriangle(scene, emitterIndex, i);
        if (triangle.IsValid == false || triangle.AreaWorld <= 0.0f)
        {
            continue;
        }
        const float3 centerWorld =
            (triangle.Position0World + triangle.Position1World + triangle.Position2World) / TRIANGLE_VERTEX_COUNT;
        const float radiusWorld =
            max(length(triangle.Position0World - centerWorld),
                max(length(triangle.Position1World - centerWorld), length(triangle.Position2World - centerWorld)));
        const float3 emitterNormalWorld = normalize(triangle.GeometricNormalWorld);
        const bool isTwoSided = GetMaterial(scene, triangle.MaterialIndex).IsEmissionTwoSided;
        [loop] for (uint receiver = 0; receiver < receiverCount; ++receiver)
        {
            const float3 toEmitterWorld = centerWorld - receivers[receiver].PositionWorld;
            const float distanceSquaredWorld = dot(toEmitterWorld, toEmitterWorld);
            float angularWeight = 1.0f;
            if (distanceSquaredWorld > minimumDistanceSquaredWorld)
            {
                const float3 directionWorld = toEmitterWorld * rsqrt(distanceSquaredWorld);
                const float cosSurface = max(dot(receivers[receiver].NormalWorld, directionWorld), 0.0f);
                const float emitterFacing = dot(emitterNormalWorld, -directionWorld);
                const float cosEmitter = isTwoSided == true ? abs(emitterFacing) : max(emitterFacing, 0.0f);
                angularWeight = cosSurface * cosEmitter;
            }
            angularWeight = max(angularWeight, TILED_NEE_ANGULAR_WEIGHT_FLOOR);
            const float clampedDistanceSquaredWorld =
                max(distanceSquaredWorld, max(radiusWorld * radiusWorld, minimumDistanceSquaredWorld));
            weight += triangle.AreaWorld * angularWeight / clampedDistanceSquaredWorld;
        }
    }
    return weight / float(receiverCount);
}

/// True when this frame's prefix pass published a usable table for the current scene.
bool TryLoadTileTable(SceneContext scene, uint tileIndex, out uint outBinCount, out float outTotalWeight)
{
    outBinCount = 0;
    outTotalWeight = 0;
    const uint emitterCount = GetEmitterCount(scene);
    if (emitterCount == 0 || HasTileCapacity(tileIndex) == false)
    {
        return false;
    }
    const uint4 header = GraphBuffer0[GetTileWord(tileIndex, 0)];
    const uint4 identity = GraphBuffer0[GetTileWord(tileIndex, 1)];
    outBinCount = header.x;
    outTotalWeight = asfloat(header.z);
    return header.w == TILED_NEE_LAYOUT_TAG && header.y == emitterCount &&
           outBinCount == GetTileBinCount(emitterCount) && all(identity.xy == scene.Generation) == true &&
           isfinite(outTotalWeight) == true && outTotalWeight > 0;
}

float GetTileBinPmf(uint tileIndex, uint bin, float totalWeight)
{
    const float previous = bin > 0 ? LoadTileBin(tileIndex, bin - 1) : 0.0f;
    return max(LoadTileBin(tileIndex, bin) - previous, 0.0f) / totalWeight;
}

/// Number of emitters sharing a bin: one, or every remaining emitter for the overflow bin.
uint GetTileBinEmitterCount(uint bin, uint binCount, uint emitterCount)
{
    return bin + 1 == binCount ? emitterCount - bin : 1;
}

/// Choose an emitter from this tile's CDF, consuming one or two stream dimensions.
TileEmitterSelection SelectTileEmitter(SceneContext scene, uint tileIndex, inout RayQueryContext queries)
{
    TileEmitterSelection result = (TileEmitterSelection)0;
    const uint emitterCount = GetEmitterCount(scene);
    if (emitterCount == 0)
    {
        return result;
    }
    uint binCount;
    float totalWeight;
    if (TryLoadTileTable(scene, tileIndex, binCount, totalWeight) == false)
    {
        result.EmitterIndex = SampleSceneRandom(queries, 0u, emitterCount);
        result.SelectionPmf = 1.0f / float(emitterCount);
        result.HasSample = queries.HasFailed == false;
        return result;
    }
    const float target = SampleSceneRandom(queries) * totalWeight;
    // Thanks to Pharr, Jakob and Humphreys, PBRT 4e, section 2.3, sampling a discrete CDF:
    // https://pbr-book.org/4ed/Monte_Carlo_Integration/Sampling_Using_the_Inversion_Method
    // The first bin whose inclusive prefix exceeds the target owns it.
    uint first = 0;
    uint last = binCount - 1;
    [loop] while (first < last)
    {
        const uint middle = (first + last) / 2;
        if (LoadTileBin(tileIndex, middle) > target)
        {
            last = middle;
        }
        else
        {
            first = middle + 1;
        }
    }
    // A target rounded onto the total lands on the last bin; step back to a positive bin.
    float binPmf = GetTileBinPmf(tileIndex, first, totalWeight);
    [loop] while (binPmf <= 0 && first > 0)
    {
        --first;
        binPmf = GetTileBinPmf(tileIndex, first, totalWeight);
    }
    const uint sharedCount = GetTileBinEmitterCount(first, binCount, emitterCount);
    result.EmitterIndex = sharedCount > 1 ? first + SampleSceneRandom(queries, 0u, sharedCount) : first;
    result.SelectionPmf = binPmf / float(sharedCount);
    result.HasSample = queries.HasFailed == false && result.SelectionPmf > 0;
    return result;
}

/// The probability SelectTileEmitter assigns to emitterIndex, read from the same table in O(1).
float EvaluateTileEmitterPmf(SceneContext scene, uint tileIndex, uint emitterIndex)
{
    const uint emitterCount = GetEmitterCount(scene);
    if (emitterIndex >= emitterCount)
    {
        return 0.0f;
    }
    uint binCount;
    float totalWeight;
    if (TryLoadTileTable(scene, tileIndex, binCount, totalWeight) == false)
    {
        return 1.0f / float(emitterCount);
    }
    const uint bin = min(emitterIndex, binCount - 1);
    return GetTileBinPmf(tileIndex, bin, totalWeight) / float(GetTileBinEmitterCount(bin, binCount, emitterCount));
}
