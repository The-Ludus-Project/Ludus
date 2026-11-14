#include <ludus/content/terrain/terrain_surface.h>

#include "internal/terrain_tile_heights.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::terrain
{
namespace
{
namespace math = foundation::math;
using foundation::BorrowedView;
using foundation::StaticArray;
using math::Vector3d;

constexpr uint32 TRIANGLES_PER_CELL = 2;
constexpr uint32 CORNERS_PER_TRIANGLE = 3;
constexpr uint32 CELL_CORNER_COUNT = 4;
constexpr uint32 CORNER_MINIMUM = 0;
constexpr uint32 CORNER_POSITIVE_X = 1;
constexpr uint32 CORNER_POSITIVE_Z = 2;
constexpr uint32 CORNER_MAXIMUM = 3;
using Triangle = StaticArray<uint32, CORNERS_PER_TRIANGLE>;
using CellCorners = StaticArray<Vector3d, CELL_CORNER_COUNT>;
constexpr StaticArray<Triangle, TRIANGLES_PER_CELL> CELL_TRIANGLES{
    Triangle{CORNER_MINIMUM, CORNER_MAXIMUM, CORNER_POSITIVE_X},
    Triangle{CORNER_MINIMUM, CORNER_POSITIVE_Z, CORNER_MAXIMUM},
};

CellCorners GetCellCorners(const TerrainTile& tile, uint32 column, uint32 row, float64 spacingMetres) noexcept
{
    const float64 minimumX = column * spacingMetres;
    const float64 minimumZ = row * spacingMetres;
    const float64 maximumX = (column + 1) * spacingMetres;
    const float64 maximumZ = (row + 1) * spacingMetres;
    return {
        Vector3d{minimumX, internal::DecodeTerrainTileHeight(tile, column, row), minimumZ},
        Vector3d{maximumX, internal::DecodeTerrainTileHeight(tile, column + 1, row), minimumZ},
        Vector3d{minimumX, internal::DecodeTerrainTileHeight(tile, column, row + 1), maximumZ},
        Vector3d{maximumX, internal::DecodeTerrainTileHeight(tile, column + 1, row + 1), maximumZ},
    };
}

Vector3d ComputeContactNormal(const CellCorners& corners, const Triangle& triangle) noexcept
{
    const Vector3d normal =
        math::Cross(corners[triangle[1]] - corners[triangle[0]], corners[triangle[2]] - corners[triangle[0]]);
    return normal * (1 / math::Length(normal));
}

float64 GetMaximumMagnitude(Vector3d vector) noexcept
{
    return math::Max(math::Abs(vector.X), math::Max(math::Abs(vector.Y), math::Abs(vector.Z)));
}

constexpr uint32 HORIZONTAL_X_AXIS = 0;
constexpr uint32 ELEVATION_AXIS = 1;
constexpr uint32 HORIZONTAL_Z_AXIS = 2;
constexpr uint32 SPATIAL_AXIS_COUNT = 3;

float64 GetComponent(Vector3d vector, uint32 axis) noexcept
{
    if (axis == HORIZONTAL_X_AXIS)
    {
        return vector.X;
    }
    return axis == ELEVATION_AXIS ? vector.Y : vector.Z;
}

// Thanks to Sven Woop, Carsten Benthin and Ingo Wald, "Watertight Ray/Triangle Intersection",
// JCGT 2(1), 2013, pp. 65–82, https://jcgt.org/published/0002/01/05/. This uses the dominant-axis
// shear and signed edge functions also used by PhysicsCollision's mesh query. Here the vertices
// are the canonical grid in binary64 and the ray is in the tile's own metric frame.
struct RayProjection final
{
    uint32 AxisX = 0;
    uint32 AxisY = 0;
    uint32 AxisZ = 0;
    float64 ShearX = 0;
    float64 ShearY = 0;
    float64 DepthScale = 1;
};

RayProjection MakeRayProjection(Vector3d directionTerrain) noexcept
{
    const Vector3d magnitude{math::Abs(directionTerrain.X),
                             math::Abs(directionTerrain.Y),
                             math::Abs(directionTerrain.Z)};
    const uint32 axisZ = magnitude.X > magnitude.Y ? (magnitude.X > magnitude.Z ? HORIZONTAL_X_AXIS : HORIZONTAL_Z_AXIS)
                                                   : (magnitude.Y > magnitude.Z ? ELEVATION_AXIS : HORIZONTAL_Z_AXIS);
    const float64 dominant = GetComponent(directionTerrain, axisZ);
    const uint32 firstAxis = (axisZ + 1) % SPATIAL_AXIS_COUNT;
    const uint32 secondAxis = (axisZ + 2) % SPATIAL_AXIS_COUNT;
    const uint32 axisX = dominant < 0 ? secondAxis : firstAxis;
    const uint32 axisY = dominant < 0 ? firstAxis : secondAxis;
    return {axisX,
            axisY,
            axisZ,
            GetComponent(directionTerrain, axisX) / dominant,
            GetComponent(directionTerrain, axisY) / dominant,
            1 / dominant};
}

Vector3d ProjectRayCorner(const RayProjection& projection, Vector3d offsetTerrain) noexcept
{
    const float64 depth = GetComponent(offsetTerrain, projection.AxisZ);
    return {GetComponent(offsetTerrain, projection.AxisX) - projection.ShearX * depth,
            GetComponent(offsetTerrain, projection.AxisY) - projection.ShearY * depth,
            projection.DepthScale * depth};
}

bool TryIntersectTriangle(const CellCorners& rayCorners,
                          const Triangle& triangle,
                          float64 maximumDistanceMetres,
                          float64& outDistanceMetres) noexcept
{
    const Vector3d first = rayCorners[triangle[0]];
    const Vector3d second = rayCorners[triangle[1]];
    const Vector3d third = rayCorners[triangle[2]];
    const float64 edgeFirst = third.X * second.Y - third.Y * second.X;
    const float64 edgeSecond = first.X * third.Y - first.Y * third.X;
    const float64 edgeThird = second.X * first.Y - second.Y * first.X;
    if ((edgeFirst < 0 || edgeSecond < 0 || edgeThird < 0) && (edgeFirst > 0 || edgeSecond > 0 || edgeThird > 0))
    {
        return false;
    }
    const float64 determinant = edgeFirst + edgeSecond + edgeThird;
    if (determinant == 0)
    {
        return false;
    }
    const float64 sign = determinant < 0 ? -1.0 : 1.0;
    const float64 scaledDistance = (edgeFirst * first.Z + edgeSecond * second.Z + edgeThird * third.Z) * sign;
    const float64 positiveDeterminant = determinant * sign;
    if (scaledDistance < 0 || scaledDistance > maximumDistanceMetres * positiveDeterminant)
    {
        return false;
    }
    outDistanceMetres =
        scaledDistance == 0 ? 0 : math::Min(scaledDistance / positiveDeterminant, maximumDistanceMetres);
    return true;
}

struct CellRange final
{
    uint32 Begin = 0;
    uint32 End = 0;
};

struct RayAxis final
{
    float64 OriginMetres = 0;
    float64 Direction = 0;
};

// A constant axis can be restricted without a rounded segment endpoint. Include both cells of a grid line.
CellRange GetRayCellRange(RayAxis ray, const TerrainTileDescription& description) noexcept
{
    const uint32 cellsPerSide = description.CellsPerSide;
    const float64 spacingMetres = static_cast<float64>(description.SpacingMetres);
    const float64 originMetres = ray.OriginMetres;
    if (ray.Direction != 0)
    {
        return {0, cellsPerSide};
    }
    if (originMetres < 0 || originMetres > cellsPerSide * spacingMetres)
    {
        return {};
    }
    uint32 column = static_cast<uint32>(originMetres / spacingMetres);
    // Division can round up to the next integer even when the origin is below the grid line.
    if (originMetres < column * spacingMetres)
    {
        --column;
    }
    if (column > 0 && originMetres == column * spacingMetres)
    {
        return {column - 1, math::Min(column + 1, cellsPerSide)};
    }
    return {column, column + 1};
}

bool TryGetMeshAxisOffset(int64 origin, int64 meshOrigin, uint32 cellsPerSide, int64& outOffset) noexcept
{
    int64 offset = 0;
    if (foundation::TrySubtract(origin, meshOrigin, offset) == false || offset < -MAX_TERRAIN_MESH_OFFSET_CELLS ||
        offset > MAX_TERRAIN_MESH_OFFSET_CELLS - cellsPerSide)
    {
        return false;
    }
    outOffset = offset;
    return true;
}
} // namespace

TerrainTileStatus
TrySampleTerrainTile(const TerrainTile& tile, float64 xCells, float64 zCells, TerrainSurfaceSample& outSample) noexcept
{
    if (tile.IsEmpty() == true)
    {
        return TerrainTileStatus::InvalidStorage;
    }
    if (math::IsFinite(xCells) == false || math::IsFinite(zCells) == false)
    {
        return TerrainTileStatus::InvalidArgument;
    }
    const TerrainTileDescription description = tile.GetDescription();
    if (xCells < 0 || zCells < 0 || xCells > description.CellsPerSide || zCells > description.CellsPerSide)
    {
        return TerrainTileStatus::OutsideDomain;
    }

    const uint32 column = math::Min(static_cast<uint32>(xCells), description.CellsPerSide - 1);
    const uint32 row = math::Min(static_cast<uint32>(zCells), description.CellsPerSide - 1);
    const float64 x = xCells - column;
    const float64 z = zCells - row;
    const bool isPositiveXTriangle = x >= z;
    const Triangle& triangle = CELL_TRIANGLES[isPositiveXTriangle == true ? 0 : 1];
    const CellCorners corners = GetCellCorners(tile, column, row, static_cast<float64>(description.SpacingMetres));
    const float64 minimum = corners[CORNER_MINIMUM].Y;
    const float64 maximum = corners[CORNER_MAXIMUM].Y;
    const float64 outer = corners[isPositiveXTriangle == true ? CORNER_POSITIVE_X : CORNER_POSITIVE_Z].Y;
    const float64 diagonalWeight = math::Min(x, z);
    const float64 outerWeight = math::Max(x, z) - diagonalWeight;
    // Identical shared edges interpolate their two endpoints in the same order.
    const float64 height = (1 - math::Max(x, z)) * minimum + diagonalWeight * maximum + outerWeight * outer;
    outSample = {height, ComputeContactNormal(corners, triangle)};
    return TerrainTileStatus::Success;
}

TerrainTileStatus TryRaycastTerrainTile(const TerrainTile& tile,
                                        const TerrainTileRay& ray,
                                        usize cellBudget,
                                        TerrainTileHit& outHit) noexcept
{
    if (tile.IsEmpty() == true)
    {
        return TerrainTileStatus::InvalidStorage;
    }
    const float64 directionScale = GetMaximumMagnitude(ray.DirectionTerrain);
    if (math::IsFinite(ray.OriginTile) == false || math::IsFinite(ray.DirectionTerrain) == false ||
        directionScale == 0 || math::IsFinite(ray.MaximumDistanceMetres) == false || ray.MaximumDistanceMetres < 0)
    {
        return TerrainTileStatus::InvalidArgument;
    }
    // Together with binary32 spacing and codec metadata, this bound keeps the projection products finite.
    if (GetMaximumMagnitude(ray.OriginTile) > MAX_TERRAIN_TILE_RAY_METRES ||
        ray.MaximumDistanceMetres > MAX_TERRAIN_TILE_RAY_METRES)
    {
        return TerrainTileStatus::CoordinateOutOfRange;
    }
    const Vector3d scaledDirection{ray.DirectionTerrain.X / directionScale,
                                   ray.DirectionTerrain.Y / directionScale,
                                   ray.DirectionTerrain.Z / directionScale};
    const Vector3d directionTerrain = scaledDirection * (1 / math::Length(scaledDirection));
    const TerrainTileDescription description = tile.GetDescription();
    const float64 spacingMetres = static_cast<float64>(description.SpacingMetres);
    const CellRange columns =
        GetRayCellRange({ .OriginMetres = ray.OriginTile.X, .Direction = directionTerrain.X }, description);
    const CellRange rows =
        GetRayCellRange({ .OriginMetres = ray.OriginTile.Z, .Direction = directionTerrain.Z }, description);
    const usize requiredCells = static_cast<usize>(columns.End - columns.Begin) * (rows.End - rows.Begin);
    if (requiredCells > cellBudget)
    {
        return TerrainTileStatus::BudgetExceeded;
    }

    const RayProjection projection = MakeRayProjection(directionTerrain);
    TerrainTileHit hit;
    hit.DistanceMetres = ray.MaximumDistanceMetres;
    bool hasHit = false;
    for (uint32 row = rows.Begin; row < rows.End; ++row)
    {
        for (uint32 column = columns.Begin; column < columns.End; ++column)
        {
            const CellCorners corners = GetCellCorners(tile, column, row, spacingMetres);
            CellCorners rayCorners;
            for (uint32 corner = 0; corner < CELL_CORNER_COUNT; ++corner)
            {
                rayCorners[corner] = ProjectRayCorner(projection, corners[corner] - ray.OriginTile);
            }
            for (const Triangle& triangle : CELL_TRIANGLES)
            {
                float64 distance = 0;
                if (TryIntersectTriangle(rayCorners, triangle, hit.DistanceMetres, distance) == true &&
                    (hasHit == false || distance < hit.DistanceMetres))
                {
                    hasHit = true;
                    hit.DistanceMetres = distance;
                    hit.NormalTerrain = ComputeContactNormal(corners, triangle);
                }
            }
        }
    }
    if (hasHit == false)
    {
        return TerrainTileStatus::Miss;
    }
    hit.PositionTile = ray.OriginTile + directionTerrain * hit.DistanceMetres;
    outHit = hit;
    return TerrainTileStatus::Success;
}

TerrainTileMeshCounts GetTerrainTileMeshCounts(const TerrainTile& tile) noexcept
{
    if (tile.IsEmpty() == true)
    {
        return {};
    }
    const usize cellsPerSide = tile.GetDescription().CellsPerSide;
    const usize samplesPerSide = cellsPerSide + 1;
    return
    {
        .Positions = samplesPerSide * samplesPerSide,
        .Indices = cellsPerSide * cellsPerSide * TRIANGLES_PER_CELL * CORNERS_PER_TRIANGLE,
    };
}

TerrainTileStatus TryExportTerrainTileMesh(const TerrainTile& tile,
                                           TerrainSampleAddress meshOrigin,
                                           BorrowedView<Vector3d> outPositionsMesh,
                                           BorrowedView<uint32> outIndices) noexcept
{
    if (tile.IsEmpty() == true || outPositionsMesh.IsValid() == false || outIndices.IsValid() == false)
    {
        return TerrainTileStatus::InvalidStorage;
    }
    const TerrainTileMeshCounts counts = GetTerrainTileMeshCounts(tile);
    if (outPositionsMesh.GetSize() < counts.Positions || outIndices.GetSize() < counts.Indices)
    {
        return TerrainTileStatus::CapacityExceeded;
    }
    const TerrainTileDescription description = tile.GetDescription();
    const uint32 cellsPerSide = description.CellsPerSide;
    TerrainSampleAddress origin;
    LUDUS_REQUIRE(TryGetTerrainTileOrigin(description.Address, cellsPerSide, origin) == TerrainTileStatus::Success);
    int64 offsetX = 0;
    int64 offsetZ = 0;
    if (TryGetMeshAxisOffset(origin.X, meshOrigin.X, cellsPerSide, offsetX) == false ||
        TryGetMeshAxisOffset(origin.Z, meshOrigin.Z, cellsPerSide, offsetZ) == false)
    {
        return TerrainTileStatus::CoordinateOutOfRange;
    }

    const uint32 samplesPerSide = cellsPerSide + 1;
    const float64 spacingMetres = static_cast<float64>(description.SpacingMetres);
    usize index = 0;
    for (uint32 row = 0; row < samplesPerSide; ++row)
    {
        for (uint32 column = 0; column < samplesPerSide; ++column)
        {
            const uint32 vertex = row * samplesPerSide + column;
            outPositionsMesh[vertex] = {static_cast<float64>(offsetX + column) * spacingMetres,
                                        internal::DecodeTerrainTileHeight(tile, column, row),
                                        static_cast<float64>(offsetZ + row) * spacingMetres};
            if (row < cellsPerSide && column < cellsPerSide)
            {
                const StaticArray<uint32, CELL_CORNER_COUNT> corners{
                    vertex,
                    vertex + 1,
                    vertex + samplesPerSide,
                    vertex + samplesPerSide + 1,
                };
                for (const Triangle& triangle : CELL_TRIANGLES)
                {
                    for (const uint32 corner : triangle)
                    {
                        outIndices[index] = corners[corner];
                        ++index;
                    }
                }
            }
        }
    }
    return TerrainTileStatus::Success;
}
} // namespace ludus::content::terrain
