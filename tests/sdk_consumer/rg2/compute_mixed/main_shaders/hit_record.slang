#pragma once
// Compute ray-tracing feasibility record (spec design D5): two uint4 words per pixel in GraphBuffer0.
// Word 0: packed triangle + 1 (0 = miss), asuint(DistanceWorld), asuint(barycentric.xy).
// Word 1: asuint(jitterPixel.xy), primary SampleIndex, asuint(primary automatic draw at dimension 0).
// Thanks to Burns and Hunt, "The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading",
// JCGT 2(2), 2013, https://jcgt.org/published/0002/02/04/ : store hit identity, not shaded attributes.
static const uint HIT_RECORD_WORDS = 2;
// The primary pass draws its jitter from explicit dimensions [0, PRIMARY_RANDOM_DIMENSION_COUNT).
static const uint PRIMARY_RANDOM_DIMENSION_COUNT = 2;

uint GetHitRecordBase(RayLaunch launch)
{
    return (launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x) * HIT_RECORD_WORDS;
}
