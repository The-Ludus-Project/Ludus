// River contracts: parameter validation, a hand-built confluence with exact
// segments and Strahler orders, capacity reporting, routing rejection, network
// invariants on generated terrain, and carving that never raises terrain, cuts
// a descending bed, stays inside its stencil and preserves outputs on failure.
// See docs/modules/content/terrain.md#river-channels.

#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/content/terrain/terrain_noise.h>
#include <ludus/content/terrain/terrain_rivers.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::terrain;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::core::float64;
using ludus::foundation::core::int64;
namespace math = ludus::foundation::math;

namespace
{
constexpr uint32 CONFLUENCE_SIDE = 7;
constexpr usize CONFLUENCE_SAMPLES = static_cast<usize>(CONFLUENCE_SIDE) * CONFLUENCE_SIDE;
constexpr float32 LARGEST_FINITE = 3.0e38F;

[[nodiscard]] float32 MakeNonFinite()
{
    const float32 large = LARGEST_FINITE;
    return large * large;
}

[[nodiscard]] uint32 GetIndex(const TerrainFieldGrid& grid, uint32 x, uint32 z)
{
    return z * grid.Columns + x;
}

/// Contributing samples for a hand-built routing, by walking every sample to its outlet.
[[nodiscard]] Array<uint32> Accumulate(const TerrainFieldGrid& grid, const Array<TerrainFlowDirection>& directions)
{
    Array<uint32> accumulation(GetTerrainSampleCount(grid), 0U);
    for (uint32 start = 0; start < accumulation.GetSize(); ++start)
    {
        uint32 sample = start;
        while (true)
        {
            ++accumulation[sample];
            if (directions[sample] == TerrainFlowDirection::Outlet)
            {
                break;
            }
            const TerrainFlowOffset offset = GetTerrainFlowOffset(directions[sample]);
            sample = GetIndex(grid,
                              static_cast<uint32>(static_cast<int32>(sample % grid.Columns) + offset.X),
                              static_cast<uint32>(static_cast<int32>(sample / grid.Columns) + offset.Z));
        }
    }
    return accumulation;
}

/// Two tributaries in columns 2 and 4 that meet at (3, 3) and drain down column 3 to row 0.
[[nodiscard]] Array<TerrainFlowDirection> MakeConfluence(const TerrainFieldGrid& grid)
{
    Array<TerrainFlowDirection> directions(GetTerrainSampleCount(grid), TerrainFlowDirection::Outlet);
    for (uint32 z = 5; z < grid.Rows; ++z)
    {
        directions[GetIndex(grid, 2, z)] = TerrainFlowDirection::NegativeZ;
        directions[GetIndex(grid, 4, z)] = TerrainFlowDirection::NegativeZ;
    }
    directions[GetIndex(grid, 2, 4)] = TerrainFlowDirection::PositiveXNegativeZ;
    directions[GetIndex(grid, 4, 4)] = TerrainFlowDirection::NegativeXNegativeZ;
    for (uint32 z = 1; z <= 3; ++z)
    {
        directions[GetIndex(grid, 3, z)] = TerrainFlowDirection::NegativeZ;
    }
    return directions;
}

/// Network storage of one extraction.
struct Network final
{
    Array<uint32> Points;
    Array<TerrainRiverSegment> Segments;
    TerrainRiverNetworkSummary Summary;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): point and segment capacities are distinct.
    Network(usize points, usize segments) : Points(points, 0U), Segments(segments) {}

    [[nodiscard]] TerrainRiverStatus Extract(const TerrainFieldGrid& grid,
                                             const Array<TerrainFlowDirection>& directions,
                                             const Array<uint32>& accumulation,
                                             const TerrainRiverParameters& parameters)
    {
        return TryExtractTerrainRivers(
            grid,
            BorrowedView<const TerrainFlowDirection>(directions.GetData(), directions.GetSize()),
            BorrowedView<const uint32>(accumulation.GetData(), accumulation.GetSize()),
            parameters,
            BorrowedView<uint32>(Points.GetData(), Points.GetSize()),
            BorrowedView<TerrainRiverSegment>(Segments.GetData(), Segments.GetSize()),
            Summary);
    }
};

