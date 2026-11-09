// Derived field contracts: slope on analytic surfaces, drainage on a tilted
// plane, a bowl, a flat domain and a saddle, conservation at the outlets,
// acyclic routing, reproducibility and failure preservation. See
// docs/architecture/terrain-generation.md#validation-and-delivery.

#include <ludus/content/terrain/terrain_fields.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::terrain;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::core::float64;
namespace math = ludus::foundation::math;

namespace
{
constexpr uint32 TEST_COLUMNS = 9;
constexpr uint32 TEST_ROWS = 7;
constexpr float32 TEST_SPACING_METRES = 2;
constexpr float32 LARGEST_FINITE = 3.0e38F;

/// Infinity produced at run time, so no literal exceeds the float range.
[[nodiscard]] float32 MakeNonFinite()
{
    const float32 large = LARGEST_FINITE;
    return large * large;
}

/// Storage of one drainage solve.
struct Drainage final
{
    Array<float32> Filled;
    Array<TerrainFlowDirection> Direction;
    Array<uint32> Accumulation;
    TerrainDrainageSummary Summary;

    explicit Drainage(usize samples)
        : Filled(samples, 0.0F), Direction(samples, TerrainFlowDirection::Outlet), Accumulation(samples, 0U)
    {
    }

    [[nodiscard]] TerrainFieldStatus Solve(const TerrainFieldGrid& grid, const Array<float32>& elevation)
    {
        return TryComputeTerrainDrainage(grid,
                                         BorrowedView<const float32>(elevation.GetData(), elevation.GetSize()),
                                         BorrowedView<float32>(Filled.GetData(), Filled.GetSize()),
                                         BorrowedView<TerrainFlowDirection>(Direction.GetData(), Direction.GetSize()),
                                         BorrowedView<uint32>(Accumulation.GetData(), Accumulation.GetSize()),
                                         Summary);
    }
};

[[nodiscard]] TerrainFieldGrid MakeGrid()
{
    return { .Columns = TEST_COLUMNS, .Rows = TEST_ROWS, .SpacingMetres = TEST_SPACING_METRES };
}

template <typename Surface>
[[nodiscard]] Array<float32> MakeElevation(const TerrainFieldGrid& grid, const Surface& surface)
{
    Array<float32> elevation(GetTerrainSampleCount(grid), 0.0F);
    for (uint32 z = 0; z < grid.Rows; ++z)
    {
        for (uint32 x = 0; x < grid.Columns; ++x)
        {
            elevation[z * grid.Columns + x] = surface(x, z);
        }
    }
    return elevation;
}

/// Follow the routing from a sample; returns the steps to an Outlet, or zero when it never arrives.
[[nodiscard]] usize CountStepsToOutlet(const TerrainFieldGrid& grid, const Drainage& drainage, uint32 start)
{
    const usize samples = GetTerrainSampleCount(grid);
    uint32 sample = start;
    for (usize step = 1; step <= samples; ++step)
    {
        const TerrainFlowDirection direction = drainage.Direction[sample];
        if (direction == TerrainFlowDirection::Outlet)
        {
            return step;
        }
        const TerrainFlowOffset offset = GetTerrainFlowOffset(direction);
        const int32 x = static_cast<int32>(sample % grid.Columns) + offset.X;
        const int32 z = static_cast<int32>(sample / grid.Columns) + offset.Z;
        REQUIRE(x >= 0);
        REQUIRE(z >= 0);
        REQUIRE(static_cast<uint32>(x) < grid.Columns);
        REQUIRE(static_cast<uint32>(z) < grid.Rows);
        sample = static_cast<uint32>(z) * grid.Columns + static_cast<uint32>(x);
    }
    return 0;
}

void CheckDrainageInvariants(const TerrainFieldGrid& grid, const Array<float32>& elevation, const Drainage& drainage)
{
    const usize samples = GetTerrainSampleCount(grid);
    CHECK(drainage.Summary.OutletAccumulation == samples);
    CHECK(drainage.Summary.OutletSamples > 0);
    for (uint32 sample = 0; sample < samples; ++sample)
    {
        CHECK(drainage.Filled[sample] >= elevation[sample]);
        CHECK(drainage.Accumulation[sample] >= 1);
        CHECK(CountStepsToOutlet(grid, drainage, sample) > 0);
        if (drainage.Direction[sample] != TerrainFlowDirection::Outlet)
        {
            const TerrainFlowOffset offset = GetTerrainFlowOffset(drainage.Direction[sample]);
            const uint32 target =
                static_cast<uint32>(static_cast<int32>(sample / grid.Columns) + offset.Z) * grid.Columns +
                static_cast<uint32>(static_cast<int32>(sample % grid.Columns) + offset.X);
            CHECK(drainage.Filled[target] <= drainage.Filled[sample]);
        }
        else
        {
            const uint32 x = sample % grid.Columns;
            const uint32 z = sample / grid.Columns;
            CHECK((x == 0 || z == 0 || x + 1 == grid.Columns || z + 1 == grid.Rows));
        }
    }
}
} // namespace

