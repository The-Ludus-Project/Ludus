#pragma once

// Renderer-owned tile geometry and bounded CDF sampling. Math weights and UCW
// contracts live in FoundationMath's stochastic_pairwise_mis.slang. The caller
// supplies immutable inclusive CDF prefixes and owns all resource bindings,
// layout, lifetime, target evaluation and cell construction. One logical cell
// is an 8x8 tile clipped to the launch extent; slots are row-major in that cell.
// Thanks to Hedstrom et al., CGF 45(2), 2026, section 5 and Algorithm 2:
// https://doi.org/10.1111/cgf.70391 . Fixed tiles replace the paper's classified
// cell lookup; the with-replacement probability correction is unchanged.
/// Caller-owned immutable inclusive CDF access. No binding or storage layout is imposed.
/// Prefixes are finite, nonnegative and nondecreasing within each clipped cell.
/// The backing storage must remain readable and unchanged for all sampler invocations.
interface IPairwiseCellReader
{
    /// Return the finite, nondecreasing inclusive CDF prefix for this absolute pixel.
    float ReadPrefix(uint pixel);
};
/// Return an addressed uniform float in [0,1). Caller assigns disjoint scope/dimensions
/// from initial sampling, canonical correction, neighbor draws and reservoir coins.
float SampleReuseCellRandom(RayLaunch launch, uint scope, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    RandomAddress address = MakeRandomAddress(uint2(pixel, scope), launch.SampleIndex, dimension);
    return SampleRandomFloat01(launch.Seed, address);
}
/// Side length in pixels of the fixed screen tile; edge tiles are clipped.
static const uint REUSE_CELL_WIDTH = 8;
// A uniform mixture preserves positive selection probability even when a tiny
// positive mass disappears during floating-point prefix accumulation.
/// Uniform mixture fraction used with positive CDF mass to preserve proposal support.
static const float REUSE_CELL_UNIFORM_PROBABILITY = 0.05f;

/// Return the in-bounds extent of the launch pixel's clipped screen tile.
/// Requires a positive extent and PositionPixel inside it; flattened pixels must fit uint.
uint2 GetReuseCellExtent(RayLaunch launch)
{
    uint2 originPixel = launch.PositionPixel / REUSE_CELL_WIDTH * REUSE_CELL_WIDTH;
    return min(uint2(REUSE_CELL_WIDTH), launch.ExtentPixel - originPixel);
}

/// Map a row-major cell slot to its absolute flattened pixel.
/// Requires slot < product(GetReuseCellExtent(launch)) and a valid launch pixel.
uint GetReuseCellPixel(RayLaunch launch, uint slot)
{
    uint2 originPixel = launch.PositionPixel / REUSE_CELL_WIDTH * REUSE_CELL_WIDTH;
    uint2 extentPixel = GetReuseCellExtent(launch);
    uint2 positionPixel = originPixel + uint2(slot % extentPixel.x, slot / extentPixel.x);
    return positionPixel.y * launch.ExtentPixel.x + positionPixel.x;
}

/// Return the launch pixel's row-major slot within its clipped cell.
/// Requires a valid launch pixel and positive extent.
uint GetReuseCanonicalSlot(RayLaunch launch)
{
    uint2 extentPixel = GetReuseCellExtent(launch);
    uint2 localPixel = launch.PositionPixel % REUSE_CELL_WIDTH;
    return localPixel.y * extentPixel.x + localPixel.x;
}

/// Recover a nonnegative mass from two immutable inclusive prefixes.
/// Requires a valid launch, slot and reader; prefixes must follow IPairwiseCellReader.
float ComputeReuseCellMass<T : IPairwiseCellReader>(RayLaunch launch, T reader, uint slot)
{
    float prefix = reader.ReadPrefix(GetReuseCellPixel(launch, slot));
    float previous = slot == 0 ? 0 : reader.ReadPrefix(GetReuseCellPixel(launch, slot - 1));
    return max(prefix - previous, 0);
}

/// Draw a uniform integer in [0,bound), requiring bound > 0 and a valid launch.
/// Latches random-exhaustion failure into queries; returns zero on failure.
/// Reserve this dimension exclusively, including the bounded sampler's rejection lanes.
uint SampleReuseCellIndex(RayLaunch launch, uint scope, uint dimension, uint bound, inout RayQueryContext queries)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    RandomAddress address = MakeRandomAddress(uint2(pixel, scope), launch.SampleIndex, dimension);
    uint result = 0;
    if (TrySampleRandomBounded(launch.Seed, address, bound, result) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED);
    return result;
}

// Draws with replacement; the canonical pixel is excluded from both proposals.
/// Draw one noncanonical slot with replacement and write its actual mixture PMF.
/// Requires at least two cell pixels, canonicalSlot matching GetReuseCanonicalSlot,
/// and finite totalMass equal to the sum of recovered noncanonical masses.
/// Reader prefixes stay immutable; reserve dimension through dimension+2 per draw.
/// Zero totalMass selects uniformly. Failures latch into queries; discard their result.
/// Repeat draws retain multiplicity; pass this PMF to TryComputeStochasticMisWeight.
uint SampleReuseCellNeighbor<T : IPairwiseCellReader>(RayLaunch launch,
                                                      T reader,
                                                      uint scope,
                                                      uint dimension,
                                                      uint canonicalSlot,
                                                      float totalMass,
                                                      out float outProbability,
                                                      inout RayQueryContext queries)
{
    uint2 extentPixel = GetReuseCellExtent(launch);
    uint count = extentPixel.x * extentPixel.y - 1;
    float uniformProbability = totalMass > 0 ? REUSE_CELL_UNIFORM_PROBABILITY : 1;
    uint slot = SampleReuseCellIndex(launch, scope, dimension + 1, count, queries);
    slot += slot >= canonicalSlot ? 1 : 0;
    if (SampleReuseCellRandom(launch, scope, dimension) >= uniformProbability)
    {
        float position = SampleReuseCellRandom(launch, scope, dimension + 2) * totalMass;
        // Scan only cached scalar masses, never shifted BSDFs or visibility.
        [loop] for (uint candidate = 0; candidate <= count; ++candidate)
        {
            if (candidate == canonicalSlot)
                continue;
            float mass = ComputeReuseCellMass(launch, reader, candidate);
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
        outProbability += (1 - uniformProbability) * ComputeReuseCellMass(launch, reader, slot) / totalMass;
    return slot;
}
