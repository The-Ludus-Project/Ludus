// Noise recipe contracts: shape validation, range, continuity, independence from
// sampling resolution, lattice-seam derivatives and the pinned elevation digests
// of each layout. The layout 1 digest pins the heights every saved default World
// was generated with when the recipe moved out of the editor; a different value
// needs a new layout version, never an updated constant.

#include <ludus/content/terrain/terrain_noise.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::terrain;
namespace math = ludus::foundation::math;
using ludus::foundation::StaticArray;
using ludus::foundation::core::int64;
using ludus::foundation::core::usize;

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
/// Pinned digest of the golden hills grid for TerrainNoiseLayout::CubicFade (layout version 1).
constexpr uint64 GOLDEN_HILLS_DIGEST = 0x9e6a601abac4dadcULL;
/// Pinned digest of the same grid for TerrainNoiseLayout::QuinticFade (layout version 2).
constexpr uint64 GOLDEN_QUINTIC_HILLS_DIGEST = 0x100e4dd63208c656ULL;

/// Lattice-seam probe: distance between one-sided samples, an exact binary fraction of a lattice cell.
constexpr float64 SEAM_STEP = 1.0 / 8192;
/// One-sided cubic-interpolation stencils over the samples 1, 2, 3 and 4 steps from the edge; they extrapolate the
/// value, the first derivative (in steps, over six) and the second derivative (in steps squared) to the edge.
constexpr usize SEAM_STENCIL_SAMPLES = 4;
using SeamStencil = StaticArray<float64, SEAM_STENCIL_SAMPLES>;
constexpr SeamStencil SEAM_VALUE_WEIGHTS = {{4, -6, 4, -1}};
constexpr SeamStencil SEAM_SLOPE_WEIGHTS = {{-26, 57, -42, 11}};
constexpr float64 SEAM_SLOPE_DIVISOR = 6;
constexpr SeamStencil SEAM_CURVATURE_WEIGHTS = {{3, -8, 7, -2}};
/// Extrapolation error bounds for the stencils at SEAM_STEP, with margin over the fades' bounded higher derivatives.
constexpr float64 SEAM_VALUE_TOLERANCE = 1.0e-9;
constexpr float64 SEAM_SLOPE_TOLERANCE = 1.0e-6;
constexpr float64 SEAM_CURVATURE_TOLERANCE = 1.0e-3;
/// Lattice edge x = 3 crossed at fixed lattice Z positions inside one row of cells.
constexpr float64 SEAM_EDGE_X = 3;
constexpr usize SEAM_CROSSINGS = 3;
constexpr StaticArray<float64, SEAM_CROSSINGS> SEAM_ACROSS_Z = {{-2.75, -2.375, -2.125}};
/// Smallest cubic second-derivative jump the fixed key must show at the probed edge for the comparison to mean
/// anything.
constexpr float64 MIN_CUBIC_CURVATURE_JUMP = 0.5;

/// One-sided limits of one octave along X at a lattice edge.
struct SeamSide final
{
    float64 Value = 0;
    float64 Slope = 0;
    float64 Curvature = 0;
};

/// Both sides of a lattice edge.
struct Seam final
{
    SeamSide Left;
    SeamSide Right;
};

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
        .Layout = TerrainNoiseLayout::CubicFade,
    };
}

/// Extrapolate the value and derivatives along X at edgeX from the samples on one side only, so neither limit reads
/// the other cell. side is -1 for the cell below the edge and +1 for the cell above it.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the edge X, the crossing Z and the side are distinct.
[[nodiscard]] SeamSide
MeasureSeamSide(math::RandomKey key, TerrainNoiseLayout layout, float64 edgeX, float64 positionZ, float64 side)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    SeamSide result;
    float64 slope = 0;
    for (usize i = 0; i < SEAM_STENCIL_SAMPLES; ++i)
    {
        const float64 positionX = edgeX + side * SEAM_STEP * static_cast<float64>(i + 1);
        const float64 sample = SampleTerrainNoise(key, layout, 0, positionX, positionZ);
        // The fade, not the output clamp, must shape the probe.
        REQUIRE(math::Abs(sample) < 1);
        result.Value += SEAM_VALUE_WEIGHTS[i] * sample;
        slope += SEAM_SLOPE_WEIGHTS[i] * sample;
        result.Curvature += SEAM_CURVATURE_WEIGHTS[i] * sample;
    }
    result.Slope = side * slope / (SEAM_SLOPE_DIVISOR * SEAM_STEP);
    result.Curvature /= SEAM_STEP * SEAM_STEP;
    return result;
}