TEST_CASE("terrain field grid validation", "[content][terrain]")
{
    CHECK(IsTerrainFieldGridValid(MakeGrid()) == true);
    CHECK(IsTerrainFieldGridValid({ .Columns = 1, .Rows = 4, .SpacingMetres = 1 }) == false);
    CHECK(IsTerrainFieldGridValid({ .Columns = 4, .Rows = MAX_TERRAIN_FIELD_AXIS_SAMPLES + 1, .SpacingMetres = 1 }) ==
          false);
    CHECK(IsTerrainFieldGridValid({ .Columns = 4, .Rows = 4, .SpacingMetres = 0 }) == false);
    CHECK(IsTerrainFieldGridValid({ .Columns = 4, .Rows = 4, .SpacingMetres = MakeNonFinite() }) == false);
    CHECK(GetTerrainSampleCount(MakeGrid()) == TEST_COLUMNS * TEST_ROWS);
}

TEST_CASE("terrain slope of a plane is its gradient", "[content][terrain]")
{
    const TerrainFieldGrid grid = MakeGrid();
    constexpr float32 RISE_PER_SAMPLE_X = 0.5F;
    constexpr float32 RISE_PER_SAMPLE_Z = 1.0F;
    const Array<float32> elevation = MakeElevation(grid, [](uint32 x, uint32 z) {
        return RISE_PER_SAMPLE_X * static_cast<float32>(x) + RISE_PER_SAMPLE_Z * static_cast<float32>(z);
    });
    Array<float32> slope(GetTerrainSampleCount(grid), -1.0F);
    REQUIRE(TryComputeTerrainSlope(grid,
                                   BorrowedView<const float32>(elevation.GetData(), elevation.GetSize()),
                                   BorrowedView<float32>(slope.GetData(), slope.GetSize())) ==
            TerrainFieldStatus::Success);
    const float32 gradientX = RISE_PER_SAMPLE_X / TEST_SPACING_METRES;
    const float32 gradientZ = RISE_PER_SAMPLE_Z / TEST_SPACING_METRES;
    const float32 expected = math::Sqrt(gradientX * gradientX + gradientZ * gradientZ);
    for (usize sample = 0; sample < slope.GetSize(); ++sample)
    {
        CHECK(slope[sample] == Catch::Approx(expected));
    }

    SECTION("failures preserve the output")
    {
        Array<float32> untouched(GetTerrainSampleCount(grid), -1.0F);
        const BorrowedView<float32> outSlope(untouched.GetData(), untouched.GetSize());
        CHECK(TryComputeTerrainSlope(grid, BorrowedView<const float32>(elevation.GetData(), 3), outSlope) ==
              TerrainFieldStatus::InvalidStorage);
        Array<float32> broken = elevation;
        broken[4] = MakeNonFinite();
        CHECK(TryComputeTerrainSlope(grid, BorrowedView<const float32>(broken.GetData(), broken.GetSize()), outSlope) ==
              TerrainFieldStatus::NonFiniteInput);
        CHECK(TryComputeTerrainSlope({ .Columns = 1, .Rows = 1, .SpacingMetres = 1 },
                                     BorrowedView<const float32>(elevation.GetData(), 1),
                                     BorrowedView<float32>(untouched.GetData(), 1)) == TerrainFieldStatus::InvalidGrid);
        for (usize sample = 0; sample < untouched.GetSize(); ++sample)
        {
            CHECK(untouched[sample] == -1.0F);
        }
    }
}

