// Placement gaps of the foliage cook against maximal Poisson-disk comparators (issue 1694, slices 4 and 5).
//
// One flat, uniform, single-species layer with equal exclusion radii covers the golden fixture's terrain and its
// negative page seams and junctions. Hole cells form a keep-out with a narrow valid strip between two bands, so an
// exclusion can be told apart from a gap. Hierarchical dart throwing (foliage_coverage.h) fills the same eligible
// roots maximally with the same minimum distance and layer seed, and randomized tiling with boundary repair
// (foliage_tiling.h) builds a maximal fill from clippings of one small seed pattern. An independent probe then
// counts, for each zone, the roots where one more candidate could still be admitted.

#include "foliage_cook_golden.h"
#include "foliage_coverage.h"
#include "foliage_tiling.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/time/time.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::content::foliage_cook;
namespace coverage = ludus::content::foliage_cook::coverage;
namespace format = ludus::content::foliage_format;
namespace math = ludus::foundation::math;
namespace pages = ludus::content::foliage_pages;
namespace timing = ludus::foundation::time;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::float64;

/// Exclusion radius of the single species, lattice units at scale one.
constexpr uint32 RADIUS = 64;
/// Two equal radii: the smallest allowed distance between roots.
constexpr int64 MINIMUM_DISTANCE = int64{2} * RADIUS;
/// Lattice units per terrain cell.
constexpr int64 LATTICE_UNITS = format::LATTICE_UNITS_PER_TERRAIN_SAMPLE;
/// Lattice units per metre at the fixture's 0.5 m sample spacing.
constexpr uint64 LATTICE_UNITS_PER_METRE = 2 * LATTICE_UNITS;
/// Terrain cells per axis.
constexpr int64 CELLS_PER_AXIS = golden::SAMPLES_PER_AXIS - 1;
/// Terrain sample column of the narrow valid strip.
constexpr int64 STRIP_COLUMN = -8;
/// Hole cells on each side of the strip.
constexpr int64 KEEP_OUT_BAND = 4;
/// First terrain sample row of the keep-out.
constexpr int64 KEEP_OUT_FIRST_ROW = -12;
/// Last terrain sample row of the keep-out, inclusive.
constexpr int64 KEEP_OUT_LAST_ROW = 12;
/// Seed of the layer and of the control fill.
constexpr uint64 LAYER_SEED = 0x6761700000000001ULL;
/// Random domain of the control fill, distinct from the cook's placement domains.
constexpr uint32 CONTROL_DOMAIN = 0x46474150U;
/// Bound on control darts; a fill of this fixture needs well under a million.
constexpr uint64 MAXIMUM_DARTS = uint64{1} << 24;
/// Probe stride of the interior zone in the quick test.
constexpr int64 QUICK_INTERIOR_STRIDE = 4;
/// Probe stride of the seam and junction zones in the quick test.
constexpr int64 QUICK_SEAM_STRIDE = 2;
/// Random domain of the tiling seed pattern.
constexpr uint32 SEED_DOMAIN = 0x46534544U;
/// Random domain of tile choices and repair priorities.
constexpr uint32 TILING_DOMAIN = 0x4654494CU;
/// Seed pattern edge: sixteen minimum distances.
constexpr int64 SEED_SIZE = 2048;
/// Tile edge: half the seed, so two tiles span a page and every page seam and junction is a tile border.
constexpr int64 TILE_SIZE = 1024;
/// Reach of a repeated configuration: neighbours closer than two minimum distances.
constexpr int64 CONFIGURATION_REACH = 2 * MINIMUM_DISTANCE;

/// The candidate lattice of one gap configuration.
struct LatticeChoice final
{
    uint32 CellSize = 0; ///< Cell edge, lattice units; divides the page size.
    uint16 Slots = 0;    ///< Candidate slots per cell.
};

/// Density at a layer's slot capacity: every slot holds a candidate.
constexpr uint32 GetCapacityDensity(const LatticeChoice& lattice) noexcept
{
    const uint64 cellsPerMetre = LATTICE_UNITS_PER_METRE / lattice.CellSize;
    return static_cast<uint32>(uint64{FOLIAGE_DENSITY_UNITS_PER_PLACEMENT} * lattice.Slots * cellsPerMetre *
                               cellsPerMetre);
}

/// One candidate per conflict-distance square: about pi candidates per conflict disk.
constexpr LatticeChoice SPARSE_LATTICE = { .CellSize = 128, .Slots = 1 };
/// Four candidates per conflict-distance square: about 4 pi candidates per conflict disk.
constexpr LatticeChoice DENSE_LATTICE = { .CellSize = 64, .Slots = 1 };

/// The golden terrain made flat, with one layer, one species and the keep-out.
struct GapFixture final
{
    golden::GoldenCook Cook;
    coverage::EligibleRegion Region;
    coverage::ZoneLayout Zones;