[[nodiscard]] Seam MeasureSeam(math::RandomKey key, TerrainNoiseLayout layout, float64 edgeX, float64 positionZ)
{
    return
    {
        .Left = MeasureSeamSide(key, layout, edgeX, positionZ, -1),
        .Right = MeasureSeamSide(key, layout, edgeX, positionZ, 1),
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
    shape.Layout = static_cast<TerrainNoiseLayout>(0);
    CHECK(CanSampleTerrainNoise(shape, 1) == false);
    shape.Layout = static_cast<TerrainNoiseLayout>(3);
    CHECK(CanSampleTerrainNoise(shape, 1) == false);
    shape.Layout = TerrainNoiseLayout::QuinticFade;
    CHECK(CanSampleTerrainNoise(shape, 1) == true);

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
    CHECK(SampleTerrainNoise(key, TerrainNoiseLayout::CubicFade, 0, 3, -2) == 0);
    CHECK(SampleTerrainNoise(key, TerrainNoiseLayout::QuinticFade, 1, -7, 5) == 0);
    constexpr float64 STEP = 1.0 / 64;
    constexpr float64 MAX_STEP_CHANGE = 0.2;
    for (const TerrainNoiseLayout layout : {TerrainNoiseLayout::CubicFade, TerrainNoiseLayout::QuinticFade})
    {
        float64 previous = SampleTerrainNoise(key, layout, 0, 0.5, 0.25);
        for (uint32 step = 1; step <= 128; ++step)
        {
            const float64 current = SampleTerrainNoise(key, layout, 0, 0.5 + step * STEP, 0.25);
            CHECK(math::Abs(current - previous) < MAX_STEP_CHANGE);
            previous = current;
        }
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

TEST_CASE("terrain noise layouts are stored explicitly", "[content][terrain]")
{
    CHECK(static_cast<uint32>(TerrainNoiseLayout::CubicFade) == 1);
    CHECK(static_cast<uint32>(TerrainNoiseLayout::QuinticFade) == 2);
    CHECK(TERRAIN_NOISE_LAYOUT_VERSION == static_cast<uint32>(TerrainNoiseLayout::QuinticFade));
    CHECK(TerrainNoiseShape{}.Layout == LATEST_TERRAIN_NOISE_LAYOUT);

    TerrainNoiseLayout layout = TerrainNoiseLayout::QuinticFade;
    CHECK(TryGetTerrainNoiseLayout(1, layout) == true);
    CHECK(layout == TerrainNoiseLayout::CubicFade);
    CHECK(TryGetTerrainNoiseLayout(2, layout) == true);
    CHECK(layout == TerrainNoiseLayout::QuinticFade);
    for (const uint32 unknown : {0U, 3U, 0xffffffffU})
    {
        CHECK(TryGetTerrainNoiseLayout(unknown, layout) == false);
        CHECK(layout == TerrainNoiseLayout::QuinticFade);
    }
}

TEST_CASE("terrain noise layouts share lattice addresses and gradients", "[content][terrain]")
{
    // Both fades equal 0, 1/2 and 1 at those offsets, so any difference there would be a gradient or address change.
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    bool hasDifference = false;
    for (uint32 octave = 0; octave < TERRAIN_NOISE_OCTAVES; ++octave)
    {
        for (int32 z = -3; z <= 3; ++z)
        {
            for (int32 x = -3; x <= 3; ++x)
            {
                for (const float64 offsetX : {0.0, 0.5})
                {
                    for (const float64 offsetZ : {0.0, 0.5})
                    {
                        CHECK(
                            SampleTerrainNoise(key, TerrainNoiseLayout::CubicFade, octave, x + offsetX, z + offsetZ) ==
                            SampleTerrainNoise(key, TerrainNoiseLayout::QuinticFade, octave, x + offsetX, z + offsetZ));
                    }
                }
                hasDifference =
                    hasDifference ||
                    SampleTerrainNoise(key, TerrainNoiseLayout::CubicFade, octave, x + 0.25, z + 0.75) !=
                        SampleTerrainNoise(key, TerrainNoiseLayout::QuinticFade, octave, x + 0.25, z + 0.75);
            }
        }
    }
    CHECK(hasDifference == true);
}

TEST_CASE("terrain noise values and slopes are continuous across a lattice edge", "[content][terrain]")
{
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    for (const TerrainNoiseLayout layout : {TerrainNoiseLayout::CubicFade, TerrainNoiseLayout::QuinticFade})
    {
        for (const float64 positionZ : SEAM_ACROSS_Z)
        {
            const Seam seam = MeasureSeam(key, layout, SEAM_EDGE_X, positionZ);
            INFO("layout " << static_cast<uint32>(layout) << " z " << positionZ);
            // The edge sample itself belongs to the upper cell; both limits must reach it.
            const float64 edge = SampleTerrainNoise(key, layout, 0, SEAM_EDGE_X, positionZ);
            CHECK(math::Abs(seam.Left.Value - edge) < SEAM_VALUE_TOLERANCE);
            CHECK(math::Abs(seam.Right.Value - edge) < SEAM_VALUE_TOLERANCE);
            CHECK(math::Abs(seam.Left.Slope - seam.Right.Slope) < SEAM_SLOPE_TOLERANCE);
        }
    }
}

TEST_CASE("terrain noise layout 1 keeps the cubic second-derivative jump that layout 2 closes", "[content][terrain]")
{
    // Gradients and addresses are the same for both layouts (see the shared-gradient case), so the fade alone moves
    // the second derivative. The cubic's is 6 (1 - 2t) times the gradient difference, nonzero at both cell ends; the
    // quintic's vanishes there.
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    for (const float64 positionZ : SEAM_ACROSS_Z)
    {
        const Seam cubic = MeasureSeam(key, TerrainNoiseLayout::CubicFade, SEAM_EDGE_X, positionZ);
        const Seam quintic = MeasureSeam(key, TerrainNoiseLayout::QuinticFade, SEAM_EDGE_X, positionZ);
        const float64 cubicJump = cubic.Right.Curvature - cubic.Left.Curvature;
        const float64 quinticJump = quintic.Right.Curvature - quintic.Left.Curvature;
        INFO("z " << positionZ << " cubic jump " << cubicJump << " quintic jump " << quinticJump);
        CHECK(math::Abs(cubicJump) > MIN_CUBIC_CURVATURE_JUMP);
        CHECK(math::Abs(quinticJump) < SEAM_CURVATURE_TOLERANCE);
        CHECK(math::Abs(quintic.Left.Curvature) < SEAM_CURVATURE_TOLERANCE);
        CHECK(math::Abs(quintic.Right.Curvature) < SEAM_CURVATURE_TOLERANCE);
    }
}

TEST_CASE("terrain noise layout 2 golden digest", "[content][terrain]")
{
    const math::RandomKey key = MakeKey(GOLDEN_SEED);
    TerrainNoiseShape quintic = MakeHills();
    quintic.Layout = TerrainNoiseLayout::QuinticFade;
    const uint64 digest = ComputeGridDigest(quintic, key, GOLDEN_SAMPLES);
    INFO("digest " << digest);
    CHECK(digest == GOLDEN_QUINTIC_HILLS_DIGEST);
    CHECK(digest != GOLDEN_HILLS_DIGEST);
}