TEST_CASE("terrain drainage on a tilted plane flows downhill to one edge", "[content][terrain]")
{
    const TerrainFieldGrid grid = MakeGrid();
    const Array<float32> elevation = MakeElevation(grid, [](uint32 x, uint32) { return static_cast<float32>(x); });
    Drainage drainage(GetTerrainSampleCount(grid));
    REQUIRE(drainage.Solve(grid, elevation) == TerrainFieldStatus::Success);
    CheckDrainageInvariants(grid, elevation, drainage);
    CHECK(drainage.Summary.FilledSamples == 0);
    CHECK(drainage.Summary.MaximumFillMetres == 0);
    CHECK(drainage.Summary.OutletSamples == TEST_ROWS);
    for (uint32 z = 0; z < grid.Rows; ++z)
    {
        CHECK(drainage.Direction[z * grid.Columns] == TerrainFlowDirection::Outlet);
        CHECK(drainage.Accumulation[z * grid.Columns] == TEST_COLUMNS);
        for (uint32 x = 1; x < grid.Columns; ++x)
        {
            CHECK(drainage.Direction[z * grid.Columns + x] == TerrainFlowDirection::NegativeX);
            CHECK(drainage.Accumulation[z * grid.Columns + x] == TEST_COLUMNS - x);
        }
    }
}

TEST_CASE("terrain drainage fills a bowl to its spill level", "[content][terrain]")
{
    const TerrainFieldGrid grid = MakeGrid();
    constexpr float32 RIM_METRES = 10;
    constexpr float32 SPILL_METRES = 4;
    constexpr float32 FLOOR_METRES = 1;
    const Array<float32> elevation = MakeElevation(grid, [&](uint32 x, uint32 z) {
        const bool isBorder = x == 0 || z == 0 || x + 1 == grid.Columns || z + 1 == grid.Rows;
        if (isBorder == true)
        {
            return x == grid.Columns / 2 && z == 0 ? SPILL_METRES : RIM_METRES;
        }
        return FLOOR_METRES;
    });
    Drainage drainage(GetTerrainSampleCount(grid));
    REQUIRE(drainage.Solve(grid, elevation) == TerrainFieldStatus::Success);
    CheckDrainageInvariants(grid, elevation, drainage);
    CHECK(drainage.Summary.FilledSamples == (TEST_COLUMNS - 2) * (TEST_ROWS - 2));
    CHECK(drainage.Summary.MaximumFillMetres == Catch::Approx(SPILL_METRES - FLOOR_METRES));
    for (uint32 z = 1; z + 1 < grid.Rows; ++z)
    {
        for (uint32 x = 1; x + 1 < grid.Columns; ++x)
        {
            CHECK(drainage.Filled[z * grid.Columns + x] == SPILL_METRES);
        }
    }
    // The rim drains into the lake and the lake leaves through the spill, the only outlet.
    const uint32 spill = grid.Columns / 2;
    CHECK(drainage.Summary.OutletSamples == 1);
    CHECK(drainage.Direction[spill] == TerrainFlowDirection::Outlet);
    CHECK(drainage.Accumulation[spill] == GetTerrainSampleCount(grid));
}

TEST_CASE("terrain drainage routes a flat domain without cycles", "[content][terrain]")
{
    const TerrainFieldGrid grid = MakeGrid();
    const Array<float32> elevation = MakeElevation(grid, [](uint32, uint32) { return 3.0F; });
    Drainage drainage(GetTerrainSampleCount(grid));
    REQUIRE(drainage.Solve(grid, elevation) == TerrainFieldStatus::Success);
    CheckDrainageInvariants(grid, elevation, drainage);
    CHECK(drainage.Summary.FilledSamples == 0);
    // Every border sample is its own outlet on a flat domain.
    CHECK(drainage.Summary.OutletSamples == 2 * TEST_COLUMNS + 2 * (TEST_ROWS - 2));
}