    explicit GapFixture(const LatticeChoice& lattice)
    {
        REQUIRE(golden::TryInitializeGoldenCook(Cook, [](int64, int64) noexcept { return golden::BASE_CODE; }) == true);
        for (int64 row = 0; row < CELLS_PER_AXIS; ++row)
        {
            for (int64 column = 0; column < CELLS_PER_AXIS; ++column)
            {
                const int64 x = golden::MINIMUM_SAMPLE + column;
                const int64 z = golden::MINIMUM_SAMPLE + row;
                const bool isBand =
                    x >= STRIP_COLUMN - KEEP_OUT_BAND && x <= STRIP_COLUMN + KEEP_OUT_BAND && x != STRIP_COLUMN;
                const bool isHole = isBand == true && z >= KEEP_OUT_FIRST_ROW && z <= KEEP_OUT_LAST_ROW;
                Cook.Holes[static_cast<usize>(row * CELLS_PER_AXIS + column)] = isHole == true ? 1 : 0;
            }
        }
        Cook.HasWetness = false;
        Cook.MaskCount = 0;
        Cook.SuppressionCount = 0;
        Cook.LayerCount = 1;

        FoliageCookSpecies& species = Cook.Species[0];
        species = {};
        // Below the fixture's other two species keys, which stay in the table unused.
        species.Key = 0x1001;
        species.PlacementRevision = 1;
        species.ExclusionRadii[0] = RADIUS;
        species.BoundsRadius = RADIUS;
        species.BoundsHeight = RADIUS;
        Cook.TreePalette[0] = { .Species = 0, .Weight = 1 };

        FoliageCookLayer& layer = Cook.Layers[0];
        layer = {};
        layer.Id = 1;
        layer.SlotsPerCell = lattice.Slots;
        layer.CellSize = lattice.CellSize;
        layer.Seed = LAYER_SEED;
        layer.Density = GetCapacityDensity(lattice);
        layer.Palette = {Cook.TreePalette.GetData(), 1};

        const int64 minimumX = (golden::MINIMUM_SAMPLE - golden::ORIGIN_X) * LATTICE_UNITS;
        const int64 minimumZ = (golden::MINIMUM_SAMPLE - golden::ORIGIN_Z) * LATTICE_UNITS;
        Region.Extent =
        {
            .MinimumX = minimumX,
            .MinimumZ = minimumZ,
            .MaximumX = minimumX + CELLS_PER_AXIS * LATTICE_UNITS,
            .MaximumZ = minimumZ + CELLS_PER_AXIS * LATTICE_UNITS,
        };
        Region.CellSize = LATTICE_UNITS;
        Region.CellColumns = CELLS_PER_AXIS;
        Region.CellRows = CELLS_PER_AXIS;
        Region.Holes = {Cook.Holes.GetData(), Cook.Holes.GetSize()};

        Zones.PageSize = golden::PAGE_SIZE;
        Zones.SeamReach = MINIMUM_DISTANCE;
        Zones.NarrowStrip =
        {
            .MinimumX = (STRIP_COLUMN - golden::ORIGIN_X) * LATTICE_UNITS,
            .MinimumZ = (KEEP_OUT_FIRST_ROW - golden::ORIGIN_Z) * LATTICE_UNITS,
            .MaximumX = (STRIP_COLUMN + 1 - golden::ORIGIN_X) * LATTICE_UNITS,
            .MaximumZ = (KEEP_OUT_LAST_ROW + 1 - golden::ORIGIN_Z) * LATTICE_UNITS,
        };
    }
};

Array<FoliagePageAddress> GetTerrainPages(const FoliageCookInput& input)
{
    Array<FoliagePageAddress> addresses;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(TryGetFoliageTerrainPages(input, addresses, diagnostic) == FoliageCookStatus::Ok);
    return addresses;
}

FoliageCookedPages CookPages(const FoliageCookInput& input, BorrowedView<const FoliagePageAddress> addresses)
{
    FoliageCookedPages cooked;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(TryCookFoliagePages(input, addresses, cooked, diagnostic) == FoliageCookStatus::Ok);
    return cooked;
}

Array<uint8> WritePages(const FoliageCookInput& input, const FoliageCookedPages& cooked)
{
    Array<uint8> bytes;
    FoliageCookDiagnostic diagnostic;
    REQUIRE(TryWriteFoliageCookedPageSet(input, cooked, bytes, diagnostic) == FoliageCookStatus::Ok);
    return bytes;
}

