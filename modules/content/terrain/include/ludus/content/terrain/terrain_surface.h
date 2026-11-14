#pragma once

#include <ludus/content/terrain/terrain_tile.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::content::terrain
{
/// Maximum distance in cells from the mesh export origin, on each horizontal axis.
inline constexpr int64 MAX_TERRAIN_MESH_OFFSET_CELLS = int64{1} << 50;

/// Exact contact on the decoded, triangulated surface.
struct TerrainSurfaceSample final
{
    /// Elevation in metres, in the same vertical frame as the tile codec.
    float64 HeightMetres = 0;
    /// Upward unit normal of the selected triangle.
    foundation::math::Vector3d NormalTerrain{};
};

/// Sample the decoded triangles of one immutable tile without allocation.
/// @param tile Nonempty tile from a retained revision.
/// @param xCells Tile-local X in cell units, including both outer endpoints.
/// @param zCells Tile-local Z in cell units, including both outer endpoints.
/// @param outSample Contact height and normal; written only on Success.
/// Interior grid lines select the cell on their positive side. The positive tile border uses the last cell.
/// The triangle on the X >= Z side of a cell's (0,0) to (1,1) diagonal owns the diagonal.
[[nodiscard]] TerrainTileStatus
TrySampleTerrainTile(const TerrainTile& tile, float64 xCells, float64 zCells, TerrainSurfaceSample& outSample) noexcept;

/// Largest absolute ray origin component or ray length in metres.
inline constexpr float64 MAX_TERRAIN_TILE_RAY_METRES = 0x1p160;

/// A finite ray segment with tile-local XZ and elevations in the codec's vertical frame.
struct TerrainTileRay final
{
    /// Ray origin in metres; X and Z are relative to the tile's minimum sample.
    foundation::math::Vector3d OriginTile{};
    /// Finite nonzero direction; normalized by the query.
    foundation::math::Vector3d DirectionTerrain{};
    /// Finite nonnegative length in metres; both segment endpoints are included.
    float64 MaximumDistanceMetres = 0;
};

/// Nearest hit within a terrain ray segment.
struct TerrainTileHit final
{
    /// Distance along the normalized direction in metres.
    float64 DistanceMetres = 0;
    /// Hit position in the same metric frame as the ray origin.
    foundation::math::Vector3d PositionTile{};
    /// Upward unit contact normal.
    foundation::math::Vector3d NormalTerrain{};
};

/// Intersect the decoded triangles of one immutable tile without allocating or loading data.
/// @param tile Nonempty tile from a retained revision.
/// @param ray Finite segment within MAX_TERRAIN_TILE_RAY_METRES. Intersections from either side count;
/// coplanar triangles are skipped.
/// @param cellBudget Maximum number of cells the reference scan may test. The scan bounds constant horizontal
/// axes, and uses the full tile extent on moving axes. If the candidate count exceeds the budget, returns
/// BudgetExceeded.
/// @param outHit Nearest contact, written only on Success. Equal distances keep the first cell in row-major order;
/// within a cell the X >= Z triangle is tested first.
[[nodiscard]] TerrainTileStatus TryRaycastTerrainTile(const TerrainTile& tile,
                                                      const TerrainTileRay& ray,
                                                      usize cellBudget,
                                                      TerrainTileHit& outHit) noexcept;

/// Required element counts for an indexed terrain mesh.
struct TerrainTileMeshCounts final
{
    /// One position per decoded sample.
    usize Positions = 0;
    /// Two triangles per cell, with three indices per triangle.
    usize Indices = 0;
};

/// Return mesh element counts for a nonempty tile. An empty tile returns zero counts.
[[nodiscard]] TerrainTileMeshCounts GetTerrainTileMeshCounts(const TerrainTile& tile) noexcept;

/// Export the same triangles used by the CPU queries as row-major positions and upward-wound indices.
/// @param tile Nonempty tile from a retained revision.
/// @param meshOrigin Global lattice sample subtracted from all horizontal coordinates before scaling to metres.
/// Use the same origin for neighbouring tiles to get identical shared positions.
/// @param outPositionsMesh Positions in metres; Y uses the tile's decoded elevation.
/// @param outIndices Zero-based indices into the written positions. The output views must not overlap each other.
/// Only the required prefixes are written on Success; every element is preserved on failure.
[[nodiscard]] TerrainTileStatus
TryExportTerrainTileMesh(const TerrainTile& tile,
                         TerrainSampleAddress meshOrigin,
                         foundation::BorrowedView<foundation::math::Vector3d> outPositionsMesh,
                         foundation::BorrowedView<uint32> outIndices) noexcept;
} // namespace ludus::content::terrain