/// Drainage of one elevation field.
struct Drainage final
{
    Array<float32> Filled;
    Array<TerrainFlowDirection> Direction;
    Array<uint32> Accumulation;
    TerrainDrainageSummary Summary;

    Drainage(const TerrainFieldGrid& grid, const Array<float32>& elevation)
        : Filled(GetTerrainSampleCount(grid), 0.0F),
          Direction(GetTerrainSampleCount(grid), TerrainFlowDirection::Outlet),
          Accumulation(GetTerrainSampleCount(grid), 0U)
    {
        REQUIRE(TryComputeTerrainDrainage(grid,
                                          BorrowedView<const float32>(elevation.GetData(), elevation.GetSize()),
                                          BorrowedView<float32>(Filled.GetData(), Filled.GetSize()),
                                          BorrowedView<TerrainFlowDirection>(Direction.GetData(), Direction.GetSize()),
                                          BorrowedView<uint32>(Accumulation.GetData(), Accumulation.GetSize()),
                                          Summary) == TerrainFieldStatus::Success);
    }
};

/// Carve storage of one run.
struct Carve final
{
    Array<float32> Carved;
    Array<float32> Surface;
    Array<float32> Depth;
    TerrainRiverCarveSummary Summary;

    explicit Carve(usize samples) : Carved(samples, -1.0F), Surface(samples, -1.0F), Depth(samples, -1.0F) {}

    [[nodiscard]] TerrainRiverStatus Run(const TerrainFieldGrid& grid,
                                         const Array<float32>& elevation,
                                         const Drainage& drainage,
                                         const TerrainRiverParameters& parameters)
    {
        return TryCarveTerrainRivers(
            grid,
            BorrowedView<const float32>(elevation.GetData(), elevation.GetSize()),
            BorrowedView<const float32>(drainage.Filled.GetData(), drainage.Filled.GetSize()),
            BorrowedView<const uint32>(drainage.Accumulation.GetData(), drainage.Accumulation.GetSize()),
            parameters,
            BorrowedView<float32>(Carved.GetData(), Carved.GetSize()),
            BorrowedView<float32>(Surface.GetData(), Surface.GetSize()),
            BorrowedView<float32>(Depth.GetData(), Depth.GetSize()),
            Summary);
    }
};

/// A valley along Z at the centre column, tilted so it drains toward row 0.
[[nodiscard]] Array<float32> MakeValley(const TerrainFieldGrid& grid)
{
    constexpr float32 SIDE_SLOPE = 1.0F;
    constexpr float32 VALLEY_SLOPE = 0.25F;
    Array<float32> elevation(GetTerrainSampleCount(grid), 0.0F);
    const int32 centre = static_cast<int32>(grid.Columns / 2);
    for (uint32 z = 0; z < grid.Rows; ++z)
    {
        for (uint32 x = 0; x < grid.Columns; ++x)
        {
            const float32 across =
                static_cast<float32>(math::Abs(static_cast<float64>(static_cast<int32>(x) - centre)));
            elevation[GetIndex(grid, x, z)] = across * SIDE_SLOPE * grid.SpacingXMetres +
                                              static_cast<float32>(z) * VALLEY_SLOPE * grid.SpacingZMetres;
        }
    }
    return elevation;
}

[[nodiscard]] TerrainRiverParameters MakeSampleCountParameters(float32 minimumSamples, const TerrainFieldGrid& grid)
{
    TerrainRiverParameters parameters;
    parameters.MinimumCatchmentSquareMetres = minimumSamples * GetTerrainSampleAreaSquareMetres(grid);
    return parameters;
}
} // namespace

TEST_CASE("terrain river parameter validation", "[content][terrain]")
{
    const TerrainRiverParameters defaults;
    CHECK(AreTerrainRiverParametersValid(defaults) == true);
    TerrainRiverParameters parameters = defaults;
    parameters.MinimumCatchmentSquareMetres = 0;
    CHECK(AreTerrainRiverParametersValid(parameters) == false);
    parameters = defaults;
    parameters.DepthExponent = 1.5F;
    CHECK(AreTerrainRiverParametersValid(parameters) == false);
    parameters = defaults;
    parameters.WidthCoefficientMetres = -1;
    CHECK(AreTerrainRiverParametersValid(parameters) == false);
    parameters = defaults;
    parameters.BankSlope = 0;
    CHECK(AreTerrainRiverParametersValid(parameters) == false);
    parameters = defaults;
    parameters.WaterFraction = MakeNonFinite();
    CHECK(AreTerrainRiverParametersValid(parameters) == false);
}