bool AreBytesEqual(const Array<uint8>& left, const Array<uint8>& right)
{
    if (left.GetSize() != right.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < left.GetSize(); ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

/// Roots of every cooked placement.
Array<coverage::CoveragePoint> GetRoots(const FoliageCookedPages& cooked, uint32 cellSize)
{
    Array<coverage::CoveragePoint> roots;
    const BorrowedView<const pages::FoliagePlacement> placements = cooked.GetPlacements();
    REQUIRE(roots.TryResize(placements.GetSize()) == true);
    for (usize i = 0; i < placements.GetSize(); ++i)
    {
        const pages::FoliageRoot root = pages::ComputeFoliageRoot(cellSize, placements[i]);
        roots[i] = { .X = root.X, .Z = root.Z };
    }
    return roots;
}

math::RandomKey MakeKey(uint32 domain)
{
    math::RandomKey key;
    REQUIRE(math::TryMakeRandomKey(LAYER_SEED, domain, key) == math::MathStatus::Success);
    return key;
}

math::RandomKey MakeControlKey()
{
    return MakeKey(CONTROL_DOMAIN);
}

float64 GetElapsedMilliseconds(timing::Timestamp start)
{
    timing::Duration elapsed;
    REQUIRE(timing::TryElapsed(start, timing::Now(), elapsed) == timing::TimeStatus::Ok);
    constexpr float64 MILLISECONDS_PER_SECOND = 1000.0;
    return timing::ToSeconds(elapsed) * MILLISECONDS_PER_SECOND;
}

void RequireEveryRootEligible(const coverage::EligibleRegion& region, const Array<coverage::CoveragePoint>& roots)
{
    for (const coverage::CoveragePoint& root : roots)
    {
        REQUIRE(region.IsEligible(root.X, root.Z) == true);
    }
}

/// Every zone keeps at least the minimum distance between its samples.
void RequireMinimumSpacing(const coverage::CoverageMeasurement& measurement)
{
    for (const coverage::ZoneMeasurement& zone : measurement.Zones)
    {
        if (zone.MinimumSpacingSquared >= 0)
        {
            CHECK(zone.MinimumSpacingSquared >= coverage::GetConflictDistanceSquared(MINIMUM_DISTANCE));
        }
        for (usize bin = 0; bin < coverage::FIRST_ADMISSIBLE_BIN; ++bin)
        {
            CHECK(zone.NearestNeighbour[bin] == 0);
        }
    }
}

uint64 GetAdmissibleRoots(const coverage::CoverageMeasurement& measurement)
{
    uint64 admissible = 0;
    for (const coverage::ZoneMeasurement& zone : measurement.Zones)
    {
        admissible += zone.AdmissibleRoots;
    }
    return admissible;
}

const coverage::ZoneMeasurement& GetZone(const coverage::CoverageMeasurement& measurement, coverage::CoverageZone zone)
{
    return measurement.Zones[static_cast<usize>(zone)];
}

/// Fraction of probed roots that are still admissible, in percent.
float64 GetGapPercent(const coverage::ZoneMeasurement& zone)
{
    constexpr float64 PERCENT = 100.0;
    return zone.ProbeRoots == 0
               ? 0.0
               : PERCENT * static_cast<float64>(zone.AdmissibleRoots) / static_cast<float64>(zone.ProbeRoots);
}

/// Report one point set's zones.
void ReportMeasurement(const char* name, const coverage::CoverageMeasurement& measurement, int64 stride)
{
    constexpr ludus::foundation::StaticArray<const char*, coverage::COVERAGE_ZONE_COUNT> ZONE_NAMES = {
        "interior",
        "seam",
        "junction",
        "narrow strip",
    };
    for (usize i = 0; i < coverage::COVERAGE_ZONE_COUNT; ++i)
    {
        const coverage::ZoneMeasurement& zone = measurement.Zones[i];
        const float64 spacing = zone.MinimumSpacingSquared < 0
                                    ? 0.0
                                    : static_cast<float64>(zone.MinimumSpacingSquared) /
                                          static_cast<float64>(coverage::GetConflictDistanceSquared(MINIMUM_DISTANCE));
        WARN(name << " " << ZONE_NAMES[i] << ": " << zone.Points << " points, min NN^2/d^2 " << spacing << ", NN bins ["
                  << zone.NearestNeighbour[0] << " " << zone.NearestNeighbour[1] << " | " << zone.NearestNeighbour[2]
                  << " " << zone.NearestNeighbour[3] << " " << zone.NearestNeighbour[4] << " "
                  << zone.NearestNeighbour[5] << " " << zone.NearestNeighbour[6] << "], probe stride " << stride << ": "
                  << zone.ProbeRoots << " roots, " << zone.AdmissibleRoots << " admissible (" << GetGapPercent(zone)
                  << " %), empty-distance gap bins [" << zone.EmptyDistance[2] << " " << zone.EmptyDistance[3] << " "
                  << zone.EmptyDistance[4] << " " << zone.EmptyDistance[5] << " " << zone.EmptyDistance[6]
                  << "], largest empty distance^2/d^2 "
                  << static_cast<float64>(zone.LargestEmptyDistanceSquared) /
                         static_cast<float64>(coverage::GetConflictDistanceSquared(MINIMUM_DISTANCE)));
    }
    WARN(name << ": " << measurement.ExcludedProbeRoots << " probe roots excluded by the keep-out or terrain edge");
}
} // namespace

TEST_CASE("the gap fixture cooks identical pages for full, reversed and isolated requests", "[foliage][cook][gap]")
{
    const GapFixture fixture(DENSE_LATTICE);
    const FoliageCookInput input = fixture.Cook.MakeInput();
    const Array<FoliagePageAddress> all = GetTerrainPages(input);
    const Array<uint8> full = WritePages(input, CookPages(input, {all.GetData(), all.GetSize()}));

    Array<FoliagePageAddress> reversed;
    REQUIRE(reversed.TryResize(all.GetSize()) == true);
    for (usize i = 0; i < all.GetSize(); ++i)
    {
        reversed[i] = all[all.GetSize() - 1 - i];
    }
    CHECK(AreBytesEqual(WritePages(input, CookPages(input, {reversed.GetData(), reversed.GetSize()})), full) == true);

    FoliageCookedPages isolated;
    for (const FoliagePageAddress& address : all)
    {
        FoliageCookDiagnostic diagnostic;
        REQUIRE(TryMergeFoliageCookedPages(CookPages(input, {&address, 1}), isolated, diagnostic) ==
                FoliageCookStatus::Ok);
    }
    CHECK(AreBytesEqual(WritePages(input, isolated), full) == true);
}

TEST_CASE("the page cook leaves gaps that a maximal dart throwing control fills", "[foliage][cook][gap]")
{
    const GapFixture fixture(DENSE_LATTICE);
    const FoliageCookInput input = fixture.Cook.MakeInput();
    const Array<FoliagePageAddress> all = GetTerrainPages(input);
    const Array<coverage::CoveragePoint> product =
        GetRoots(CookPages(input, {all.GetData(), all.GetSize()}), DENSE_LATTICE.CellSize);
    RequireEveryRootEligible(fixture.Region, product);

    Array<coverage::CoveragePoint> control;
    coverage::DartThrowingStatistics statistics;
    REQUIRE(coverage::TryThrowHierarchicalDarts(fixture.Region,
                                                MINIMUM_DISTANCE,
                                                MakeControlKey(),
                                                MAXIMUM_DARTS,
                                                control,
                                                statistics) == true);
    RequireEveryRootEligible(fixture.Region, control);
    CHECK(statistics.Accepted == control.GetSize());
    CHECK(statistics.SmallestCellSize == 1);

    const coverage::ProbeSpacing spacing =
    {
        .Interior = QUICK_INTERIOR_STRIDE,
        .Seam = QUICK_SEAM_STRIDE,
        .Junction = QUICK_SEAM_STRIDE,
        .NarrowStrip = 1,
    };
    coverage::CoverageMeasurement productMeasurement;
    coverage::CoverageMeasurement controlMeasurement;
    REQUIRE(coverage::TryMeasureCoverage(fixture.Region,
                                         fixture.Zones,
                                         {product.GetData(), product.GetSize()},
                                         MINIMUM_DISTANCE,
                                         spacing,
                                         productMeasurement) == true);
    REQUIRE(coverage::TryMeasureCoverage(fixture.Region,
                                         fixture.Zones,
                                         {control.GetData(), control.GetSize()},
                                         MINIMUM_DISTANCE,
                                         spacing,
                                         controlMeasurement) == true);

    // Both sets keep the minimum distance; the probe finds no admissible root after the maximal control.
    RequireMinimumSpacing(productMeasurement);
    RequireMinimumSpacing(controlMeasurement);
    CHECK(GetAdmissibleRoots(controlMeasurement) == 0);
    // The keep-out is excluded, not a gap: both sets place roots in the narrow strip and the probe skips the holes.
    CHECK(controlMeasurement.ExcludedProbeRoots > 0);
    CHECK(GetZone(productMeasurement, coverage::CoverageZone::NarrowStrip).Points > 0);
    CHECK(GetZone(controlMeasurement, coverage::CoverageZone::NarrowStrip).Points > 0);
    CHECK(GetZone(controlMeasurement, coverage::CoverageZone::NarrowStrip).AdmissibleRoots == 0);
    // Finite-support thinning underfills: it leaves admissible roots in every zone and fewer samples.
    for (const coverage::ZoneMeasurement& zone : productMeasurement.Zones)
    {
        CHECK(zone.AdmissibleRoots > 0);
    }
    CHECK(product.GetSize() < control.GetSize());
}

TEST_CASE("Benchmark: foliage placement gaps against hierarchical dart throwing", "[.benchmark][foliage][gap]")
{
    // Run with the "[.benchmark]" tag in an optimized build; prints counts, spacing and gap histograms per zone,
    // the cook and control times, and the control's work and peak bytes. Every root is probed.
    const coverage::ProbeSpacing spacing = { .Interior = 1, .Seam = 1, .Junction = 1, .NarrowStrip = 1 };
    constexpr usize LATTICE_COUNT = 2;
    constexpr ludus::foundation::StaticArray<const char*, LATTICE_COUNT> LATTICE_NAMES = {
        "sparse lattice (cell 128, 1 slot)",
        "dense lattice (cell 64, 1 slot)",
    };
    constexpr ludus::foundation::StaticArray<LatticeChoice, LATTICE_COUNT> LATTICES = {SPARSE_LATTICE, DENSE_LATTICE};
    for (usize i = 0; i < LATTICE_COUNT; ++i)
    {
        const GapFixture fixture(LATTICES[i]);
        const FoliageCookInput input = fixture.Cook.MakeInput();
        const Array<FoliagePageAddress> all = GetTerrainPages(input);
        const timing::Timestamp cookStart = timing::Now();
        const FoliageCookedPages cooked = CookPages(input, {all.GetData(), all.GetSize()});
        const float64 cookMilliseconds = GetElapsedMilliseconds(cookStart);
        const Array<coverage::CoveragePoint> product = GetRoots(cooked, LATTICES[i].CellSize);
        coverage::CoverageMeasurement measurement;
        REQUIRE(coverage::TryMeasureCoverage(fixture.Region,
                                             fixture.Zones,
                                             {product.GetData(), product.GetSize()},
                                             MINIMUM_DISTANCE,
                                             spacing,
                                             measurement) == true);
        WARN(LATTICE_NAMES[i] << ": cook " << product.GetSize() << " placements over " << all.GetSize() << " pages in "
                              << cookMilliseconds << " ms");
        ReportMeasurement("cook", measurement, spacing.Interior);
    }

    const GapFixture fixture(DENSE_LATTICE);
    Array<coverage::CoveragePoint> control;
    coverage::DartThrowingStatistics statistics;
    const timing::Timestamp controlStart = timing::Now();
    REQUIRE(coverage::TryThrowHierarchicalDarts(fixture.Region,
                                                MINIMUM_DISTANCE,
                                                MakeControlKey(),
                                                MAXIMUM_DARTS,
                                                control,
                                                statistics) == true);
    const float64 controlMilliseconds = GetElapsedMilliseconds(controlStart);
    coverage::CoverageMeasurement measurement;
    REQUIRE(coverage::TryMeasureCoverage(fixture.Region,
                                         fixture.Zones,
                                         {control.GetData(), control.GetSize()},
                                         MINIMUM_DISTANCE,
                                         spacing,
                                         measurement) == true);
    WARN("control: " << control.GetSize() << " samples in " << controlMilliseconds << " ms, " << statistics.Darts
                     << " darts, " << statistics.CoveredCells << " covered cells, base cell " << statistics.BaseCellSize
                     << ", smallest cell " << statistics.SmallestCellSize << ", " << statistics.Levels
                     << " levels, peak " << statistics.PeakActiveCells << " active cells and " << statistics.PeakBytes
                     << " bytes");
    for (usize level = 0; level < statistics.Levels; ++level)
    {
        WARN("control level " << level << ": " << statistics.ActiveCells[level] << " active cells");
    }
    ReportMeasurement("control", measurement, spacing.Interior);
    CHECK(GetAdmissibleRoots(measurement) == 0);
}

namespace
{
/// A tiling configuration of the comparator.
struct TilingChoice final
{
    const char* Name = "";
    int64 SeedSize = 0;
    int64 TileSize = 0;
    uint32 SymmetryCount = coverage::TRANSLATION_ONLY;
};

coverage::SeedPattern MakeSeed(int64 size, coverage::DartThrowingStatistics& outStatistics)
{
    coverage::SeedPattern seed;
    REQUIRE(coverage::TryMakeSeedPattern(size,
                                         MINIMUM_DISTANCE,
                                         MakeKey(SEED_DOMAIN),
                                         MAXIMUM_DARTS,
                                         seed,
                                         outStatistics) == true);
    return seed;
}

coverage::TilingParameters MakeTilingParameters(const TilingChoice& choice, bool isReversed)
{
    return
    {
        .TileSize = choice.TileSize,
        .SymmetryCount = choice.SymmetryCount,
        .Key = MakeKey(TILING_DOMAIN),
        .IsReversed = isReversed,
    };
}

Array<coverage::CoveragePoint> TileWindow(const coverage::EligibleRegion& region,
                                          const FoliageLatticeRectangle& window,
                                          const coverage::SeedPattern& seed,
                                          const coverage::TilingParameters& parameters,
                                          coverage::TilingStatistics& outStatistics)
{
    Array<coverage::CoveragePoint> points;
    REQUIRE(coverage::TryTileAndRepair(region, window, seed, MINIMUM_DISTANCE, parameters, points, outStatistics) ==
            true);
    return points;
}

bool ArePointsEqual(const Array<coverage::CoveragePoint>& left, const Array<coverage::CoveragePoint>& right)
{
    if (left.GetSize() != right.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < left.GetSize(); ++i)
    {
        if (left[i].X != right[i].X || left[i].Z != right[i].Z)
        {
            return false;
        }
    }
    return true;
}

bool IsInside(const FoliageLatticeRectangle& rectangle, const coverage::CoveragePoint& point)
{
    return point.X >= rectangle.MinimumX && point.X < rectangle.MaximumX && point.Z >= rectangle.MinimumZ &&
           point.Z < rectangle.MaximumZ;
}

/// Points of two Z-then-X sorted sets inside a rectangle that the other set lacks.
uint64 CountDifferences(const Array<coverage::CoveragePoint>& left,
                        const Array<coverage::CoveragePoint>& right,
                        const FoliageLatticeRectangle& rectangle)
{
    const auto isBefore = [](const coverage::CoveragePoint& a, const coverage::CoveragePoint& b) {
        return a.Z != b.Z ? a.Z < b.Z : a.X < b.X;
    };
    uint64 differences = 0;
    usize i = 0;
    usize j = 0;
    while (i < left.GetSize() || j < right.GetSize())
    {
        if (i < left.GetSize() && IsInside(rectangle, left[i]) == false)
        {
            ++i;
        }
        else if (j < right.GetSize() && IsInside(rectangle, right[j]) == false)
        {
            ++j;
        }
        else if (i == left.GetSize() || (j < right.GetSize() && isBefore(right[j], left[i]) == true))
        {
            ++differences;
            ++j;
        }
        else if (j == right.GetSize() || isBefore(left[i], right[j]) == true)
        {
            ++differences;
            ++i;
        }
        else
        {
            ++i;
            ++j;
        }
    }
    return differences;
}

/// Pages, clipped to the region, that a full and an isolated tiling disagree on.
struct IsolationResult final
{
    uint64 Pages = 0;
    uint64 MismatchedPages = 0;
    uint64 MismatchedPoints = 0;
};

/// Tile each page alone with a halo and compare it with the full tiling.
IsolationResult MeasureIsolation(const GapFixture& fixture,
                                 const coverage::SeedPattern& seed,
                                 const coverage::TilingParameters& parameters,
                                 const Array<coverage::CoveragePoint>& full,
                                 int64 halo)
{
    const FoliageLatticeRectangle& extent = fixture.Region.Extent;
    const int64 pageSize = golden::PAGE_SIZE;
    IsolationResult result;
    for (int64 pageZ = coverage::FloorDivide(extent.MinimumZ, pageSize); pageZ * pageSize < extent.MaximumZ; ++pageZ)
    {
        for (int64 pageX = coverage::FloorDivide(extent.MinimumX, pageSize); pageX * pageSize < extent.MaximumX;
             ++pageX)
        {
            const FoliageLatticeRectangle page =
            {
                .MinimumX = pageX * pageSize,
                .MinimumZ = pageZ * pageSize,
                .MaximumX = (pageX + 1) * pageSize,
                .MaximumZ = (pageZ + 1) * pageSize,
            };
            const FoliageLatticeRectangle window =
            {
                .MinimumX = page.MinimumX - halo,
                .MinimumZ = page.MinimumZ - halo,
                .MaximumX = page.MaximumX + halo,
                .MaximumZ = page.MaximumZ + halo,
            };
            coverage::TilingStatistics statistics;
            const Array<coverage::CoveragePoint> isolated =
                TileWindow(fixture.Region, window, seed, parameters, statistics);
            const uint64 differences = CountDifferences(full, isolated, page);
            result.Pages += 1;
            if (differences != 0)
            {
                result.MismatchedPages += 1;
                result.MismatchedPoints += differences;
            }
        }
    }
    return result;
}

coverage::RepetitionMeasurement MeasureRepetition(const GapFixture& fixture,
                                                  const Array<coverage::CoveragePoint>& points)
{
    coverage::RepetitionMeasurement measurement;
    REQUIRE(coverage::TryMeasureRepetition({points.GetData(), points.GetSize()},
                                           fixture.Region.Extent,
                                           CONFIGURATION_REACH,
                                           measurement) == true);
    return measurement;
}

void ReportRepetition(const char* name, const coverage::RepetitionMeasurement& measurement)
{
    constexpr float64 PERCENT = 100.0;
    const float64 configurations = static_cast<float64>(measurement.Configurations);
    WARN(name << " repetition: " << measurement.Configurations << " configurations of " << measurement.Points
              << " points, " << measurement.TranslatedRepeats << " translated repeats ("
              << PERCENT * static_cast<float64>(measurement.TranslatedRepeats) / configurations << " %), "
              << measurement.SymmetricRepeats << " repeats under a square symmetry ("
              << PERCENT * static_cast<float64>(measurement.SymmetricRepeats) / configurations << " %)");
}

constexpr TilingChoice QUICK_TILING =
{
    .Name = "tiles 1024 from seed 2048, dihedral",
    .SeedSize = SEED_SIZE,
    .TileSize = TILE_SIZE,
    .SymmetryCount = coverage::DIHEDRAL_SYMMETRIES,
};
} // namespace

TEST_CASE("randomized tiling repairs seams and junctions into a maximal fill", "[foliage][cook][gap][tiling]")
{
    const GapFixture fixture(DENSE_LATTICE);
    coverage::DartThrowingStatistics seedStatistics;
    const coverage::SeedPattern seed = MakeSeed(QUICK_TILING.SeedSize, seedStatistics);
    CHECK(seedStatistics.SmallestCellSize == 1);

    coverage::TilingStatistics statistics;
    const Array<coverage::CoveragePoint> tiled =
        TileWindow(fixture.Region, fixture.Region.Extent, seed, MakeTilingParameters(QUICK_TILING, false), statistics);
    RequireEveryRootEligible(fixture.Region, tiled);
    // Repair did work at the borders: clipped points lost conflicts and gaps were filled.
    CHECK(statistics.RemovedPoints > 0);
    CHECK(statistics.IneligiblePoints > 0);
    CHECK(statistics.FilledPoints > 0);
    CHECK(tiled.GetSize() ==
          statistics.ClippedPoints - statistics.IneligiblePoints - statistics.RemovedPoints + statistics.FilledPoints);

    const coverage::ProbeSpacing spacing =
    {
        .Interior = QUICK_INTERIOR_STRIDE,
        .Seam = QUICK_SEAM_STRIDE,
        .Junction = QUICK_SEAM_STRIDE,
        .NarrowStrip = 1,
    };
    coverage::CoverageMeasurement measurement;
    REQUIRE(coverage::TryMeasureCoverage(fixture.Region,
                                         fixture.Zones,
                                         {tiled.GetData(), tiled.GetSize()},
                                         MINIMUM_DISTANCE,
                                         spacing,
                                         measurement) == true);
    RequireMinimumSpacing(measurement);
    CHECK(GetAdmissibleRoots(measurement) == 0);
    CHECK(GetZone(measurement, coverage::CoverageZone::Seam).Points > 0);
    CHECK(GetZone(measurement, coverage::CoverageZone::Junction).Points > 0);
    CHECK(GetZone(measurement, coverage::CoverageZone::NarrowStrip).Points > 0);

    // Repair is a function of addresses and priorities, not of the order tiles and rows are visited.
    coverage::TilingStatistics reversedStatistics;
    const Array<coverage::CoveragePoint> reversed = TileWindow(fixture.Region,
                                                               fixture.Region.Extent,
                                                               seed,
                                                               MakeTilingParameters(QUICK_TILING, true),
                                                               reversedStatistics);
    CHECK(ArePointsEqual(reversed, tiled) == true);

    // Clippings of one seed repeat its configurations; an independent maximal fill does not.
    Array<coverage::CoveragePoint> control;
    coverage::DartThrowingStatistics controlStatistics;
    REQUIRE(coverage::TryThrowHierarchicalDarts(fixture.Region,
                                                MINIMUM_DISTANCE,
                                                MakeControlKey(),
                                                MAXIMUM_DARTS,
                                                control,
                                                controlStatistics) == true);
    const coverage::RepetitionMeasurement tiledRepetition = MeasureRepetition(fixture, tiled);
    const coverage::RepetitionMeasurement controlRepetition = MeasureRepetition(fixture, control);
    CHECK(tiledRepetition.SymmetricRepeats > 0);
    CHECK(tiledRepetition.TranslatedRepeats <= tiledRepetition.SymmetricRepeats);
    CHECK(controlRepetition.Configurations > 0);
    CHECK(controlRepetition.SymmetricRepeats == 0);
}

TEST_CASE("Benchmark: randomized tiling against hierarchical dart throwing", "[.benchmark][foliage][gap][tiling]")
{
    // Run with the "[.benchmark]" tag in an optimized build; prints the control and each tiling's counts, spacing,
    // gaps, repetition, work, bytes and isolated-page agreement over a halo sweep. Every root is probed.
    const coverage::ProbeSpacing spacing = { .Interior = 1, .Seam = 1, .Junction = 1, .NarrowStrip = 1 };
    const GapFixture fixture(DENSE_LATTICE);

    const FoliageCookInput input = fixture.Cook.MakeInput();
    const Array<FoliagePageAddress> all = GetTerrainPages(input);
    ReportRepetition(
        "cook (cell 64)",
        MeasureRepetition(fixture, GetRoots(CookPages(input, {all.GetData(), all.GetSize()}), DENSE_LATTICE.CellSize)));

    Array<coverage::CoveragePoint> control;
    coverage::DartThrowingStatistics controlStatistics;
    const timing::Timestamp controlStart = timing::Now();
    REQUIRE(coverage::TryThrowHierarchicalDarts(fixture.Region,
                                                MINIMUM_DISTANCE,
                                                MakeControlKey(),
                                                MAXIMUM_DARTS,
                                                control,
                                                controlStatistics) == true);
    const float64 controlMilliseconds = GetElapsedMilliseconds(controlStart);
    WARN("control: " << control.GetSize() << " samples in " << controlMilliseconds << " ms, "
                     << controlStatistics.PeakBytes << " peak bytes");
    ReportRepetition("control", MeasureRepetition(fixture, control));

    constexpr usize TILING_COUNT = 3;
    constexpr ludus::foundation::StaticArray<TilingChoice, TILING_COUNT> TILINGS = {
        TilingChoice
        {
            .Name = "tiles 1024 from seed 2048, translations",
            .SeedSize = SEED_SIZE,
            .TileSize = TILE_SIZE,
            .SymmetryCount = coverage::TRANSLATION_ONLY,
        },
        QUICK_TILING,
        TilingChoice
        {
            .Name = "tiles 512 from seed 1024, dihedral",
            .SeedSize = SEED_SIZE / 2,
            .TileSize = TILE_SIZE / 2,
            .SymmetryCount = coverage::DIHEDRAL_SYMMETRIES,
        },
    };
    constexpr usize HALO_COUNT = 6;
    constexpr ludus::foundation::StaticArray<int64, HALO_COUNT> HALO_DISTANCES = {0, 1, 2, 4, 8, 16};
    for (const TilingChoice& choice : TILINGS)
    {
        coverage::DartThrowingStatistics seedStatistics;
        const timing::Timestamp seedStart = timing::Now();
        const coverage::SeedPattern seed = MakeSeed(choice.SeedSize, seedStatistics);
        const float64 seedMilliseconds = GetElapsedMilliseconds(seedStart);
        const coverage::TilingParameters parameters = MakeTilingParameters(choice, false);
        coverage::TilingStatistics statistics;
        const timing::Timestamp tilingStart = timing::Now();
        const Array<coverage::CoveragePoint> tiled =
            TileWindow(fixture.Region, fixture.Region.Extent, seed, parameters, statistics);
        const float64 tilingMilliseconds = GetElapsedMilliseconds(tilingStart);
        WARN(choice.Name << ": seed " << seed.Points.GetSize() << " points in " << seedMilliseconds << " ms; "
                         << tiled.GetSize() << " points in " << tilingMilliseconds << " ms over " << statistics.Tiles
                         << " tiles: " << statistics.ClippedPoints << " clipped, " << statistics.IneligiblePoints
                         << " in the keep-out, " << statistics.RemovedPoints << " removed, " << statistics.GapRoots
                         << " gap roots, " << statistics.FilledPoints << " filled, peak " << statistics.PeakBytes
                         << " bytes");
        coverage::CoverageMeasurement measurement;
        REQUIRE(coverage::TryMeasureCoverage(fixture.Region,
                                             fixture.Zones,
                                             {tiled.GetData(), tiled.GetSize()},
                                             MINIMUM_DISTANCE,
                                             spacing,
                                             measurement) == true);
        ReportMeasurement("tiling", measurement, spacing.Interior);
        CHECK(GetAdmissibleRoots(measurement) == 0);
        ReportRepetition("tiling", MeasureRepetition(fixture, tiled));
        for (const int64 halo : HALO_DISTANCES)
        {
            const timing::Timestamp isolationStart = timing::Now();
            const IsolationResult isolation =
                MeasureIsolation(fixture, seed, parameters, tiled, halo * MINIMUM_DISTANCE);
            WARN("  isolated pages with a halo of "
                 << halo << " d: " << isolation.MismatchedPages << " of " << isolation.Pages << " pages differ by "
                 << isolation.MismatchedPoints << " points (" << GetElapsedMilliseconds(isolationStart) << " ms)");
        }
    }
}
