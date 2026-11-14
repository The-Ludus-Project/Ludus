#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::terrain
{
using foundation::core::float64;
using foundation::core::int64;

/// Layout version of the raw row-major height payload. Store it alongside the tile metadata.
inline constexpr uint32 TERRAIN_TILE_HEIGHT_LAYOUT_VERSION = 1;

/// Largest supported tile edge in cells. Each tile stores one additional sample on each axis.
inline constexpr uint32 MAX_TERRAIN_TILE_AXIS_CELLS = 256;

/// Outcome of a tiled terrain operation. Every non-success status preserves the caller's outputs.
enum class TerrainTileStatus : uint8
{
    /// The requested result was written.
    Success,
    /// Metadata, query parameters or the relationship between inputs violates the operation's contract.
    InvalidArgument,
    /// An input view or its extent is invalid, or an input tile is empty.
    InvalidStorage,
    /// Coordinates or metric arithmetic exceed the supported range.
    CoordinateOutOfRange,
    /// A source height is nonfinite or falls outside the revision codec's range.
    HeightOutOfRange,
    /// The tiles use different codes along the shared edge or at the shared corner.
    SeamMismatch,
    /// A caller-owned output view is too small.
    CapacityExceeded,
    /// Storage for a new tile could not be allocated.
    AllocationFailed,
    /// A point query is outside this tile.
    OutsideDomain,
    /// The finite ray segment has no intersection with this tile.
    Miss,
    /// Ray traversal stopped before determining the nearest hit or a miss.
    BudgetExceeded,
};

/// Signed horizontal coordinates of a lattice sample or cell's minimum corner.
struct TerrainSampleAddress final
{
    /// Global sample column along +X.
    int64 X = 0;
    /// Global sample row along +Z.
    int64 Z = 0;
};

/// Signed horizontal tile coordinates.
struct TerrainTileAddress final
{
    /// Tile column along +X.
    int64 X = 0;
    /// Tile row along +Z.
    int64 Z = 0;
};

/// Tile and nonnegative local coordinates owning a global cell in the half-open tile partition.
struct TerrainCellAddress final
{
    /// Tile that owns the cell.
    TerrainTileAddress Tile{};
    /// Local column in [0, CellsPerSide).
    uint32 X = 0;
    /// Local row in [0, CellsPerSide).
    uint32 Z = 0;
};

/// Resolve a global cell using floor division, including at negative coordinates and signed limits.
/// @param cell Global cell minimum corner.
/// @param cellsPerSide Edge length in [1, MAX_TERRAIN_TILE_AXIS_CELLS].
/// @param outAddress Owner and local offsets; written only on Success.
[[nodiscard]] TerrainTileStatus
TryLocateTerrainCell(TerrainSampleAddress cell, uint32 cellsPerSide, TerrainCellAddress& outAddress) noexcept;

/// Return a tile's global minimum sample. Both minimum and maximum endpoints must fit int64 on each axis.
/// @param tile Tile address.
/// @param cellsPerSide Edge length in [1, MAX_TERRAIN_TILE_AXIS_CELLS].
/// @param outOrigin Global minimum sample; written only on Success.
[[nodiscard]] TerrainTileStatus
TryGetTerrainTileOrigin(TerrainTileAddress tile, uint32 cellsPerSide, TerrainSampleAddress& outOrigin) noexcept;

/// Integer representation of each sample in the version-one raw little-endian height payload.
enum class TerrainHeightEncoding : uint8
{
    /// Unsigned 16-bit height code.
    Unsigned16,
    /// Signed two's-complement 32-bit height code.
    Signed32,
};

/// Common height decode parameters for every tile of one terrain revision.
struct TerrainHeightCodec final
{
    /// Integer representation used by the payload.
    TerrainHeightEncoding Encoding = TerrainHeightEncoding::Unsigned16;
    /// Code zero elevation in metres, finite.
    float32 OriginMetres = 0;
    /// Elevation increment per code in metres, finite and positive.
    float32 QuantumMetres = 1;
};

/// Metadata supplied by the caller alongside each tile payload.
struct TerrainTileDescription final
{
    /// Identifies the revision within the caller's terrain. Compare only tiles from the same terrain.
    uint64 Revision = 0;
    /// Signed tile column and row.
    TerrainTileAddress Address{};
    /// Square tile edge in cells, from 1 through MAX_TERRAIN_TILE_AXIS_CELLS.
    uint32 CellsPerSide = 0;
    /// Common positive, finite horizontal sample spacing in metres.
    float32 SpacingMetres = 1;
    /// Shared height encoding for this revision.
    TerrainHeightCodec Codec{};
};

/// Move-only owner of one immutable, all-solid tile. A default or moved-from tile is empty.
/// Views into a tile require that its owner stay alive and remain unmoved and unreplaced.
class TerrainTile final
{
public:
    /// Create an empty tile without allocation.
    TerrainTile() noexcept = default;
    /// Transfer payload and metadata, leaving the source empty.
    TerrainTile(TerrainTile&& other) noexcept = default;
    /// Replace the tile by transferring payload and metadata, leaving the source empty for distinct objects.
    TerrainTile& operator=(TerrainTile&& other) noexcept = default;

    /// Whether the tile has no admitted height payload.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mEncodedHeights.IsEmpty();
    }

    /// Copy the metadata of a nonempty tile. An empty tile has no meaningful metadata.
    [[nodiscard]] LUDUS_INLINE TerrainTileDescription GetDescription() const noexcept
    {
        return mDescription;
    }

    /// Borrow the exact version-one raw height payload for as long as this tile remains alive and unchanged.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const uint8> GetEncodedHeights() const noexcept
    {
        return {mEncodedHeights.GetData(), mEncodedHeights.GetSize()};
    }

private:
    friend TerrainTileStatus TryCookTerrainTile(const TerrainTileDescription& description,
                                                const TerrainFieldGrid& sourceGrid,
                                                foundation::BorrowedView<const float32> elevationsMetres,
                                                uint32 sourceColumn,
                                                uint32 sourceRow,
                                                TerrainTile& outTile) noexcept;
    friend TerrainTileStatus TryLoadTerrainTile(const TerrainTileDescription& description,
                                                foundation::BorrowedView<const uint8> encodedHeights,
                                                TerrainTile& outTile) noexcept;

    TerrainTileDescription mDescription;
    foundation::Array<uint8> mEncodedHeights;
};

