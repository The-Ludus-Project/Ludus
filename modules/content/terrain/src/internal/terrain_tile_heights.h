#pragma once

#include <ludus/content/terrain/terrain_tile.h>

namespace ludus::content::terrain::internal
{
/// Decode a sample of a validated nonempty tile. Column and row include the positive endpoints.
[[nodiscard]] float64 DecodeTerrainTileHeight(const TerrainTile& tile, uint32 column, uint32 row) noexcept;

/// Read the integer code of a sample of a validated nonempty tile. Column and row include the positive endpoints.
[[nodiscard]] int64 ReadTerrainTileHeightCode(const TerrainTile& tile, uint32 column, uint32 row) noexcept;

/// Decode a height code with the canonical binary64 rule shared by every tile: origin plus quantum times code.
[[nodiscard]] float64 DecodeTerrainHeight(const TerrainHeightCodec& codec, int64 code) noexcept;

/// Quantize a height with the canonical tile rule: nearest code, half-way values toward +infinity.
/// @return False, preserving outCode, for a nonfinite height or one outside the codec's closed range.
[[nodiscard]] bool
TryEncodeTerrainHeight(const TerrainHeightCodec& codec, float64 heightMetres, int64& outCode) noexcept;

/// Validate tile metadata as TryCookTerrainTile and TryLoadTerrainTile do.
[[nodiscard]] TerrainTileStatus ValidateTerrainTileDescription(const TerrainTileDescription& description) noexcept;
} // namespace ludus::content::terrain::internal