TEST_CASE("terrain rivers split a confluence into ordered segments", "[content][terrain]")
{
    const TerrainFieldGrid grid
    {
        .Columns = CONFLUENCE_SIDE,
        .Rows = CONFLUENCE_SIDE,
        .SpacingXMetres = 1,
        .SpacingZMetres = 1,
    };
    const Array<TerrainFlowDirection> directions = MakeConfluence(grid);
    const Array<uint32> accumulation = Accumulate(grid, directions);
    REQUIRE(accumulation[GetIndex(grid, 3, 0)] == 10);
    const TerrainRiverParameters parameters = MakeSampleCountParameters(2, grid);
    Network network(CONFLUENCE_SAMPLES, CONFLUENCE_SAMPLES);
    REQUIRE(network.Extract(grid, directions, accumulation, parameters) == TerrainRiverStatus::Success);
    CHECK(network.Summary.ChannelSamples == 8);
    CHECK(network.Summary.SegmentCount == 3);
    CHECK(network.Summary.PointCount == 10);
    CHECK(network.Summary.OutletSegments == 1);
    CHECK(network.Summary.MaximumOrder == 2);

    // Heads in ascending sample index: the confluence (3, 3), then the tributaries (2, 5) and (4, 5).
    const TerrainRiverSegment& trunk = network.Segments[0];
    CHECK(trunk.PointCount == 4);
    CHECK(trunk.DownstreamSegment == TERRAIN_RIVER_NONE);
    CHECK(trunk.Order == 2);
    CHECK(network.Points[trunk.FirstPoint] == GetIndex(grid, 3, 3));
    CHECK(network.Points[trunk.FirstPoint + 3] == GetIndex(grid, 3, 0));
    for (uint32 tributary = 1; tributary <= 2; ++tributary)
    {
        const TerrainRiverSegment& segment = network.Segments[tributary];
        CHECK(segment.PointCount == 3);
        CHECK(segment.DownstreamSegment == 0);
        CHECK(segment.Order == 1);
        CHECK(network.Points[segment.FirstPoint + 2] == GetIndex(grid, 3, 3));
    }
    CHECK(network.Points[network.Segments[1].FirstPoint] == GetIndex(grid, 2, 5));
    CHECK(network.Points[network.Segments[2].FirstPoint] == GetIndex(grid, 4, 5));

    SECTION("too little storage reports the required counts and writes nothing else")
    {
        Network small(9, 3);
        small.Points[0] = 77;
        CHECK(small.Extract(grid, directions, accumulation, parameters) == TerrainRiverStatus::CapacityExceeded);
        CHECK(small.Summary.PointCount == 10);
        CHECK(small.Summary.SegmentCount == 3);
        CHECK(small.Points[0] == 77);
        CHECK(small.Segments[0].PointCount == 0);
    }

    SECTION("a raised threshold keeps only the trunk below the confluence")
    {
        Network trunkOnly(CONFLUENCE_SAMPLES, CONFLUENCE_SAMPLES);
        REQUIRE(trunkOnly.Extract(grid, directions, accumulation, MakeSampleCountParameters(7, grid)) ==
                TerrainRiverStatus::Success);
        CHECK(trunkOnly.Summary.SegmentCount == 1);
        CHECK(trunkOnly.Summary.MaximumOrder == 1);
        CHECK(trunkOnly.Points[0] == GetIndex(grid, 3, 3));
    }
}

