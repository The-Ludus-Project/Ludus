#pragma once

#include <ludus/content/terrain/terrain_tile.h>

namespace ludus::content::terrain::internal
{
/// Decode a sample of a validated nonempty tile. Column and row include the positive endpoints.
[[nodiscard]] float64 DecodeTerrainTileHeight(const TerrainTile& tile, uint32 column, uint32 row) noexcept;
} // namespace ludus::content::terrain::internal
