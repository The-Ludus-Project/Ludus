#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/content/foliage_cook/foliage_cook.h>
#include <ludus/content/terrain/terrain_tile.h>

#include "foliage_fixed_point.h"

// Fixed-point sampling of the canonical terrain triangles. It reads the same
// quantized codes and triangulation as ContentTerrain's TrySampleTerrainTile but
// converts them exactly, so the cook never depends on binary64 rounding.
namespace ludus::content::foliage_cook::internal
{
/// Fraction bits of a sample height: log2(ROOT_HEIGHT_UNITS_PER_METRE) plus FOLIAGE_HEIGHT_FRACTION_BITS.
inline constexpr int32 SAMPLE_HEIGHT_SCALE_BITS = 26;
/// Exclusive bound of |sample height| and |spacing|: FOLIAGE_MAXIMUM_ABSOLUTE_HEIGHT in sample height units.
inline constexpr int64 MAXIMUM_SAMPLE_HEIGHT = FOLIAGE_MAXIMUM_ABSOLUTE_HEIGHT << FOLIAGE_HEIGHT_FRACTION_BITS;
/// Corners interpolated at a root: the cell's minimum corner, its maximum corner and the triangle's outer corner.
inline constexpr usize ROOT_CORNER_COUNT = 3;

/// Prepared, validated view of a terrain input. Copyable; borrows the input's views.
struct TerrainSampler final
{
    foundation::BorrowedView<const terrain::TerrainTile> Tiles;
    foundation::BorrowedView<const uint8> Holes;
    foundation::BorrowedView<const uint16> Wetness;
    foundation::BorrowedView<const FoliageMaskChannel> Masks;
    uint32 TileColumns = 0;
    uint32 CellsPerSide = 0;
    int64 MinimumSampleX = 0;
    int64 MinimumSampleZ = 0;
    int64 CellColumns = 0;
    int64 CellRows = 0;
    terrain::TerrainHeightEncoding Encoding = terrain::TerrainHeightEncoding::Unsigned16;
    DyadicValue HeightOrigin;
    DyadicValue HeightQuantum;
    DyadicValue SpacingMetres;
    int64 Spacing = 0; ///< Sample spacing in sample height units.
    int64 DomainOriginX = 0;
    int64 DomainOriginZ = 0;
    FoliageLatticeRectangle Extent; ///< Roots inside the terrain cells, relative to the domain origin.
};

/// Canonical surface quantities at one root.
struct RootSample final
{
    bool IsInside = false; ///< Whether the root's terrain cell is in the grid.
    bool IsHole = false;   ///< Whether that cell is a hole.
    int32 Height = 0;      ///< Root height in root height units.
    int64 GradientX = 0;   ///< Height change per sample along X, in sample height units.
    int64 GradientZ = 0;   ///< Height change per sample along Z, in sample height units.
    foundation::StaticArray<usize, ROOT_CORNER_COUNT> Samples{};  ///< Grid sample index of each corner.
    foundation::StaticArray<uint32, ROOT_CORNER_COUNT> Weights{}; ///< Corner weights, in lattice units, summing to 256.
};

/// @brief Validate the domain and terrain input and prepare a sampler.
/// @return Ok, or the status of the first violated rule with outIssue and outIndex naming it.
[[nodiscard]] FoliageCookStatus PrepareTerrainSampler(const FoliageCookInput& input,
                                                      TerrainSampler& outSampler,
                                                      FoliageCookIssue& outIssue,
                                                      usize& outIndex) noexcept;

/// @brief Sample the canonical triangle under a root, relative to the domain origin.
///
/// The root's cell is floor(root / 256) samples from the origin. In the cell, the triangle on the
/// X >= Z side of the minimum-to-maximum diagonal owns the diagonal. Heights interpolate the exact
/// sample heights with lattice weights and round half-way values toward +infinity.
/// @pre The root lies within the sampler's extent expanded by any amount that keeps it in int64 range.
[[nodiscard]] RootSample SampleRoot(const TerrainSampler& sampler, int64 rootX, int64 rootZ) noexcept;

/// @brief Interpolate a per-sample channel at a sampled root, half-way values toward +infinity.
/// @pre sample.IsInside, and the channel holds one value per grid sample.
[[nodiscard]] uint32 InterpolateField(foundation::BorrowedView<const uint16> values, const RootSample& sample) noexcept;
} // namespace ludus::content::foliage_cook::internal