TEST_CASE("terrain rivers reject inconsistent routing and storage", "[content][terrain]")
{
    const TerrainFieldGrid grid
    {
        .Columns = CONFLUENCE_SIDE,
        .Rows = CONFLUENCE_SIDE,
        .SpacingXMetres = 1,
        .SpacingZMetres = 1,
    };
    Array<TerrainFlowDirection> directions = MakeConfluence(grid);
    Array<uint32> accumulation = Accumulate(grid, directions);
    const TerrainRiverParameters parameters = MakeSampleCountParameters(2, grid);
    Network network(CONFLUENCE_SAMPLES, CONFLUENCE_SAMPLES);
    network.Points[0] = 77;

    Array<uint32> flat = accumulation;
    flat[GetIndex(grid, 3, 2)] = flat[GetIndex(grid, 3, 3)];
    CHECK(network.Extract(grid, directions, flat, parameters) == TerrainRiverStatus::InvalidRouting);
    directions[GetIndex(grid, 0, 3)] = TerrainFlowDirection::NegativeX;
    CHECK(network.Extract(grid, directions, accumulation, parameters) == TerrainRiverStatus::InvalidRouting);
    directions = MakeConfluence(grid);
    TerrainRiverParameters invalid = parameters;
    invalid.BankSlope = -1;
    CHECK(network.Extract(grid, directions, accumulation, invalid) == TerrainRiverStatus::InvalidArgument);
    CHECK(TryExtractTerrainRivers(
              grid,
              BorrowedView<const TerrainFlowDirection>(directions.GetData(), 3),
              BorrowedView<const uint32>(accumulation.GetData(), accumulation.GetSize()),
              parameters,
              BorrowedView<uint32>(network.Points.GetData(), network.Points.GetSize()),
              BorrowedView<TerrainRiverSegment>(network.Segments.GetData(), network.Segments.GetSize()),
              network.Summary) == TerrainRiverStatus::InvalidStorage);
    CHECK(network.Points[0] == 77);
    CHECK(network.Summary.SegmentCount == 0);
}

TEST_CASE("terrain rivers on generated terrain form a joined, ordered forest", "[content][terrain]")
{
    constexpr uint32 SIDE = 65;
    constexpr uint32 NOISE_DOMAIN = 1;
    constexpr uint64 SEED = 11;
    constexpr float32 SPACING_METRES = 4;
    constexpr float64 SPACING_METRES_WIDE = static_cast<float64>(SPACING_METRES);
    const TerrainFieldGrid grid
    {
        .Columns = SIDE,
        .Rows = SIDE,
        .SpacingXMetres = SPACING_METRES,
        .SpacingZMetres = SPACING_METRES,
    };
    math::RandomKey key;
    REQUIRE(math::TryMakeRandomKey(SEED, NOISE_DOMAIN, key) == math::MathStatus::Success);
    const TerrainNoiseShape shape
    {
        .MinimumElevationMetres = 0,
        .MaximumElevationMetres = 40,
        .FeatureSizeMetres = 96,
        .Roughness = 0.45,
        .Profile = TerrainNoiseProfile::Ridges,
    };
    Array<float32> elevation(GetTerrainSampleCount(grid), 0.0F);
    for (uint32 z = 0; z < SIDE; ++z)
    {
        for (uint32 x = 0; x < SIDE; ++x)
        {
            elevation[GetIndex(grid, x, z)] =
                static_cast<float32>(ComputeTerrainElevation(shape,
                                                             key,
                                                             static_cast<float64>(x) * SPACING_METRES_WIDE,
                                                             static_cast<float64>(z) * SPACING_METRES_WIDE));
        }
    }
    const Drainage drainage(grid, elevation);
    TerrainRiverParameters parameters;
    Network network(2 * GetTerrainSampleCount(grid), GetTerrainSampleCount(grid));
    REQUIRE(network.Extract(grid, drainage.Direction, drainage.Accumulation, parameters) ==
            TerrainRiverStatus::Success);
    REQUIRE(network.Summary.SegmentCount > 1);
    CHECK(network.Summary.MaximumOrder >= 2);
    for (usize index = 0; index < network.Summary.SegmentCount; ++index)
    {
        const TerrainRiverSegment& segment = network.Segments[index];
        REQUIRE(segment.PointCount >= 1);
        for (uint32 point = 0; point + 1 < segment.PointCount; ++point)
        {
            const uint32 sample = network.Points[segment.FirstPoint + point];
            const TerrainFlowOffset offset = GetTerrainFlowOffset(drainage.Direction[sample]);
            const uint32 next = GetIndex(grid,
                                         static_cast<uint32>(static_cast<int32>(sample % SIDE) + offset.X),
                                         static_cast<uint32>(static_cast<int32>(sample / SIDE) + offset.Z));
            CHECK(network.Points[segment.FirstPoint + point + 1] == next);
        }
        const uint32 last = network.Points[segment.FirstPoint + segment.PointCount - 1];
        if (segment.DownstreamSegment == TERRAIN_RIVER_NONE)
        {
            CHECK(drainage.Direction[last] == TerrainFlowDirection::Outlet);
        }
        else
        {
            const TerrainRiverSegment& downstream = network.Segments[segment.DownstreamSegment];
            CHECK(network.Points[downstream.FirstPoint] == last);
            CHECK(downstream.Order >= segment.Order);
        }
    }
    Network repeat(2 * GetTerrainSampleCount(grid), GetTerrainSampleCount(grid));
    REQUIRE(repeat.Extract(grid, drainage.Direction, drainage.Accumulation, parameters) == TerrainRiverStatus::Success);
    for (usize point = 0; point < network.Summary.PointCount; ++point)
    {
        CHECK(repeat.Points[point] == network.Points[point]);
    }
}

