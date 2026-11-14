#include <ludus/content/terrain/terrain_tile.h>

#include "internal/terrain_tile_heights.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::terrain
{
namespace
{
namespace math = foundation::math;
using foundation::Array;
using foundation::BorrowedView;
using foundation::core::uint16;

constexpr int64 MAX_UNSIGNED16_CODE = 0xffff;
constexpr int64 MAX_SIGNED32_CODE = 0x7fffffff;
constexpr int64 MIN_SIGNED32_CODE = -MAX_SIGNED32_CODE - 1;

bool IsTileSizeValid(uint32 cellsPerSide) noexcept
{
    return cellsPerSide > 0 && cellsPerSide <= MAX_TERRAIN_TILE_AXIS_CELLS;
}

uint32 LocateTileAxis(int64 cell, int64 cellsPerSide, int64& outTile) noexcept
{
    const int64 remainder = cell % cellsPerSide;
    outTile = cell / cellsPerSide;
    if (remainder < 0)
    {
        --outTile;
        return static_cast<uint32>(remainder + cellsPerSide);
    }
    return static_cast<uint32>(remainder);
}

bool TryGetTileAxisOrigin(int64 tile, int64 cellsPerSide, int64& outOrigin) noexcept
{
    int64 end = 0;
    return foundation::TryMultiply(tile, cellsPerSide, outOrigin) == true &&
           foundation::TryAdd(outOrigin, cellsPerSide, end) == true;
}

usize GetHeightCodeWidth(TerrainHeightEncoding encoding) noexcept
{
    return encoding == TerrainHeightEncoding::Unsigned16 ? sizeof(uint16) : sizeof(int32);
}

usize GetPayloadSize(const TerrainTileDescription& description) noexcept
{
    const usize samplesPerSide = static_cast<usize>(description.CellsPerSide) + 1;
    return samplesPerSide * samplesPerSide * GetHeightCodeWidth(description.Codec.Encoding);
}

TerrainTileStatus ValidateTileDescription(const TerrainTileDescription& description) noexcept
{
    const TerrainHeightCodec& codec = description.Codec;
    if ((codec.Encoding != TerrainHeightEncoding::Unsigned16 && codec.Encoding != TerrainHeightEncoding::Signed32) ||
        math::IsFinite(codec.OriginMetres) == false || math::IsFinite(codec.QuantumMetres) == false ||
        codec.QuantumMetres <= 0 || math::IsFinite(description.SpacingMetres) == false ||
        description.SpacingMetres <= 0)
    {
        return TerrainTileStatus::InvalidArgument;
    }
    TerrainSampleAddress origin;
    return TryGetTerrainTileOrigin(description.Address, description.CellsPerSide, origin);
}

float64 DecodeHeight(const TerrainHeightCodec& codec, int64 code) noexcept
{
    return static_cast<float64>(codec.OriginMetres) +
           static_cast<float64>(codec.QuantumMetres) * static_cast<float64>(code);
}

bool TryEncodeHeight(const TerrainHeightCodec& codec, float64 heightMetres, int64& outCode) noexcept
{
    const bool isUnsigned = codec.Encoding == TerrainHeightEncoding::Unsigned16;
    const int64 minimumCode = isUnsigned == true ? 0 : MIN_SIGNED32_CODE;
    const int64 maximumCode = isUnsigned == true ? MAX_UNSIGNED16_CODE : MAX_SIGNED32_CODE;
    if (math::IsFinite(heightMetres) == false || heightMetres < DecodeHeight(codec, minimumCode) ||
        heightMetres > DecodeHeight(codec, maximumCode))
    {
        return false;
    }

    const float64 scaled =
        (heightMetres - static_cast<float64>(codec.OriginMetres)) / static_cast<float64>(codec.QuantumMetres);
    const float64 lower = math::Floor(scaled);
    const float64 rounded = scaled - lower < 0.5 ? lower : lower + 1;
    if (rounded < static_cast<float64>(minimumCode) || rounded > static_cast<float64>(maximumCode))
    {
        return false;
    }
    outCode = static_cast<int64>(rounded);
    return true;
}

void WriteHeightCode(int64 code, BorrowedView<uint8> outBytes) noexcept
{
    if (outBytes.GetSize() == sizeof(uint16))
    {
        LUDUS_REQUIRE(foundation::TryWriteLittleEndian(static_cast<uint16>(code), foundation::MakeSpan(outBytes)));
    }
    else
    {
        LUDUS_REQUIRE(foundation::TryWriteLittleEndian(static_cast<uint32>(code), foundation::MakeSpan(outBytes)));
    }
}

int64 ReadHeightCode(BorrowedView<const uint8> bytes) noexcept
{
    if (bytes.GetSize() == sizeof(uint16))
    {
        uint16 code = 0;
        LUDUS_REQUIRE(foundation::TryReadLittleEndian(foundation::MakeSpan(bytes), code));
        return code;
    }
    uint32 code = 0;
    LUDUS_REQUIRE(foundation::TryReadLittleEndian(foundation::MakeSpan(bytes), code));
    // C++23 defines this conversion modulo the destination width, including negative two's-complement codes.
    return static_cast<int32>(code);
}

int64 ReadTileHeightCode(const TerrainTile& tile,
                         const TerrainTileDescription& description,
                         uint32 column,
                         uint32 row) noexcept
{
    const usize width = GetHeightCodeWidth(description.Codec.Encoding);
    const usize index = static_cast<usize>(row) * (description.CellsPerSide + 1) + column;
    return ReadHeightCode({tile.GetEncodedHeights().GetDataOrNull() + index * width, width});
}

struct SharedAxis final
{
    uint32 First = 0;
    uint32 Second = 0;
    uint32 Count = 0;
};

SharedAxis GetSharedAxis(const TerrainTileDescription& description, int64 first, int64 second) noexcept
{
    int64 offset = 0;
    if (foundation::TrySubtract(second, first, offset) == false || offset < -1 || offset > 1)
    {
        return {};
    }
    const uint32 cellsPerSide = description.CellsPerSide;
    if (offset == 0)
    {
        return {0, 0, cellsPerSide + 1};
    }
    return {offset > 0 ? cellsPerSide : 0, offset < 0 ? cellsPerSide : 0, 1};
}
} // namespace

TerrainTileStatus
TryLocateTerrainCell(TerrainSampleAddress cell, uint32 cellsPerSide, TerrainCellAddress& outAddress) noexcept
{
    if (IsTileSizeValid(cellsPerSide) == false)
    {
        return TerrainTileStatus::InvalidArgument;
    }
    TerrainCellAddress address;
    address.X = LocateTileAxis(cell.X, cellsPerSide, address.Tile.X);
    address.Z = LocateTileAxis(cell.Z, cellsPerSide, address.Tile.Z);
    outAddress = address;
    return TerrainTileStatus::Success;
}

TerrainTileStatus
TryGetTerrainTileOrigin(TerrainTileAddress tile, uint32 cellsPerSide, TerrainSampleAddress& outOrigin) noexcept
{
    if (IsTileSizeValid(cellsPerSide) == false)
    {
        return TerrainTileStatus::InvalidArgument;
    }
    TerrainSampleAddress origin;
    if (TryGetTileAxisOrigin(tile.X, cellsPerSide, origin.X) == false ||
        TryGetTileAxisOrigin(tile.Z, cellsPerSide, origin.Z) == false)
    {
        return TerrainTileStatus::CoordinateOutOfRange;
    }
    outOrigin = origin;
    return TerrainTileStatus::Success;
}

TerrainTileStatus TryCookTerrainTile(const TerrainTileDescription& description,
                                     const TerrainFieldGrid& sourceGrid,
                                     BorrowedView<const float32> elevationsMetres,
                                     uint32 sourceColumn,
                                     uint32 sourceRow,
                                     TerrainTile& outTile) noexcept
{
    const TerrainTileStatus status = ValidateTileDescription(description);
    if (status != TerrainTileStatus::Success)
    {
        return status;
    }
    if (IsTerrainFieldGridValid(sourceGrid) == false || sourceGrid.SpacingXMetres != description.SpacingMetres ||
        sourceGrid.SpacingZMetres != description.SpacingMetres)
    {
        return TerrainTileStatus::InvalidArgument;
    }
    if (elevationsMetres.IsValid() == false || elevationsMetres.GetSize() != GetTerrainSampleCount(sourceGrid))
    {
        return TerrainTileStatus::InvalidStorage;
    }
    const uint32 samplesPerSide = description.CellsPerSide + 1;
    if (sourceColumn >= sourceGrid.Columns || samplesPerSide > sourceGrid.Columns - sourceColumn ||
        sourceRow >= sourceGrid.Rows || samplesPerSide > sourceGrid.Rows - sourceRow)
    {
        return TerrainTileStatus::InvalidArgument;
    }

    TerrainTile candidate;
    candidate.mDescription = description;
    if (candidate.mEncodedHeights.TryResize(GetPayloadSize(description)) == false)
    {
        return TerrainTileStatus::AllocationFailed;
    }
    const usize width = GetHeightCodeWidth(description.Codec.Encoding);
    usize offset = 0;
    for (uint32 row = 0; row < samplesPerSide; ++row)
    {
        const usize sourceStart = static_cast<usize>(sourceRow + row) * sourceGrid.Columns + sourceColumn;
        for (uint32 column = 0; column < samplesPerSide; ++column)
        {
            int64 code = 0;
            if (TryEncodeHeight(description.Codec,
                                static_cast<float64>(elevationsMetres[sourceStart + column]),
                                code) == false)
            {
                return TerrainTileStatus::HeightOutOfRange;
            }
            WriteHeightCode(code, {candidate.mEncodedHeights.GetData() + offset, width});
            offset += width;
        }
    }
    outTile = foundation::Move(candidate);
    return TerrainTileStatus::Success;
}

TerrainTileStatus TryLoadTerrainTile(const TerrainTileDescription& description,
                                     BorrowedView<const uint8> encodedHeights,
                                     TerrainTile& outTile) noexcept
{
    const TerrainTileStatus status = ValidateTileDescription(description);
    if (status != TerrainTileStatus::Success)
    {
        return status;
    }
    if (encodedHeights.IsValid() == false || encodedHeights.GetSize() != GetPayloadSize(description))
    {
        return TerrainTileStatus::InvalidStorage;
    }
    TerrainTile candidate;
    candidate.mDescription = description;
    if (candidate.mEncodedHeights.TryAddRange(encodedHeights) == false)
    {
        return TerrainTileStatus::AllocationFailed;
    }
    outTile = foundation::Move(candidate);
    return TerrainTileStatus::Success;
}

TerrainTileStatus CheckTerrainTileSeam(const TerrainTile& first, const TerrainTile& second) noexcept
{
    if (first.IsEmpty() == true || second.IsEmpty() == true)
    {
        return TerrainTileStatus::InvalidStorage;
    }
    const TerrainTileDescription firstDescription = first.GetDescription();
    const TerrainTileDescription secondDescription = second.GetDescription();
    if (firstDescription.Revision != secondDescription.Revision ||
        firstDescription.CellsPerSide != secondDescription.CellsPerSide ||
        firstDescription.SpacingMetres != secondDescription.SpacingMetres ||
        firstDescription.Codec.Encoding != secondDescription.Codec.Encoding ||
        firstDescription.Codec.OriginMetres != secondDescription.Codec.OriginMetres ||
        firstDescription.Codec.QuantumMetres != secondDescription.Codec.QuantumMetres)
    {
        return TerrainTileStatus::InvalidArgument;
    }

    const SharedAxis x = GetSharedAxis(firstDescription, firstDescription.Address.X, secondDescription.Address.X);
    const SharedAxis z = GetSharedAxis(firstDescription, firstDescription.Address.Z, secondDescription.Address.Z);
    if (x.Count == 0 || z.Count == 0 || (x.Count > 1 && z.Count > 1))
    {
        return TerrainTileStatus::InvalidArgument;
    }
    for (uint32 row = 0; row < z.Count; ++row)
    {
        for (uint32 column = 0; column < x.Count; ++column)
        {
            if (ReadTileHeightCode(first, firstDescription, x.First + column, z.First + row) !=
                ReadTileHeightCode(second, secondDescription, x.Second + column, z.Second + row))
            {
                return TerrainTileStatus::SeamMismatch;
            }
        }
    }
    return TerrainTileStatus::Success;
}

float64 internal::DecodeTerrainTileHeight(const TerrainTile& tile, uint32 column, uint32 row) noexcept
{
    const TerrainTileDescription description = tile.GetDescription();
    return DecodeHeight(description.Codec, ReadTileHeightCode(tile, description, column, row));
}
} // namespace ludus::content::terrain
