#include "internal/foliage_terrain_sampler.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>

// The triangulation, corner order and diagonal ownership match ContentTerrain's
// TrySampleTerrainTile (modules/content/terrain/src/terrain_surface.cpp); the
// exact integer interpolation is the foliage cook's portable replacement for its
// binary64 evaluation. Original implementation.

namespace ludus::content::foliage_cook::internal
{
namespace
{
namespace format = foliage_format;

constexpr int64 LATTICE_UNITS = format::LATTICE_UNITS_PER_TERRAIN_SAMPLE;
/// Bits of the lattice weights, log2(LATTICE_UNITS_PER_TERRAIN_SAMPLE).
constexpr uint32 LATTICE_WEIGHT_BITS = 8;
static_assert((int64{1} << LATTICE_WEIGHT_BITS) == LATTICE_UNITS);
/// Total fraction bits of a weighted height sum, removed when rounding to root height units.
constexpr uint32 HEIGHT_SUM_FRACTION_BITS = LATTICE_WEIGHT_BITS + FOLIAGE_HEIGHT_FRACTION_BITS;
/// Largest absolute lattice coordinate of a terrain extent; keeps every halo expansion inside int64.
constexpr int64 MAXIMUM_LATTICE_COORDINATE = int64{1} << 52;
constexpr usize UNSIGNED16_CODE_BYTES = 2;
constexpr usize SIGNED32_CODE_BYTES = 4;
constexpr usize CORNER_MINIMUM = 0;
constexpr usize CORNER_MAXIMUM = 1;
constexpr usize CORNER_OUTER = 2;

bool AreCodecsEqual(const terrain::TerrainHeightCodec& left, const terrain::TerrainHeightCodec& right) noexcept
{
    return left.Encoding == right.Encoding && left.OriginMetres == right.OriginMetres &&
           left.QuantumMetres == right.QuantumMetres;
}

bool AreDescriptionsCompatible(const terrain::TerrainTileDescription& left,
                               const terrain::TerrainTileDescription& right) noexcept
{
    return left.Revision == right.Revision && left.CellsPerSide == right.CellsPerSide &&
           left.SpacingMetres == right.SpacingMetres && AreCodecsEqual(left.Codec, right.Codec);
}

usize GetCodeBytes(terrain::TerrainHeightEncoding encoding) noexcept
{
    return encoding == terrain::TerrainHeightEncoding::Unsigned16 ? UNSIGNED16_CODE_BYTES : SIGNED32_CODE_BYTES;
}

int64 ReadCode(const terrain::TerrainTile& tile,
               terrain::TerrainHeightEncoding encoding,
               uint32 cellsPerSide,
               uint32 column,
               uint32 row) noexcept
{
    const usize codeBytes = GetCodeBytes(encoding);
    const usize index = (static_cast<usize>(row) * (cellsPerSide + 1) + column) * codeBytes;
    const foundation::BorrowedView<const uint8> payload = tile.GetEncodedHeights();
    const foundation::BorrowedView<const uint8> field{payload.GetDataOrNull() + index, codeBytes};
    if (encoding == terrain::TerrainHeightEncoding::Unsigned16)
    {
        uint16 code = 0;
        [[maybe_unused]] const bool isRead = foundation::TryReadLittleEndian(field, code);
        LUDUS_ASSERT(isRead == true);
        return code;
    }
    uint32 bits = 0;
    [[maybe_unused]] const bool isRead = foundation::TryReadLittleEndian(field, bits);
    LUDUS_ASSERT(isRead == true);
    return static_cast<int32>(bits);
}

int64 ConvertHeight(const TerrainSampler& sampler, int64 code) noexcept
{
    int64 height = 0;
    // Preparation proved every code of every tile converts inside MAXIMUM_SAMPLE_HEIGHT.
    [[maybe_unused]] const bool isConverted =
        TryRoundDyadicSum(sampler.HeightOrigin, sampler.HeightQuantum, code, SAMPLE_HEIGHT_SCALE_BITS, height);
    LUDUS_ASSERT(isConverted == true);
    return height;
}

bool IsSampleHeightAdmitted(DyadicValue origin, DyadicValue quantum, int64 code) noexcept
{
    int64 height = 0;
    return TryRoundDyadicSum(origin, quantum, code, SAMPLE_HEIGHT_SCALE_BITS, height) == true &&
           height > -MAXIMUM_SAMPLE_HEIGHT && height < MAXIMUM_SAMPLE_HEIGHT;
}

FoliageCookStatus Fail(FoliageCookStatus status,
                       FoliageCookIssue issue,
                       usize index,
                       FoliageCookIssue& outIssue,
                       usize& outIndex) noexcept
{
    outIssue = issue;
    outIndex = index;
    return status;
}

FoliageCookStatus ValidateTiles(const FoliageTerrainGrid& grid, FoliageCookIssue& outIssue, usize& outIndex) noexcept
{
    const usize tileCount = static_cast<usize>(grid.TileColumns) * grid.TileRows;
    if (grid.TileColumns == 0 || grid.TileRows == 0 || tileCount > MAX_FOLIAGE_TERRAIN_TILES ||
        grid.Tiles.IsValid() == false || grid.Tiles.GetSize() != tileCount)
    {
        return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::TerrainGrid, 0, outIssue, outIndex);
    }
    const terrain::TerrainTileDescription first = grid.Tiles[0].GetDescription();
    for (usize i = 0; i < tileCount; ++i)
    {
        const terrain::TerrainTile& tile = grid.Tiles[i];
        if (tile.IsEmpty() == true)
        {
            return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::TerrainTile, i, outIssue, outIndex);
        }
        const terrain::TerrainTileDescription description = tile.GetDescription();
        int64 expectedX = 0;
        int64 expectedZ = 0;
        if (foundation::TryAdd(grid.MinimumTile.X, static_cast<int64>(i % grid.TileColumns), expectedX) == false ||
            foundation::TryAdd(grid.MinimumTile.Z, static_cast<int64>(i / grid.TileColumns), expectedZ) == false)
        {
            return Fail(FoliageCookStatus::CoordinateOutOfRange,
                        FoliageCookIssue::TerrainCoordinates,
                        i,
                        outIssue,
                        outIndex);
        }
        if (AreDescriptionsCompatible(first, description) == false || description.Address.X != expectedX ||
            description.Address.Z != expectedZ)
        {
            return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::TerrainTile, i, outIssue, outIndex);
        }
    }
    for (usize i = 0; i < tileCount; ++i)
    {
        const bool hasRight = (i % grid.TileColumns) + 1 < grid.TileColumns;
        const bool hasBelow = (i / grid.TileColumns) + 1 < grid.TileRows;
        if ((hasRight == true &&
             terrain::CheckTerrainTileSeam(grid.Tiles[i], grid.Tiles[i + 1]) != terrain::TerrainTileStatus::Success) ||
            (hasBelow == true && terrain::CheckTerrainTileSeam(grid.Tiles[i], grid.Tiles[i + grid.TileColumns]) !=
                                     terrain::TerrainTileStatus::Success))
        {
            return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::TerrainSeam, i, outIssue, outIndex);
        }
    }
    return FoliageCookStatus::Ok;
}