TEST_CASE("terrain river carving cuts a descending bed without raising terrain", "[content][terrain]")
{
    constexpr uint32 COLUMNS = 21;
    constexpr uint32 ROWS = 41;
    constexpr float32 SPACING_METRES = 2;
    const TerrainFieldGrid grid
    {
        .Columns = COLUMNS,
        .Rows = ROWS,
        .SpacingXMetres = SPACING_METRES,
        .SpacingZMetres = SPACING_METRES,
    };
    const Array<float32> elevation = MakeValley(grid);
    const Drainage drainage(grid, elevation);
    TerrainRiverParameters parameters = MakeSampleCountParameters(8, grid);
    parameters.DepthCoefficientMetres = 0.1F;
    parameters.WidthCoefficientMetres = 0.05F;
    Carve carve(GetTerrainSampleCount(grid));
    REQUIRE(carve.Run(grid, elevation, drainage, parameters) == TerrainRiverStatus::Success);
    CHECK(carve.Summary.ChannelSamples > 0);
    CHECK(carve.Summary.CarvedSamples > 0);
    CHECK(carve.Summary.WetSamples > 0);
    CHECK(carve.Summary.ClampedChannels == 0);
    for (usize sample = 0; sample < elevation.GetSize(); ++sample)
    {
        CHECK(carve.Carved[sample] <= elevation[sample]);
        CHECK(carve.Depth[sample] >= 0);
        CHECK(carve.Surface[sample] == carve.Carved[sample] + carve.Depth[sample]);
    }
    const uint32 centre = COLUMNS / 2;
    // The valley floor's channel starts where the catchment reaches eight samples; downstream of it the bed is
    // lower than the original surface, descends toward the outlet and holds water.
    float32 previousBed = carve.Carved[GetIndex(grid, centre, ROWS / 2)];
    for (int64 z = static_cast<int64>(ROWS / 2) - 1; z >= 0; --z)
    {
        const uint32 sample = GetIndex(grid, centre, static_cast<uint32>(z));
        CHECK(carve.Carved[sample] < elevation[sample]);
        CHECK(carve.Carved[sample] <= previousBed);
        CHECK(carve.Depth[sample] > 0);
        previousBed = carve.Carved[sample];
    }
    // The ridge crests are farther than any profile reaches.
    for (uint32 z = 0; z < ROWS; ++z)
    {
        CHECK(carve.Carved[GetIndex(grid, 0, z)] == elevation[GetIndex(grid, 0, z)]);
        CHECK(carve.Depth[GetIndex(grid, 0, z)] == 0);
    }

    SECTION("carving is repeatable")
    {
        Carve repeat(GetTerrainSampleCount(grid));
        REQUIRE(repeat.Run(grid, elevation, drainage, parameters) == TerrainRiverStatus::Success);
        for (usize sample = 0; sample < elevation.GetSize(); ++sample)
        {
            CHECK(repeat.Carved[sample] == carve.Carved[sample]);
            CHECK(repeat.Depth[sample] == carve.Depth[sample]);
        }
    }

    SECTION("a zero depth holds no water, and zero depth and width leave the terrain uncut")
    {
        TerrainRiverParameters shallow = parameters;
        shallow.DepthCoefficientMetres = 0;
        Carve floor(GetTerrainSampleCount(grid));
        REQUIRE(floor.Run(grid, elevation, drainage, shallow) == TerrainRiverStatus::Success);
        CHECK(floor.Summary.WetSamples == 0);
        shallow.WidthCoefficientMetres = 0;
        Carve flat(GetTerrainSampleCount(grid));
        REQUIRE(flat.Run(grid, elevation, drainage, shallow) == TerrainRiverStatus::Success);
        CHECK(flat.Summary.CarvedSamples == 0);
        CHECK(flat.Summary.WetSamples == 0);
    }

    SECTION("an oversized profile is clamped to the stencil")
    {
        TerrainRiverParameters wide = parameters;
        wide.WidthCoefficientMetres = 1;
        wide.WidthExponent = 1;
        Carve clamped(GetTerrainSampleCount(grid));
        REQUIRE(clamped.Run(grid, elevation, drainage, wide) == TerrainRiverStatus::Success);
        CHECK(clamped.Summary.ClampedChannels > 0);
    }

    SECTION("failures preserve the outputs")
    {
        Carve untouched(GetTerrainSampleCount(grid));
        TerrainRiverParameters invalid = parameters;
        invalid.WaterFraction = 2;
        CHECK(untouched.Run(grid, elevation, drainage, invalid) == TerrainRiverStatus::InvalidArgument);
        Array<float32> broken = elevation;
        broken[3] = MakeNonFinite();
        CHECK(untouched.Run(grid, broken, drainage, parameters) == TerrainRiverStatus::NonFiniteInput);
        const TerrainFieldGrid other
        {
            .Columns = COLUMNS,
            .Rows = ROWS - 1,
            .SpacingXMetres = SPACING_METRES,
            .SpacingZMetres = SPACING_METRES,
        };
        CHECK(untouched.Run(other, elevation, drainage, parameters) == TerrainRiverStatus::InvalidStorage);
        for (usize sample = 0; sample < elevation.GetSize(); ++sample)
        {
            CHECK(untouched.Carved[sample] == -1.0F);
            CHECK(untouched.Depth[sample] == -1.0F);
        }
        CHECK(untouched.Summary.ChannelSamples == 0);
    }
}