/// Quantize a square window of a bounded elevation field, including the shared positive endpoints.
/// Samples are row-major from +X to +Z. Nearest code wins, with half-way values rounded toward +infinity.
/// Inputs must lie within the codec's closed metric range. No source storage is retained.
/// @param description Destination tile metadata and shared codec.
/// @param sourceGrid Dimensions and spacing of the complete source field. Both spacings must match the tile.
/// @param elevationsMetres One elevation in metres per source sample; the selected window must be finite.
/// @param sourceColumn Column of the tile's first sample in the field.
/// @param sourceRow Row of the tile's first sample in the field.
/// @param outTile Replaced only on Success.
[[nodiscard]] TerrainTileStatus TryCookTerrainTile(const TerrainTileDescription& description,
                                                   const TerrainFieldGrid& sourceGrid,
                                                   foundation::BorrowedView<const float32> elevationsMetres,
                                                   uint32 sourceColumn,
                                                   uint32 sourceRow,
                                                   TerrainTile& outTile) noexcept;

/// Copy one exact version-one raw little-endian height payload into an immutable tile.
/// Metadata comes from the caller. The payload is exactly (CellsPerSide + 1)^2 codes, in row-major order.
/// @param description Validated independently of the payload bytes.
/// @param encodedHeights Borrowed only for the call; truncation and trailing bytes both fail.
/// @param outTile Replaced only on Success. The source may be borrowed from the previous outTile.
[[nodiscard]] TerrainTileStatus TryLoadTerrainTile(const TerrainTileDescription& description,
                                                   foundation::BorrowedView<const uint8> encodedHeights,
                                                   TerrainTile& outTile) noexcept;

/// Compare every duplicated sample at the edge or corner shared by two neighbouring tiles.
/// Both tiles must have the same revision, layout, spacing and codec. This check allocates no storage.
[[nodiscard]] TerrainTileStatus CheckTerrainTileSeam(const TerrainTile& first, const TerrainTile& second) noexcept;
} // namespace ludus::content::terrain