/// Every code of every tile converts inside the admitted range: conversion is monotone, so the extremes decide.
FoliageCookStatus ValidateHeights(const TerrainSampler& sampler, FoliageCookIssue& outIssue, usize& outIndex) noexcept
{
    for (usize i = 0; i < sampler.Tiles.GetSize(); ++i)
    {
        int64 lowest = 0;
        int64 highest = 0;
        for (uint32 row = 0; row <= sampler.CellsPerSide; ++row)
        {
            for (uint32 column = 0; column <= sampler.CellsPerSide; ++column)
            {
                const int64 code = ReadCode(sampler.Tiles[i], sampler.Encoding, sampler.CellsPerSide, column, row);
                const bool isFirst = row == 0 && column == 0;
                lowest = isFirst == true || code < lowest ? code : lowest;
                highest = isFirst == true || code > highest ? code : highest;
            }
        }
        if (IsSampleHeightAdmitted(sampler.HeightOrigin, sampler.HeightQuantum, lowest) == false ||
            IsSampleHeightAdmitted(sampler.HeightOrigin, sampler.HeightQuantum, highest) == false)
        {
            return Fail(FoliageCookStatus::CoordinateOutOfRange,
                        FoliageCookIssue::TerrainHeight,
                        i,
                        outIssue,
                        outIndex);
        }
    }
    return FoliageCookStatus::Ok;
}

