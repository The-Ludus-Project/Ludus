// Placement gaps of the foliage cook against a maximal Poisson-disk control (issue 1694, slice 4).
//
// One flat, uniform, single-species layer with equal exclusion radii covers the golden fixture's terrain and its
// negative page seams. Hole cells form a keep-out with a narrow valid strip between two bands, so an exclusion
// can be told apart from a gap. Hierarchical dart throwing (foliage_coverage.h) fills the same eligible roots
// maximally with the same minimum distance and layer seed. An independent probe then counts, for each zone, the
// roots where one more candidate could still be admitted.

#include "foliage_cook_golden.h"
#include "foliage_coverage.h"

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
/// Probe stride of the seam zone in the quick test.
constexpr int64 QUICK_SEAM_STRIDE = 2;

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

math::RandomKey MakeControlKey()
{
    math::RandomKey key;
    REQUIRE(math::TryMakeRandomKey(LAYER_SEED, CONTROL_DOMAIN, key) == math::MathStatus::Success);
    return key;
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
    const coverage::ProbeSpacing spacing = { .Interior = 1, .Seam = 1, .NarrowStrip = 1 };
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
