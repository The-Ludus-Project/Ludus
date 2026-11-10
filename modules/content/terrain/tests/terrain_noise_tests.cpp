// Noise recipe contracts: shape validation, range, continuity, independence from
// sampling resolution and the pinned elevation digest of layout version 1. The
// digest pins the heights every saved default World was generated with when
// the recipe moved out of the editor; a different value needs a new layout
// version, never an updated constant.

#include <ludus/content/terrain/terrain_noise.h>

#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::terrain;
namespace math = ludus::foundation::math;
using ludus::foundation::core::int64;

namespace
{
/// Random domain the editor's default World terrain uses.
constexpr uint32 EDITOR_TERRAIN_DOMAIN = 1;
constexpr uint64 GOLDEN_SEED = 7;
constexpr uint32 GOLDEN_SAMPLES = 17;
constexpr float64 GOLDEN_EXTENT_METRES = 32;
/// Heights are pinned to a micrometre so libm rounding differences cannot move the digest.
constexpr float64 DIGEST_SCALE = 1.0e6;
/// FNV-1a 64-bit offset basis and prime.
constexpr uint64 FNV_OFFSET = 0xcbf29ce484222325ULL;
constexpr uint64 FNV_PRIME = 0x100000001b3ULL;
/// Pinned digest of the golden hills grid for TERRAIN_NOISE_LAYOUT_VERSION 1.
constexpr uint64 GOLDEN_HILLS_DIGEST = 0x9e6a601abac4dadcULL;

[[nodiscard]] math::RandomKey MakeKey(uint64 seed)
{
    math::RandomKey key;
    REQUIRE(math::TryMakeRandomKey(seed, EDITOR_TERRAIN_DOMAIN, key) == math::MathStatus::Success);
    return key;
}

[[nodiscard]] TerrainNoiseShape MakeHills()
{
    return
    {
        .MinimumElevationMetres = 0,
        .MaximumElevationMetres = 8,
        .FeatureSizeMetres = 16,
        .Roughness = 0.45,
        .Profile = TerrainNoiseProfile::Hills,
    };
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the running digest and the next height are distinct.
[[nodiscard]] uint64 MixDigest(uint64 digest, float64 elevation)
{
    const int64 quantized = static_cast<int64>(math::Floor(elevation * DIGEST_SCALE + 0.5));
    uint64 bits = static_cast<uint64>(quantized);
    for (uint32 byte = 0; byte < sizeof(bits); ++byte)
    {
        digest ^= bits & 0xffU;
        digest *= FNV_PRIME;
        bits >>= 8U;
    }
    return digest;
}

[[nodiscard]] uint64 ComputeGridDigest(const TerrainNoiseShape& shape, math::RandomKey key, uint32 samples)
{
    uint64 digest = FNV_OFFSET;
    for (uint32 z = 0; z < samples; ++z)
    {
        const float64 positionZ = (static_cast<float64>(z) / (samples - 1) - 0.5) * GOLDEN_EXTENT_METRES;
        for (uint32 x = 0; x < samples; ++x)
        {
            const float64 positionX = (static_cast<float64>(x) / (samples - 1) - 0.5) * GOLDEN_EXTENT_METRES;
            digest = MixDigest(digest, ComputeTerrainElevation(shape, key, positionX, positionZ));
        }
    }
    return digest;
}
} // namespace

TEST_CASE("terrain noise shape validation", "[content][terrain]")
{
    const TerrainNoiseShape hills = MakeHills();
    CHECK(CanSampleTerrainNoise(hills, GOLDEN_EXTENT_METRES) == true);
    CHECK(CanSampleTerrainNoise(hills, -1) == false);

    TerrainNoiseShape shape = hills;
    shape.FeatureSizeMetres = 0;
    CHECK(CanSampleTerrainNoise(shape, 1) == false);

    shape = hills;
    shape.MaximumElevationMetres = shape.MinimumElevationMetres - 1;
    CHECK(CanSampleTerrainNoise(shape, 1) == false);

    shape = hills;
    shape.Roughness = 1.5;
    CHECK(CanSampleTerrainNoise(shape, 1) == false);

    shape = hills;
    shape.FeatureSizeMetres = 1;
    CHECK(CanSampleTerrainNoise(shape, MAX_TERRAIN_LATTICE_POSITION) == false);
    CHECK(CanSampleTerrainNoise(shape, MAX_TERRAIN_LATTICE_POSITION / 4) == true);
}

TEST_CASE("terrain noise stays within the elevation bounds", "[content][terrain]")
{
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    for (const TerrainNoiseProfile profile : {TerrainNoiseProfile::Hills, TerrainNoiseProfile::Ridges})
    {
        TerrainNoiseShape shape = MakeHills();
        shape.Profile = profile;
        for (uint32 z = 0; z < GOLDEN_SAMPLES; ++z)
        {
            for (uint32 x = 0; x < GOLDEN_SAMPLES; ++x)
            {
                const float64 elevation = ComputeTerrainElevation(shape, key, x * 1.7 - 11.0, z * 2.3 - 13.0);
                CHECK(elevation >= shape.MinimumElevationMetres);
                CHECK(elevation <= shape.MaximumElevationMetres);
            }
        }
    }
    TerrainNoiseShape flat = MakeHills();
    flat.Profile = TerrainNoiseProfile::Flat;
    CHECK(ComputeTerrainElevation(flat, key, 3, 4) == flat.MinimumElevationMetres);
}

TEST_CASE("terrain noise is zero on the lattice and continuous between points", "[content][terrain]")
{
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    CHECK(SampleTerrainNoise(key, 0, 3, -2) == 0);
    CHECK(SampleTerrainNoise(key, 1, -7, 5) == 0);
    constexpr float64 STEP = 1.0 / 64;
    constexpr float64 MAX_STEP_CHANGE = 0.2;
    float64 previous = SampleTerrainNoise(key, 0, 0.5, 0.25);
    for (uint32 step = 1; step <= 128; ++step)
    {
        const float64 current = SampleTerrainNoise(key, 0, 0.5 + step * STEP, 0.25);
        CHECK(math::Abs(current - previous) < MAX_STEP_CHANGE);
        previous = current;
    }
}

TEST_CASE("terrain noise does not depend on sampling resolution", "[content][terrain]")
{
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    const TerrainNoiseShape shape = MakeHills();
    constexpr uint32 COARSE = 9;
    constexpr uint32 FINE = 2 * (COARSE - 1) + 1;
    for (uint32 z = 0; z < COARSE; ++z)
    {
        for (uint32 x = 0; x < COARSE; ++x)
        {
            const float64 coarseX = (static_cast<float64>(x) / (COARSE - 1) - 0.5) * GOLDEN_EXTENT_METRES;
            const float64 coarseZ = (static_cast<float64>(z) / (COARSE - 1) - 0.5) * GOLDEN_EXTENT_METRES;
            const float64 fineX = (static_cast<float64>(2 * x) / (FINE - 1) - 0.5) * GOLDEN_EXTENT_METRES;
            const float64 fineZ = (static_cast<float64>(2 * z) / (FINE - 1) - 0.5) * GOLDEN_EXTENT_METRES;
            CHECK(ComputeTerrainElevation(shape, key, coarseX, coarseZ) ==
                  Catch::Approx(ComputeTerrainElevation(shape, key, fineX, fineZ)));
        }
    }
}

TEST_CASE("terrain noise golden digest", "[content][terrain]")
{
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    const uint64 digest = ComputeGridDigest(MakeHills(), key, GOLDEN_SAMPLES);
    INFO("digest " << digest);
    CHECK(digest == GOLDEN_HILLS_DIGEST);
    CHECK(ComputeGridDigest(MakeHills(), MakeKey(GOLDEN_SEED + 1), GOLDEN_SAMPLES) != digest);
}