bool TryGetLatticeCoordinate(int64 sample, int64 origin, int64& outLattice) noexcept
{
    int64 offset = 0;
    int64 lattice = 0;
    if (foundation::TrySubtract(sample, origin, offset) == false ||
        foundation::TryMultiply(offset, LATTICE_UNITS, lattice) == false || lattice < -MAXIMUM_LATTICE_COORDINATE ||
        lattice > MAXIMUM_LATTICE_COORDINATE)
    {
        return false;
    }
    outLattice = lattice;
    return true;
}

/// Per-sample and per-cell element counts of a terrain grid.
struct GridCounts final
{
    usize Samples = 0;
    usize Cells = 0;
};

FoliageCookStatus ValidateChannels(const FoliageTerrainInput& terrain,
                                   GridCounts counts,
                                   FoliageCookIssue& outIssue,
                                   usize& outIndex) noexcept
{
    const usize sampleCount = counts.Samples;
    const usize cellCount = counts.Cells;
    if (terrain.Holes.IsValid() == false || (terrain.Holes.IsEmpty() == false && terrain.Holes.GetSize() != cellCount))
    {
        return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::Holes, 0, outIssue, outIndex);
    }
    if (terrain.Wetness.IsValid() == false ||
        (terrain.Wetness.IsEmpty() == false && terrain.Wetness.GetSize() != sampleCount))
    {
        return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::Wetness, 0, outIssue, outIndex);
    }
    if (terrain.Masks.IsValid() == false || terrain.Masks.GetSize() > MAX_FOLIAGE_MASK_CHANNELS)
    {
        return Fail(FoliageCookStatus::LimitExceeded, FoliageCookIssue::MaskChannels, 0, outIssue, outIndex);
    }
    for (usize i = 0; i < terrain.Masks.GetSize(); ++i)
    {
        const FoliageMaskChannel& channel = terrain.Masks[i];
        if (channel.Id == 0 || (i > 0 && channel.Id <= terrain.Masks[i - 1].Id) || channel.Values.IsValid() == false ||
            channel.Values.GetSize() != sampleCount)
        {
            return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::MaskChannels, i, outIssue, outIndex);
        }
    }
    return FoliageCookStatus::Ok;
}
} // namespace