TEST_CASE("terrain drainage splits a saddle between two basins", "[content][terrain]")
{
    const TerrainFieldGrid grid = MakeGrid();
    const uint32 ridge = grid.Columns / 2;
    const Array<float32> elevation = MakeElevation(grid, [&](uint32 x, uint32) {
        const uint32 distance = x > ridge ? x - ridge : ridge - x;
        return static_cast<float32>(ridge - distance);
    });
    Drainage drainage(GetTerrainSampleCount(grid));
    REQUIRE(drainage.Solve(grid, elevation) == TerrainFieldStatus::Success);
    CheckDrainageInvariants(grid, elevation, drainage);
    uint64 west = 0;
    uint64 east = 0;
    for (uint32 z = 0; z < grid.Rows; ++z)
    {
        west += drainage.Accumulation[z * grid.Columns];
        east += drainage.Accumulation[z * grid.Columns + grid.Columns - 1];
    }
    // The ridge samples tie between -X and +X and resolve to the first direction in enum order (+X).
    CHECK(east == west + TEST_ROWS);
    CHECK(east + west == GetTerrainSampleCount(grid));
}

TEST_CASE("terrain drainage of a noisy surface is conservative and reproducible", "[content][terrain]")
{
    constexpr uint32 NOISY_COLUMNS = 33;
    constexpr uint32 NOISY_ROWS = 25;
    constexpr uint32 NOISE_DOMAIN = 5;
    constexpr float32 NOISE_SCALE = 1.0F / 4294967296.0F;
    const TerrainFieldGrid grid{ .Columns = NOISY_COLUMNS, .Rows = NOISY_ROWS, .SpacingMetres = 1 };
    math::RandomKey key;
    REQUIRE(math::TryMakeRandomKey(3, NOISE_DOMAIN, key) == math::MathStatus::Success);
    const Array<float32> elevation = MakeElevation(grid, [&](uint32 x, uint32 z) {
        const uint64 scope = (static_cast<uint64>(x) << 32U) | z;
        return static_cast<float32>(math::SampleUInt32(key, { .Scope = scope, .Event = 0 })) * NOISE_SCALE;
    });
    Drainage first(GetTerrainSampleCount(grid));
    Drainage second(GetTerrainSampleCount(grid));
    REQUIRE(first.Solve(grid, elevation) == TerrainFieldStatus::Success);
    REQUIRE(second.Solve(grid, elevation) == TerrainFieldStatus::Success);
    CheckDrainageInvariants(grid, elevation, first);
    CHECK(first.Summary.FilledSamples > 0);
    for (usize sample = 0; sample < GetTerrainSampleCount(grid); ++sample)
    {
        CHECK(first.Filled[sample] == second.Filled[sample]);
        CHECK(first.Direction[sample] == second.Direction[sample]);
        CHECK(first.Accumulation[sample] == second.Accumulation[sample]);
    }
}

TEST_CASE("terrain drainage failures preserve the outputs", "[content][terrain]")
{
    const TerrainFieldGrid grid = MakeGrid();
    Array<float32> elevation = MakeElevation(grid, [](uint32 x, uint32) { return static_cast<float32>(x); });
    Drainage drainage(GetTerrainSampleCount(grid));
    drainage.Accumulation[0] = 77;
    CHECK(TryComputeTerrainDrainage({ .Columns = 1, .Rows = 2, .SpacingMetres = 1 },
                                    BorrowedView<const float32>(elevation.GetData(), 2),
                                    BorrowedView<float32>(drainage.Filled.GetData(), 2),
                                    BorrowedView<TerrainFlowDirection>(drainage.Direction.GetData(), 2),
                                    BorrowedView<uint32>(drainage.Accumulation.GetData(), 2),
                                    drainage.Summary) == TerrainFieldStatus::InvalidGrid);
    CHECK(TryComputeTerrainDrainage(
              grid,
              BorrowedView<const float32>(elevation.GetData(), elevation.GetSize()),
              BorrowedView<float32>(drainage.Filled.GetData(), 2),
              BorrowedView<TerrainFlowDirection>(drainage.Direction.GetData(), drainage.Direction.GetSize()),
              BorrowedView<uint32>(drainage.Accumulation.GetData(), drainage.Accumulation.GetSize()),
              drainage.Summary) == TerrainFieldStatus::InvalidStorage);
    elevation[5] = MakeNonFinite();
    CHECK(drainage.Solve(grid, elevation) == TerrainFieldStatus::NonFiniteInput);
    CHECK(drainage.Accumulation[0] == 77);
    CHECK(drainage.Filled[0] == 0);
    CHECK(drainage.Summary.OutletSamples == 0);
}

