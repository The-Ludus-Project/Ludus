#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>

#include <ludus/content/foliage_cook/foliage_cook.h>

// Offline coverage controls for the foliage placement gap benchmark (issue 1694). The product under
// qualification is FoliageCook's finite-support thinning; these helpers only measure it. They compare it with a
// maximal Poisson-disk fill made by hierarchical dart throwing and check both with an independent probe.
//
// Hierarchical dart throwing adapts D. Cline, K. B. White and P. K. Egbert, "Poisson Disk Point Sets by
// Hierarchical Dart Throwing", IEEE Symposium on Interactive Ray Tracing 2007, pp. 129-132,
// https://scholarsarchive.byu.edu/facpub/237/: darts go only into regions that existing samples do not already
// cover. This version keeps active quadtree cells per level, throws into a cell chosen by area, discards a cell
// only when one disk or the keep-out covers all of it, and splits the others. Here the domain is the integer
// lattice of FoliageCook roots, so the quadtree ends at single-root cells whose one dart is decided exactly and the
// fill is maximal with zero termination tolerance. The coverage objective, sampling until no admissible point
// remains, is that of D. Dunbar and G. Humphreys, "A Spatial Data Structure for Fast Poisson-Disk Sample
// Generation", ACM Transactions on Graphics 25(3) (SIGGRAPH 2006), pp. 503-508,
// https://doi.org/10.1145/1141911.1141915.
namespace ludus::content::foliage_cook::coverage
{
/// A root in lattice units relative to the domain origin.
struct CoveragePoint final
{
    int64 X = 0; ///< Lattice X.
    int64 Z = 0; ///< Lattice Z.
};

/// Roots a candidate may occupy: integer lattice points in non-hole terrain cells.
struct EligibleRegion final
{
    FoliageLatticeRectangle Extent{};            ///< Roots inside the terrain cells, half-open.
    int64 CellSize = 0;                          ///< Terrain cell edge in lattice units.
    int64 CellColumns = 0;                       ///< Terrain cells along X.
    int64 CellRows = 0;                          ///< Terrain cells along Z.
    foundation::BorrowedView<const uint8> Holes; ///< One byte per terrain cell, nonzero for a hole; empty for none.

    /// Whether a root lies in a non-hole terrain cell.
    [[nodiscard]] bool IsEligible(int64 x, int64 z) const noexcept;
};

/// Integer squared distance below which two equal-radius roots conflict: (2 * radius)^2.
[[nodiscard]] constexpr int64 GetConflictDistanceSquared(int64 minimumDistance) noexcept
{
    return minimumDistance * minimumDistance;
}

/// Index value that skips no point in a nearest-neighbour query.
inline constexpr usize NO_SKIP_INDEX = ~usize{0};

/// A nearest-neighbour query of a PointGrid.
struct NearestQuery final
{
    CoveragePoint Position{};        ///< Position to search from.
    int64 Reach = 0;                 ///< Half edge of the square searched around the position.
    usize SkipIndex = NO_SKIP_INDEX; ///< Point to ignore, such as the point the query starts from.
};

/// Uniform bucket grid over points for nearest-neighbour queries; makes no spacing assumption.
class PointGrid final
{
public:
    /// @brief Bucket points by cells of the given edge over an extent.
    /// @return False when a point lies outside the extent or allocation fails.
    [[nodiscard]] bool TryBuild(foundation::BorrowedView<const CoveragePoint> points,
                                const FoliageLatticeRectangle& extent,
                                int64 cellSize) noexcept;