FoliageCookStatus PrepareTerrainSampler(const FoliageCookInput& input,
                                        TerrainSampler& outSampler,
                                        FoliageCookIssue& outIssue,
                                        usize& outIndex) noexcept
{
    const FoliageTerrainInput& terrain = input.Terrain;
    const FoliageCookStatus tileStatus = ValidateTiles(terrain.Grid, outIssue, outIndex);
    if (tileStatus != FoliageCookStatus::Ok)
    {
        return tileStatus;
    }
    const terrain::TerrainTileDescription description = terrain.Grid.Tiles[0].GetDescription();
    TerrainSampler sampler;
    sampler.Tiles = terrain.Grid.Tiles;
    sampler.Holes = terrain.Holes;
    sampler.Wetness = terrain.Wetness;
    sampler.Masks = terrain.Masks;
    sampler.TileColumns = terrain.Grid.TileColumns;
    sampler.CellsPerSide = description.CellsPerSide;
    sampler.Encoding = description.Codec.Encoding;
    sampler.CellColumns = int64{terrain.Grid.TileColumns} * description.CellsPerSide;
    sampler.CellRows = int64{terrain.Grid.TileRows} * description.CellsPerSide;
    sampler.DomainOriginX = input.Domain.OriginX;
    sampler.DomainOriginZ = input.Domain.OriginZ;

    terrain::TerrainSampleAddress minimum;
    if (terrain::TryGetTerrainTileOrigin(terrain.Grid.MinimumTile, description.CellsPerSide, minimum) !=
        terrain::TerrainTileStatus::Success)
    {
        return Fail(FoliageCookStatus::CoordinateOutOfRange,
                    FoliageCookIssue::TerrainCoordinates,
                    0,
                    outIssue,
                    outIndex);
    }
    sampler.MinimumSampleX = minimum.X;
    sampler.MinimumSampleZ = minimum.Z;
    int64 maximumSampleX = 0;
    int64 maximumSampleZ = 0;
    FoliageLatticeRectangle extent;
    if (foundation::TryAdd(minimum.X, sampler.CellColumns, maximumSampleX) == false ||
        foundation::TryAdd(minimum.Z, sampler.CellRows, maximumSampleZ) == false ||
        TryGetLatticeCoordinate(minimum.X, input.Domain.OriginX, extent.MinimumX) == false ||
        TryGetLatticeCoordinate(minimum.Z, input.Domain.OriginZ, extent.MinimumZ) == false ||
        TryGetLatticeCoordinate(maximumSampleX, input.Domain.OriginX, extent.MaximumX) == false ||
        TryGetLatticeCoordinate(maximumSampleZ, input.Domain.OriginZ, extent.MaximumZ) == false)
    {
        return Fail(FoliageCookStatus::CoordinateOutOfRange,
                    FoliageCookIssue::TerrainCoordinates,
                    0,
                    outIssue,
                    outIndex);
    }
    sampler.Extent = extent;

    if (TryDecomposeFloat32(description.SpacingMetres, sampler.SpacingMetres) == false ||
        TryDecomposeFloat32(description.Codec.OriginMetres, sampler.HeightOrigin) == false ||
        TryDecomposeFloat32(description.Codec.QuantumMetres, sampler.HeightQuantum) == false)
    {
        return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::TerrainTile, 0, outIssue, outIndex);
    }
    if (TryRoundDyadicSum(sampler.SpacingMetres, {}, 0, SAMPLE_HEIGHT_SCALE_BITS, sampler.Spacing) == false ||
        sampler.Spacing < 1 || sampler.Spacing >= MAXIMUM_SAMPLE_HEIGHT)
    {
        return Fail(FoliageCookStatus::InvalidInput, FoliageCookIssue::TerrainSpacing, 0, outIssue, outIndex);
    }
    const FoliageCookStatus heightStatus = ValidateHeights(sampler, outIssue, outIndex);
    if (heightStatus != FoliageCookStatus::Ok)
    {
        return heightStatus;
    }

    // At most 4096 tiles of 256 cells keep both counts far below 2^32.
    const usize sampleCount = static_cast<usize>(sampler.CellColumns + 1) * static_cast<usize>(sampler.CellRows + 1);
    const usize cellCount = static_cast<usize>(sampler.CellColumns) * static_cast<usize>(sampler.CellRows);
    const FoliageCookStatus channelStatus =
        ValidateChannels(terrain, { .Samples = sampleCount, .Cells = cellCount }, outIssue, outIndex);
    if (channelStatus != FoliageCookStatus::Ok)
    {
        return channelStatus;
    }
    outSampler = sampler;
    return FoliageCookStatus::Ok;
}