TEST_CASE("terrain wetness grows with catchment and falls with slope", "[content][terrain]")
{
    const TerrainFieldGrid grid{ .Columns = 4, .Rows = 2, .SpacingMetres = 1 };
    const Array<float32> slope{0.0F, 0.5F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.0F};
    const Array<uint32> accumulation{1U, 1U, 1000U, 1000U, 1U, 1U, 1U, 1U};
    Array<float32> wetness(GetTerrainSampleCount(grid), -1.0F);
    const TerrainWetnessParameters parameters;
    REQUIRE(TryComputeTerrainWetness(grid,
                                     BorrowedView<const float32>(slope.GetData(), slope.GetSize()),
                                     BorrowedView<const uint32>(accumulation.GetData(), accumulation.GetSize()),
                                     parameters,
                                     BorrowedView<float32>(wetness.GetData(), wetness.GetSize())) ==
            TerrainFieldStatus::Success);
    for (usize sample = 0; sample < wetness.GetSize(); ++sample)
    {
        CHECK(wetness[sample] >= 0);
        CHECK(wetness[sample] <= 1);
    }
    CHECK(wetness[2] > wetness[0]);
    CHECK(wetness[1] < wetness[0]);
    CHECK(wetness[3] < wetness[2]);
    // ln(1 / 0.001) = 6.9 lies between the default bounds 2 and 12.
    const float64 expectedIndex = math::Log(1.0 / static_cast<float64>(parameters.MinimumSlope));
    const float64 expectedWetness =
        (expectedIndex - static_cast<float64>(parameters.MinimumIndex)) /
        (static_cast<float64>(parameters.MaximumIndex) - static_cast<float64>(parameters.MinimumIndex));
    CHECK(static_cast<float64>(wetness[0]) == Catch::Approx(expectedWetness));

    SECTION("parameter and input failures preserve the output")
    {
        Array<float32> untouched(GetTerrainSampleCount(grid), -1.0F);
        const BorrowedView<float32> outWetness(untouched.GetData(), untouched.GetSize());
        const BorrowedView<const float32> slopeView(slope.GetData(), slope.GetSize());
        const BorrowedView<const uint32> accumulationView(accumulation.GetData(), accumulation.GetSize());
        TerrainWetnessParameters reversed = parameters;
        reversed.MaximumIndex = reversed.MinimumIndex;
        CHECK(TryComputeTerrainWetness(grid, slopeView, accumulationView, reversed, outWetness) ==
              TerrainFieldStatus::InvalidArgument);
        TerrainWetnessParameters zeroSlope = parameters;
        zeroSlope.MinimumSlope = 0;
        CHECK(TryComputeTerrainWetness(grid, slopeView, accumulationView, zeroSlope, outWetness) ==
              TerrainFieldStatus::InvalidArgument);
        const Array<uint32> emptyCatchment{0U, 1U, 1U, 1U, 1U, 1U, 1U, 1U};
        CHECK(TryComputeTerrainWetness(grid,
                                       slopeView,
                                       BorrowedView<const uint32>(emptyCatchment.GetData(), emptyCatchment.GetSize()),
                                       parameters,
                                       outWetness) == TerrainFieldStatus::InvalidArgument);
        CHECK(TryComputeTerrainWetness(grid,
                                       BorrowedView<const float32>(slope.GetData(), 2),
                                       accumulationView,
                                       parameters,
                                       outWetness) == TerrainFieldStatus::InvalidStorage);
        for (usize sample = 0; sample < untouched.GetSize(); ++sample)
        {
            CHECK(untouched[sample] == -1.0F);
        }
    }
}