TEST_CASE("terrain river carving measures its profile in metres on rectangular cells", "[content][terrain]")
{
    constexpr uint32 COLUMNS = 41;
    constexpr uint32 ROWS = 21;
    constexpr float32 SPACING_X_METRES = 1;
    constexpr float32 SPACING_Z_METRES = 4;
    const TerrainFieldGrid grid
    {
        .Columns = COLUMNS,
        .Rows = ROWS,
        .SpacingXMetres = SPACING_X_METRES,
        .SpacingZMetres = SPACING_Z_METRES,
    };
    const Array<float32> elevation = MakeValley(grid);
    const Drainage drainage(grid, elevation);
    TerrainRiverParameters parameters = MakeSampleCountParameters(4, grid);
    parameters.DepthCoefficientMetres = 0.1F;
    parameters.WidthCoefficientMetres = 0.2F;
    Carve carve(GetTerrainSampleCount(grid));
    REQUIRE(carve.Run(grid, elevation, drainage, parameters) == TerrainRiverStatus::Success);
    CHECK(carve.Summary.CarvedSamples > 0);
    for (usize sample = 0; sample < elevation.GetSize(); ++sample)
    {
        CHECK(carve.Carved[sample] <= elevation[sample]);
        CHECK(carve.Depth[sample] >= 0);
    }
    // Across the valley, one-metre columns let the profile cut more than one sample on each side of the floor.
    const uint32 centre = COLUMNS / 2;
    const uint32 row = 1;
    CHECK(carve.Carved[GetIndex(grid, centre + 1, row)] < elevation[GetIndex(grid, centre + 1, row)]);
    CHECK(carve.Carved[GetIndex(grid, 0, row)] == elevation[GetIndex(grid, 0, row)]);
}