RootSample SampleRoot(const TerrainSampler& sampler, int64 rootX, int64 rootZ) noexcept
{
    RootSample sample;
    // The extent bound keeps these sums far inside int64.
    const int64 localX = sampler.DomainOriginX + FloorDivide(rootX, LATTICE_UNITS) - sampler.MinimumSampleX;
    const int64 localZ = sampler.DomainOriginZ + FloorDivide(rootZ, LATTICE_UNITS) - sampler.MinimumSampleZ;
    if (localX < 0 || localZ < 0 || localX >= sampler.CellColumns || localZ >= sampler.CellRows)
    {
        return sample;
    }
    sample.IsInside = true;
    const usize cellIndex =
        static_cast<usize>(localZ) * static_cast<usize>(sampler.CellColumns) + static_cast<usize>(localX);
    if (sampler.Holes.IsEmpty() == false && sampler.Holes[cellIndex] != 0)
    {
        sample.IsHole = true;
    }
    const int64 fractionX = FloorModulo(rootX, LATTICE_UNITS);
    const int64 fractionZ = FloorModulo(rootZ, LATTICE_UNITS);
    const bool isPositiveXTriangle = fractionX >= fractionZ;
    const int64 larger = isPositiveXTriangle == true ? fractionX : fractionZ;
    const int64 smaller = isPositiveXTriangle == true ? fractionZ : fractionX;

    const uint32 cellsPerSide = sampler.CellsPerSide;
    const usize tileIndex =
        static_cast<usize>(localZ / cellsPerSide) * sampler.TileColumns + static_cast<usize>(localX / cellsPerSide);
    const terrain::TerrainTile& tile = sampler.Tiles[tileIndex];
    const uint32 column = static_cast<uint32>(localX % cellsPerSide);
    const uint32 row = static_cast<uint32>(localZ % cellsPerSide);
    const int64 minimum = ConvertHeight(sampler, ReadCode(tile, sampler.Encoding, cellsPerSide, column, row));
    const int64 maximum = ConvertHeight(sampler, ReadCode(tile, sampler.Encoding, cellsPerSide, column + 1, row + 1));
    const uint32 outerColumn = isPositiveXTriangle == true ? column + 1 : column;
    const uint32 outerRow = isPositiveXTriangle == true ? row : row + 1;
    const int64 outer = ConvertHeight(sampler, ReadCode(tile, sampler.Encoding, cellsPerSide, outerColumn, outerRow));

    const int64 minimumWeight = LATTICE_UNITS - larger;
    const int64 maximumWeight = smaller;
    const int64 outerWeight = larger - smaller;
    // |height| < 2^45 and weights summing to 2^8 keep the sum below 2^53.
    const int64 sum = minimumWeight * minimum + maximumWeight * maximum + outerWeight * outer;
    const int64 half = int64{1} << (HEIGHT_SUM_FRACTION_BITS - 1);
    sample.Height = static_cast<int32>((sum + half) >> HEIGHT_SUM_FRACTION_BITS);
    if (isPositiveXTriangle == true)
    {
        sample.GradientX = outer - minimum;
        sample.GradientZ = maximum - outer;
    }
    else
    {
        sample.GradientX = maximum - outer;
        sample.GradientZ = outer - minimum;
    }

    const usize sampleColumns = static_cast<usize>(sampler.CellColumns) + 1;
    const usize minimumSample = static_cast<usize>(localZ) * sampleColumns + static_cast<usize>(localX);
    sample.Samples[CORNER_MINIMUM] = minimumSample;
    sample.Samples[CORNER_MAXIMUM] = minimumSample + sampleColumns + 1;
    sample.Samples[CORNER_OUTER] = isPositiveXTriangle == true ? minimumSample + 1 : minimumSample + sampleColumns;
    sample.Weights[CORNER_MINIMUM] = static_cast<uint32>(minimumWeight);
    sample.Weights[CORNER_MAXIMUM] = static_cast<uint32>(maximumWeight);
    sample.Weights[CORNER_OUTER] = static_cast<uint32>(outerWeight);
    return sample;
}

uint32 InterpolateField(foundation::BorrowedView<const uint16> values, const RootSample& sample) noexcept
{
    uint32 sum = 0;
    for (usize i = 0; i < ROOT_CORNER_COUNT; ++i)
    {
        sum += sample.Weights[i] * values[sample.Samples[i]];
    }
    const uint32 half = uint32{1} << (LATTICE_WEIGHT_BITS - 1);
    return (sum + half) >> LATTICE_WEIGHT_BITS;
}
} // namespace ludus::content::foliage_cook::internal
