#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/addressed_random.hpp>

#include "foliage_coverage.h"

// Randomized tiling with boundary repair, an offline coverage comparator for the foliage placement gap benchmark
// (issue 1694). It is measured against the whole-region dart throwing control in foliage_coverage.h; FoliageCook
// remains the product under qualification.
//
// The idea of forming a large maximal Poisson-disk pattern from randomized clippings of one small pattern and
// repairing conflicts and gaps at tile borders is from T. Wang and R. Suda, "Fast Maximal Poisson-Disk Sampling by
// Randomized Tiling", High-Performance Graphics 2017, https://doi.org/10.1145/3105762.3105778. Their KD-tree tiling
// and repair procedure were not consulted in detail; the clipping and repair rules here are this comparator's own.
// Each tile copies a window of the seed, optionally under one of the eight symmetries of the square, chosen by
// Philox from the tile address. Repair is deterministic and independent of traversal order: a clipped point loses
// to a conflicting clipped point of higher priority, and the remaining uncovered roots are filled greedily in
// priority order, so the result is the priority-ordered maximal fill of those roots.
namespace ludus::content::foliage_cook::coverage
{
/// A small maximal pattern on the square [0, Size)^2 that tiles are clipped from.
struct SeedPattern final
{
    int64 Size = 0;                          ///< Edge of the seed square, lattice units.
    foundation::Array<CoveragePoint> Points; ///< Maximal samples in the square.
};

/// @brief Make a seed pattern by hierarchical dart throwing over an unobstructed square.
/// @param size Edge of the square, a multiple of the dart throwing base cell.
/// @return False when the fill fails; see TryThrowHierarchicalDarts.
[[nodiscard]] bool TryMakeSeedPattern(int64 size,
                                      int64 minimumDistance,
                                      foundation::math::RandomKey key,
                                      uint64 maximumDarts,
                                      SeedPattern& outPattern,
                                      DartThrowingStatistics& outStatistics) noexcept;

/// Symmetries of the square a tile may apply to its clipping: the identity only.
inline constexpr uint32 TRANSLATION_ONLY = 1;
/// Symmetries of the square a tile may apply to its clipping: every rotation and reflection.
inline constexpr uint32 DIHEDRAL_SYMMETRIES = 8;

/// Choices of one tiling.
struct TilingParameters final
{
    int64 TileSize = 0;                      ///< Tile edge, lattice units; at most the seed size.
    uint32 SymmetryCount = TRANSLATION_ONLY; ///< TRANSLATION_ONLY or DIHEDRAL_SYMMETRIES.
    foundation::math::RandomKey Key{};       ///< Tile choices, clipped priorities and fill priorities.
    bool IsReversed = false;                 ///< Visit tiles and rows in reverse, to check order independence.
};

/// Work and memory of one tiling.
struct TilingStatistics final
{
    uint64 Tiles = 0;            ///< Tiles overlapping the window.
    uint64 ClippedPoints = 0;    ///< Seed points copied into the window.
    uint64 IneligiblePoints = 0; ///< Copied points dropped by the keep-out.
    uint64 RemovedPoints = 0;    ///< Copied points that lost a conflict across a tile border.
    uint64 GapRoots = 0;         ///< Eligible roots no kept point covered before the fill.
    uint64 FilledPoints = 0;     ///< Points the fill added.
    usize PeakBytes = 0;         ///< Largest bytes held by point, grid and gap root storage together.
};

/// @brief Tile a window with clippings of a seed pattern and repair the tile borders.
///
/// Every quantity depends only on tile addresses, seed indices and roots, so a window that contains another
/// produces the same tiles and priorities there; whether it also produces the same points is what the isolated
/// page check measures.
/// @param region Roots the tiling may place; the window is clipped to its extent.
/// @param window Rectangle in which points are made and repaired; points outside it do not exist for this call.
/// @param seed Pattern the tiles are clipped from; its size must be at least the tile size.
/// @param minimumDistance Smallest allowed distance between points, lattice units, at least 2.
/// @param parameters Tile size, symmetries, key and traversal order.
/// @param outPoints Receives the points sorted by Z then X.
/// @param outStatistics Receives the work and memory.
/// @return False for invalid arguments, coordinates beyond int32 or allocation failure.
[[nodiscard]] bool TryTileAndRepair(const EligibleRegion& region,
                                    const FoliageLatticeRectangle& window,
                                    const SeedPattern& seed,
                                    int64 minimumDistance,
                                    const TilingParameters& parameters,
                                    foundation::Array<CoveragePoint>& outPoints,
                                    TilingStatistics& outStatistics) noexcept;

/// Fewest neighbours a point needs for its neighbourhood to count as a configuration.
inline constexpr usize MINIMUM_CONFIGURATION_NEIGHBOURS = 3;

/// Repetition of local configurations, a measure of periodicity at the seed-pattern scale.
struct RepetitionMeasurement final
{
    uint64 Points = 0;            ///< Points measured.
    uint64 Configurations = 0;    ///< Points with at least MINIMUM_CONFIGURATION_NEIGHBOURS neighbours in reach.
    uint64 TranslatedRepeats = 0; ///< Configurations whose exact displacement set occurs at another point.
    uint64 SymmetricRepeats = 0;  ///< Configurations that occur at another point under a symmetry of the square.
};

/// @brief Count points whose whole neighbourhood is an exact copy of another point's.
///
/// The configuration of a point is the set of integer displacements to every other point closer than the reach.
/// An independent maximal fill on the lattice almost never repeats a configuration of three or more displacements,
/// so repeats reveal copied seed regions.
/// @return False when a point lies outside the extent or allocation fails.
[[nodiscard]] bool TryMeasureRepetition(foundation::BorrowedView<const CoveragePoint> points,
                                        const FoliageLatticeRectangle& extent,
                                        int64 reach,
                                        RepetitionMeasurement& outMeasurement) noexcept;
} // namespace ludus::content::foliage_cook::coverage