    /// @brief Squared distance from the query position to its nearest point within the reach, other than the
    /// skipped one.
    /// @return False when no point lies within the reach box; outDistanceSquared is then unchanged.
    [[nodiscard]] bool TryFindNearest(const NearestQuery& query, int64& outDistanceSquared) const noexcept;

private:
    foundation::BorrowedView<const CoveragePoint> mPoints;
    FoliageLatticeRectangle mExtent{};
    int64 mCellSize = 1;
    int64 mColumns = 0;
    int64 mRows = 0;
    foundation::Array<uint32> mFirst;
    foundation::Array<uint32> mIndices;
};

/// Quadtree levels from the base cell down to a single root.
inline constexpr usize MAX_DART_LEVELS = 16;

/// Work and memory of one hierarchical dart throwing fill.
struct DartThrowingStatistics final
{
    uint64 Darts = 0;                                               ///< Darts thrown.
    uint64 Accepted = 0;                                            ///< Darts that became samples.
    uint64 CoveredCells = 0;                                        ///< Cells discarded as covered by one disk.
    foundation::StaticArray<uint64, MAX_DART_LEVELS> ActiveCells{}; ///< Cells made active per level.
    usize Levels = 0;                                               ///< Levels used, base level first.
    int64 BaseCellSize = 0;                                         ///< Edge of a level-zero cell, lattice units.
    /// Edge of the smallest explored cell. One means single-root cells were decided exactly, so the termination
    /// tolerance is zero: no admissible root is left unexplored.
    int64 SmallestCellSize = 0;
    usize PeakActiveCells = 0; ///< Largest number of simultaneously active cells.
    usize PeakBytes = 0;       ///< Largest bytes held by cell lists, samples and the sample grid together.
};

/// @brief Fill an eligible region with a maximal set of roots at least `minimumDistance` apart.
///
/// Draws come from Philox with the given key: event e of the fill selects a cell by area and a root in it.
/// Every admissible root is either a sample or lies closer than minimumDistance to one at termination.
/// @param region Roots the fill may place; its extent must be aligned to the base cell size.
/// @param minimumDistance Smallest allowed distance between samples, lattice units, at least 2.
/// @param key Random key of the fill.
/// @param maximumDarts Bound on darts; exceeding it fails.
/// @param outPoints Receives the samples in placement order.
/// @param outStatistics Receives the work and memory of the fill.
/// @return False for invalid arguments, an exhausted dart bound or allocation failure.
[[nodiscard]] bool TryThrowHierarchicalDarts(const EligibleRegion& region,
                                             int64 minimumDistance,
                                             foundation::math::RandomKey key,
                                             uint64 maximumDarts,
                                             foundation::Array<CoveragePoint>& outPoints,
                                             DartThrowingStatistics& outStatistics) noexcept;

/// Where a root lies for reporting.
enum class CoverageZone : uint8
{
    Interior,    ///< At least the reach of a seam away from every page boundary.
    Seam,        ///< Within the reach of a page boundary.
    NarrowStrip, ///< Inside the narrow valid strip beside the keep-out.
};

/// Number of reporting zones.
inline constexpr usize COVERAGE_ZONE_COUNT = 3;

/// Geometry that assigns zones.
struct ZoneLayout final
{
    int64 PageSize = 0;                    ///< Page edge in lattice units.
    int64 SeamReach = 0;                   ///< Distance from a page boundary that still counts as the seam.
    FoliageLatticeRectangle NarrowStrip{}; ///< The narrow valid strip, half-open.

    /// Zone of a root; the narrow strip takes precedence over the seam.
    [[nodiscard]] CoverageZone Classify(int64 x, int64 z) const noexcept;
};

/// Histogram bins of a distance in units of the minimum distance: [0, 1/2), [1/2, 1), [1, 5/4), [5/4, 3/2),
/// [3/2, 2), [2, 3) and at least 3 or none within the search reach.
inline constexpr usize DISTANCE_BIN_COUNT = 7;
/// First bin at or beyond the minimum distance.
inline constexpr usize FIRST_ADMISSIBLE_BIN = 2;

/// Distance histogram bins scaled by a minimum distance.
struct DistanceBins final
{
    int64 MinimumDistance = 1; ///< Unit of the bins, lattice units.

    /// Bin of a squared distance.
    [[nodiscard]] usize GetBin(int64 distanceSquared) const noexcept;
};

/// Measurements of one point set in one zone.
struct ZoneMeasurement final
{
    uint64 Points = 0;                ///< Samples in the zone.
    int64 MinimumSpacingSquared = -1; ///< Smallest squared NN distance; -1 for none.
    foundation::StaticArray<uint64, DISTANCE_BIN_COUNT> NearestNeighbour{}; ///< NN distance histogram.
    uint64 ProbeRoots = 0;                                                  ///< Eligible probe roots.
    uint64 AdmissibleRoots = 0;                                             ///< Probe roots no sample excludes.
    foundation::StaticArray<uint64, DISTANCE_BIN_COUNT> EmptyDistance{};    ///< Probe distance-to-sample histogram.
    int64 LargestEmptyDistanceSquared = 0; ///< Largest squared probe distance to a sample within the reach.
};

/// Measurements of one point set across every zone.
struct CoverageMeasurement final
{
    foundation::StaticArray<ZoneMeasurement, COVERAGE_ZONE_COUNT> Zones{}; ///< Indexed by CoverageZone.
    uint64 ExcludedProbeRoots = 0; ///< Probe roots in the keep-out, which are excluded rather than gaps.
};

/// Probe sampling: every `Stride`-th root on each axis, starting at the extent minimum.
struct ProbeSpacing final
{
    int64 Interior = 1;    ///< Stride in the interior zone.
    int64 Seam = 1;        ///< Stride in the seam zone.
    int64 NarrowStrip = 1; ///< Stride in the narrow strip.
};

/// @brief Measure nearest-neighbour spacing and the admissible area left by a point set.
///
/// The probe is independent of the cook and of the fill: it tests each probe root against the eligible region
/// and every sample within reach with exact integer distances.
/// @return False when a point lies outside the region's extent or allocation fails.
[[nodiscard]] bool TryMeasureCoverage(const EligibleRegion& region,
                                      const ZoneLayout& layout,
                                      foundation::BorrowedView<const CoveragePoint> points,
                                      int64 minimumDistance,
                                      const ProbeSpacing& spacing,
                                      CoverageMeasurement& outMeasurement) noexcept;
} // namespace ludus::content::foliage_cook::coverage
