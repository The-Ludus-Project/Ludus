#include <ludus/content/terrain/terrain_surface.h>
#include <ludus/content/terrain/terrain_tile.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
namespace terrain = ludus::content::terrain;
namespace math = ludus::foundation::math;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::int64;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using terrain::TerrainTileStatus;

constexpr uint32 ONE_CELL = 1;
constexpr uint32 TWO_CELLS = 2;
constexpr uint32 SMALL_AXIS = ONE_CELL + 1;
constexpr usize SMALL_SAMPLE_COUNT = SMALL_AXIS * SMALL_AXIS;
constexpr usize SMALL_INDEX_COUNT = 6;
using SmallHeights = StaticArray<float32, SMALL_SAMPLE_COUNT>;
using SmallPositions = StaticArray<math::Vector3d, SMALL_SAMPLE_COUNT>;
using SmallIndices = StaticArray<uint32, SMALL_INDEX_COUNT>;
constexpr terrain::TerrainFieldGrid SMALL_GRID{ .Columns = SMALL_AXIS, .Rows = SMALL_AXIS };
constexpr terrain::TerrainTileDescription SMALL_TILE{ .Revision = 1, .CellsPerSide = ONE_CELL };

void CookSaddle(terrain::TerrainTile& outTile)
{
    constexpr SmallHeights HEIGHTS{0, 0, 0, 4};
    REQUIRE(terrain::TryCookTerrainTile(SMALL_TILE, SMALL_GRID, HEIGHTS, 0, 0, outTile) == TerrainTileStatus::Success);
}
} // namespace

TEST_CASE("Terrain cells use floor division and tile endpoints use checked integer arithmetic", "[terrain][tile]")
{
    terrain::TerrainCellAddress address;
    REQUIRE(terrain::TryLocateTerrainCell({-1, -2}, TWO_CELLS, address) == TerrainTileStatus::Success);
    CHECK(address.Tile.X == -1);
    CHECK(address.Tile.Z == -1);
    CHECK(address.X == 1);
    CHECK(address.Z == 0);
    REQUIRE(terrain::TryLocateTerrainCell({-3, 2}, TWO_CELLS, address) == TerrainTileStatus::Success);
    CHECK(address.Tile.X == -2);
    CHECK(address.Tile.Z == 1);
    CHECK(address.X == 1);
    CHECK(address.Z == 0);

    constexpr int64 MAX_ADDRESS = 0x7fffffffffffffff;
    constexpr int64 MIN_ADDRESS = -MAX_ADDRESS - 1;
    REQUIRE(terrain::TryLocateTerrainCell({MIN_ADDRESS, MAX_ADDRESS}, 3, address) == TerrainTileStatus::Success);
    CHECK(address.X == 1);
    CHECK(address.Z == 1);
    CHECK(terrain::TryLocateTerrainCell({0, 0}, 0, address) == TerrainTileStatus::InvalidArgument);
    CHECK(address.X == 1);
    CHECK(address.Z == 1);

    terrain::TerrainSampleAddress origin{11, 12};
    CHECK(terrain::TryGetTerrainTileOrigin({MAX_ADDRESS, 0}, ONE_CELL, origin) ==
          TerrainTileStatus::CoordinateOutOfRange);
    CHECK(terrain::TryGetTerrainTileOrigin({MIN_ADDRESS, 0}, TWO_CELLS, origin) ==
          TerrainTileStatus::CoordinateOutOfRange);
    CHECK(origin.X == 11);
    CHECK(origin.Z == 12);
    REQUIRE(terrain::TryGetTerrainTileOrigin({MIN_ADDRESS, MAX_ADDRESS - 1}, ONE_CELL, origin) ==
            TerrainTileStatus::Success);
    CHECK(origin.X == MIN_ADDRESS);
    CHECK(origin.Z == MAX_ADDRESS - 1);
}

TEST_CASE("Terrain quantizes with a shared origin and round-half-up little-endian codes", "[terrain][tile]")
{
    terrain::TerrainTileDescription description = SMALL_TILE;
    description.Codec =
    {
        .Encoding = terrain::TerrainHeightEncoding::Unsigned16,
        .OriginMetres = 10,
        .QuantumMetres = 0.5F,
    };
    const SmallHeights unsignedHeights{10, 10.25F, 138.5F, 32777.5F};
    terrain::TerrainTile tile;
    REQUIRE(terrain::TryCookTerrainTile(description, SMALL_GRID, unsignedHeights, 0, 0, tile) ==
            TerrainTileStatus::Success);
    const BorrowedView<const uint8> unsignedBytes = tile.GetEncodedHeights();
    CHECK(unsignedBytes.GetSize() == SMALL_SAMPLE_COUNT * sizeof(ludus::foundation::uint16));
    CHECK(unsignedBytes[0] == 0);
    CHECK(unsignedBytes[2] == 1);
    CHECK(unsignedBytes[4] == 1);
    CHECK(unsignedBytes[5] == 1);
    CHECK(unsignedBytes[6] == 0xff);
    CHECK(unsignedBytes[7] == 0xff);

    terrain::TerrainSurfaceSample sample;
    REQUIRE(terrain::TrySampleTerrainTile(tile, 1, 0, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == 10.5);
    CHECK(sample.HeightMetres - static_cast<float64>(unsignedHeights[1]) ==
          static_cast<float64>(description.Codec.QuantumMetres) * 0.5);

    description.Codec = { .Encoding = terrain::TerrainHeightEncoding::Signed32, .OriginMetres = 0, .QuantumMetres = 1 };
    constexpr SmallHeights SIGNED_HEIGHTS{-2147483648.0F, -1.5F, -0.5F, 16777216.0F};
    REQUIRE(terrain::TryCookTerrainTile(description, SMALL_GRID, SIGNED_HEIGHTS, 0, 0, tile) ==
            TerrainTileStatus::Success);
    const BorrowedView<const uint8> signedBytes = tile.GetEncodedHeights();
    CHECK(signedBytes.GetSize() == SMALL_SAMPLE_COUNT * sizeof(ludus::foundation::int32));
    CHECK(signedBytes[0] == 0);
    CHECK(signedBytes[3] == 0x80);
    CHECK(signedBytes[4] == 0xff);
    CHECK(signedBytes[7] == 0xff);
    CHECK(signedBytes[8] == 0);
    CHECK(signedBytes[15] == 1);

    terrain::TerrainTile loaded;
    REQUIRE(terrain::TryLoadTerrainTile(description, signedBytes, loaded) == TerrainTileStatus::Success);
    REQUIRE(terrain::TrySampleTerrainTile(loaded, 0, 0, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == static_cast<float64>(SIGNED_HEIGHTS[0]));
    REQUIRE(terrain::TrySampleTerrainTile(loaded, 1, 0, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == -1);
    REQUIRE(terrain::TrySampleTerrainTile(loaded, 0, 1, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == 0);
    REQUIRE(terrain::TrySampleTerrainTile(loaded, 1, 1, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == static_cast<float64>(SIGNED_HEIGHTS[3]));

    // Load an arbitrary signed payload that includes a code binary32 elevations cannot represent exactly.
    constexpr usize SIGNED_PAYLOAD_SIZE = SMALL_SAMPLE_COUNT * sizeof(ludus::foundation::int32);
    constexpr StaticArray<uint8, SIGNED_PAYLOAD_SIZE> MAX_SIGNED_BYTES{0xff, 0xff, 0xff, 0x7f};
    REQUIRE(terrain::TryLoadTerrainTile(description, MAX_SIGNED_BYTES, loaded) == TerrainTileStatus::Success);
    REQUIRE(terrain::TrySampleTerrainTile(loaded, 0, 0, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == 2147483647.0);
}

TEST_CASE("Four terrain tiles agree on their duplicated negative-address edges and centre corner", "[terrain][tile]")
{
    constexpr uint32 AXIS = 5;
    constexpr usize SAMPLE_COUNT = AXIS * AXIS;
    constexpr terrain::TerrainFieldGrid GRID{ .Columns = AXIS, .Rows = AXIS };
    StaticArray<float32, SAMPLE_COUNT> heights{};
    for (usize index = 0; index < SAMPLE_COUNT; ++index)
    {
        heights[index] = static_cast<float32>(index * index) * 0.25F;
    }
    terrain::TerrainTileDescription description{ .Revision = 7, .Address = {-1, -1}, .CellsPerSide = TWO_CELLS };
    description.Codec.QuantumMetres = 0.5F;
    terrain::TerrainTile northWest;
    terrain::TerrainTile northEast;
    terrain::TerrainTile southWest;
    terrain::TerrainTile southEast;
    REQUIRE(terrain::TryCookTerrainTile(description, GRID, heights, 0, 0, northWest) == TerrainTileStatus::Success);
    description.Address.X = 0;
    REQUIRE(terrain::TryCookTerrainTile(description, GRID, heights, TWO_CELLS, 0, northEast) ==
            TerrainTileStatus::Success);
    description.Address = {-1, 0};
    REQUIRE(terrain::TryCookTerrainTile(description, GRID, heights, 0, TWO_CELLS, southWest) ==
            TerrainTileStatus::Success);
    description.Address.X = 0;
    REQUIRE(terrain::TryCookTerrainTile(description, GRID, heights, TWO_CELLS, TWO_CELLS, southEast) ==
            TerrainTileStatus::Success);

    CHECK(terrain::CheckTerrainTileSeam(northWest, northEast) == TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(northWest, southWest) == TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(southEast, northEast) == TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(southEast, southWest) == TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(southEast, northWest) == TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(northEast, southWest) == TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(northEast, northEast) == TerrainTileStatus::InvalidArgument);

    terrain::TerrainSurfaceSample west;
    terrain::TerrainSurfaceSample east;
    REQUIRE(terrain::TrySampleTerrainTile(northWest, 2, 0.375, west) == TerrainTileStatus::Success);
    REQUIRE(terrain::TrySampleTerrainTile(northEast, 0, 0.375, east) == TerrainTileStatus::Success);
    CHECK(west.HeightMetres == east.HeightMetres);
    CHECK(west.HeightMetres == 5.3125);

    constexpr usize TILE_SAMPLE_COUNT = 9;
    constexpr usize TILE_INDEX_COUNT = 24;
    StaticArray<math::Vector3d, TILE_SAMPLE_COUNT> northWestPositions{};
    StaticArray<math::Vector3d, TILE_SAMPLE_COUNT> northEastPositions{};
    StaticArray<uint32, TILE_INDEX_COUNT> indices{};
    const terrain::TerrainSampleAddress meshOrigin{-2, -1};
    REQUIRE(terrain::TryExportTerrainTileMesh(northWest, meshOrigin, northWestPositions, indices) ==
            TerrainTileStatus::Success);
    REQUIRE(terrain::TryExportTerrainTileMesh(northEast, meshOrigin, northEastPositions, indices) ==
            TerrainTileStatus::Success);
    constexpr uint32 TILE_SAMPLE_AXIS = TWO_CELLS + 1;
    for (uint32 row = 0; row < TILE_SAMPLE_AXIS; ++row)
    {
        CHECK(northWestPositions[row * TILE_SAMPLE_AXIS + TWO_CELLS] == northEastPositions[row * TILE_SAMPLE_AXIS]);
    }

    heights[TWO_CELLS * AXIS + TWO_CELLS] += 1;
    REQUIRE(terrain::TryCookTerrainTile(description, GRID, heights, TWO_CELLS, TWO_CELLS, southEast) ==
            TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(northWest, southEast) == TerrainTileStatus::SeamMismatch);
    CHECK(terrain::CheckTerrainTileSeam(northEast, southEast) == TerrainTileStatus::SeamMismatch);
    ++description.Revision;
    REQUIRE(terrain::TryCookTerrainTile(description, GRID, heights, TWO_CELLS, TWO_CELLS, southEast) ==
            TerrainTileStatus::Success);
    CHECK(terrain::CheckTerrainTileSeam(northWest, southEast) == TerrainTileStatus::InvalidArgument);
}

TEST_CASE("Headless queries and indexed mesh export use the same saddle triangles", "[terrain][tile]")
{
    terrain::TerrainTile tile;
    CookSaddle(tile);
    const terrain::TerrainTileMeshCounts counts = terrain::GetTerrainTileMeshCounts(tile);
    CHECK(counts.Positions == SMALL_SAMPLE_COUNT);
    CHECK(counts.Indices == SMALL_INDEX_COUNT);
    SmallPositions positions{};
    SmallIndices indices{};
    REQUIRE(terrain::TryExportTerrainTileMesh(tile, {0, 0}, positions, indices) == TerrainTileStatus::Success);
    CHECK(indices == SmallIndices{0, 3, 1, 0, 2, 3});
    CHECK(positions[3] == math::Vector3d{1, 4, 1});

    const math::Vector3d firstNormal =
        math::Cross(positions[indices[1]] - positions[indices[0]], positions[indices[2]] - positions[indices[0]]);
    CHECK(firstNormal.Y > 0);
    terrain::TerrainSurfaceSample sample;
    REQUIRE(terrain::TrySampleTerrainTile(tile, 0.5, 0.5, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == 2);
    CHECK(sample.NormalTerrain == firstNormal * (1 / math::Length(firstNormal)));
    REQUIRE(terrain::TrySampleTerrainTile(tile, 0.25, 0.75, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == 1);
    CHECK(sample.NormalTerrain.X < 0);
    CHECK(sample.NormalTerrain.Y > 0);
    CHECK(sample.NormalTerrain.Z == 0);

    const terrain::TerrainTileRay vertical
    {
        .OriginTile = {0.5, 5, 0.5},
        .DirectionTerrain = {0, -2, 0},
        .MaximumDistanceMetres = 3,
    };
    terrain::TerrainTileHit hit;
    REQUIRE(terrain::TryRaycastTerrainTile(tile, vertical, 1, hit) == TerrainTileStatus::Success);
    CHECK(hit.DistanceMetres == 3);
    CHECK(hit.PositionTile == math::Vector3d{0.5, 2, 0.5});
    CHECK(hit.NormalTerrain == firstNormal * (1 / math::Length(firstNormal)));

    const terrain::TerrainTileRay oblique
    {
        .OriginTile = {0, 1, 0.5},
        .DirectionTerrain = {2, 0, 0},
        .MaximumDistanceMetres = 1,
    };
    REQUIRE(terrain::TryRaycastTerrainTile(tile, oblique, 1, hit) == TerrainTileStatus::Success);
    CHECK(hit.DistanceMetres == Catch::Approx(0.25));
    CHECK(hit.PositionTile == math::Vector3d{0.25, 1, 0.5});

    const terrain::TerrainTileRay boundary
    {
        .OriginTile = {1, 5, 1},
        .DirectionTerrain = {0, -1, 0},
        .MaximumDistanceMetres = 1,
    };
    REQUIRE(terrain::TryRaycastTerrainTile(tile, boundary, 1, hit) == TerrainTileStatus::Success);
    CHECK(hit.PositionTile == math::Vector3d{1, 4, 1});
}

TEST_CASE("A ray with multiple intersections returns the nearest hit in either direction", "[terrain][tile]")
{
    constexpr uint32 AXIS = TWO_CELLS + 1;
    constexpr usize SAMPLES = AXIS * AXIS;
    constexpr StaticArray<float32, SAMPLES> RIDGE{0, 4, 0, 0, 4, 0, 0, 4, 0};
    terrain::TerrainTile tile;
    REQUIRE(terrain::TryCookTerrainTile({ .CellsPerSide = TWO_CELLS }, {AXIS, AXIS}, RIDGE, 0, 0, tile) ==
            TerrainTileStatus::Success);
    terrain::TerrainTileRay ray
    {
        .OriginTile = {2, 1, 0.5},
        .DirectionTerrain = {-1, 0, 0},
        .MaximumDistanceMetres = 2,
    };
    terrain::TerrainTileHit hit;
    REQUIRE(terrain::TryRaycastTerrainTile(tile, ray, TWO_CELLS, hit) == TerrainTileStatus::Success);
    CHECK(hit.DistanceMetres == 0.25);
    CHECK(hit.PositionTile.X == 1.75);
    ray.OriginTile.X = 0;
    ray.DirectionTerrain.X = 1;
    REQUIRE(terrain::TryRaycastTerrainTile(tile, ray, TWO_CELLS, hit) == TerrainTileStatus::Success);
    CHECK(hit.DistanceMetres == 0.25);
    CHECK(hit.PositionTile.X == 0.25);
}

TEST_CASE("Terrain failures preserve an existing tile, contact, and both mesh buffers", "[terrain][tile]")
{
    terrain::TerrainTile tile;
    CookSaddle(tile);
    const BorrowedView<const uint8> bytes = tile.GetEncodedHeights();
    const SmallHeights belowRange{0, 0, 0, -0.25F};
    const SmallHeights aboveRange{0, 0, 0, 65535.25F};
    CHECK(terrain::TryCookTerrainTile(SMALL_TILE, SMALL_GRID, belowRange, 0, 0, tile) ==
          TerrainTileStatus::HeightOutOfRange);
    CHECK(terrain::TryCookTerrainTile(SMALL_TILE, SMALL_GRID, aboveRange, 0, 0, tile) ==
          TerrainTileStatus::HeightOutOfRange);
    CHECK(terrain::TryCookTerrainTile(SMALL_TILE, SMALL_GRID, {}, 0, 0, tile) == TerrainTileStatus::InvalidStorage);
    CHECK(terrain::TryCookTerrainTile(SMALL_TILE, SMALL_GRID, belowRange, 1, 0, tile) ==
          TerrainTileStatus::InvalidArgument);
    CHECK(terrain::TryLoadTerrainTile(SMALL_TILE, {bytes.GetDataOrNull(), bytes.GetSize() - 1}, tile) ==
          TerrainTileStatus::InvalidStorage);
    constexpr usize OVERSIZED_PAYLOAD = SMALL_SAMPLE_COUNT * sizeof(ludus::foundation::uint16) + 1;
    constexpr StaticArray<uint8, OVERSIZED_PAYLOAD> EXTRA_BYTES{};
    CHECK(terrain::TryLoadTerrainTile(SMALL_TILE, EXTRA_BYTES, tile) == TerrainTileStatus::InvalidStorage);
    terrain::TerrainTileDescription invalid = SMALL_TILE;
    invalid.Codec.QuantumMetres = 0;
    CHECK(terrain::TryLoadTerrainTile(invalid, bytes, tile) == TerrainTileStatus::InvalidArgument);
    CHECK(tile.GetEncodedHeights().GetDataOrNull() == bytes.GetDataOrNull());
    REQUIRE(terrain::TryLoadTerrainTile(SMALL_TILE, bytes, tile) == TerrainTileStatus::Success);
    terrain::TerrainSurfaceSample sample;
    REQUIRE(terrain::TrySampleTerrainTile(tile, 1, 1, sample) == TerrainTileStatus::Success);
    CHECK(sample.HeightMetres == 4);
    const terrain::TerrainSurfaceSample previous = sample;
    CHECK(terrain::TrySampleTerrainTile(tile, -0.25, 0, sample) == TerrainTileStatus::OutsideDomain);
    CHECK(sample.HeightMetres == previous.HeightMetres);
    CHECK(sample.NormalTerrain == previous.NormalTerrain);

    const terrain::TerrainTileRay ray
    {
        .OriginTile = {0.5, 5, 0.5},
        .DirectionTerrain = {0, -1, 0},
        .MaximumDistanceMetres = 2,
    };
    terrain::TerrainTileHit hit{ .DistanceMetres = 7, .PositionTile = {1, 2, 3}, .NormalTerrain = {0, 1, 0} };
    CHECK(terrain::TryRaycastTerrainTile(tile, ray, 0, hit) == TerrainTileStatus::BudgetExceeded);
    CHECK(terrain::TryRaycastTerrainTile(tile, ray, 1, hit) == TerrainTileStatus::Miss);
    CHECK(terrain::TryRaycastTerrainTile(tile, {}, 1, hit) == TerrainTileStatus::InvalidArgument);
    CHECK(hit.DistanceMetres == 7);
    CHECK(hit.PositionTile == math::Vector3d{1, 2, 3});
    CHECK(hit.NormalTerrain == math::Vector3d{0, 1, 0});

    SmallPositions positions{math::Vector3d{8, 9, 10}};
    SmallIndices indices{77};
    const SmallPositions previousPositions = positions;
    const SmallIndices previousIndices = indices;
    CHECK(terrain::TryExportTerrainTileMesh(tile, {}, positions, {indices.GetData(), SMALL_INDEX_COUNT - 1}) ==
          TerrainTileStatus::CapacityExceeded);
    CHECK(
        terrain::TryExportTerrainTileMesh(tile, {terrain::MAX_TERRAIN_MESH_OFFSET_CELLS + 1, 0}, positions, indices) ==
        TerrainTileStatus::CoordinateOutOfRange);
    CHECK(positions == previousPositions);
    CHECK(indices == previousIndices);
}

TEST_CASE("The largest terrain tile keeps metre spacing after rebasing large signed addresses", "[terrain][tile]")
{
    constexpr uint32 CELLS = terrain::MAX_TERRAIN_TILE_AXIS_CELLS;
    constexpr uint32 AXIS = CELLS + 1;
    constexpr usize SAMPLES = AXIS * AXIS;
    constexpr int64 TILE_X = -(int64{1} << 53);
    constexpr int64 TILE_Z = int64{1} << 53;
    constexpr terrain::TerrainTileDescription DESCRIPTION
    {
        .Address = {TILE_X, TILE_Z},
        .CellsPerSide = CELLS,
        .SpacingMetres = 2,
    };
    const terrain::TerrainFieldGrid grid{AXIS, AXIS, DESCRIPTION.SpacingMetres, DESCRIPTION.SpacingMetres};
    Array<float32> heights;
    REQUIRE(heights.TryResize(SAMPLES, 1) == true);
    terrain::TerrainTile tile;
    REQUIRE(terrain::TryCookTerrainTile(DESCRIPTION, grid, heights, 0, 0, tile) == TerrainTileStatus::Success);
    terrain::TerrainSampleAddress origin;
    REQUIRE(terrain::TryGetTerrainTileOrigin(DESCRIPTION.Address, CELLS, origin) == TerrainTileStatus::Success);
    const terrain::TerrainTileMeshCounts counts = terrain::GetTerrainTileMeshCounts(tile);
    CHECK(counts.Positions == SAMPLES);
    CHECK(counts.Indices == static_cast<usize>(CELLS) * CELLS * SMALL_INDEX_COUNT);

    constexpr uint32 UNUSED_INDEX = 0xffffffff;
    const math::Vector3d unusedPosition{-1, -1, -1};
    Array<math::Vector3d> positions;
    Array<uint32> indices;
    REQUIRE(positions.TryResize(counts.Positions + 1, unusedPosition) == true);
    REQUIRE(indices.TryResize(counts.Indices + 1, UNUSED_INDEX) == true);
    REQUIRE(terrain::TryExportTerrainTileMesh(tile, origin, positions, indices) == TerrainTileStatus::Success);
    CHECK(positions[0] == math::Vector3d{0, 1, 0});
    CHECK(positions[1] == math::Vector3d{2, 1, 0});
    CHECK(positions[SAMPLES - 1] == math::Vector3d{512, 1, 512});
    CHECK(positions[counts.Positions] == unusedPosition);
    CHECK(indices[counts.Indices - 1] == SAMPLES - 1);
    CHECK(indices[counts.Indices] == UNUSED_INDEX);

    const terrain::TerrainTileRay cornerRay
    {
        .OriginTile = {2, 0, 2},
        .DirectionTerrain = {0, 5, 0},
        .MaximumDistanceMetres = 1,
    };
    terrain::TerrainTileHit hit;
    CHECK(terrain::TryRaycastTerrainTile(tile, cornerRay, 3, hit) == TerrainTileStatus::BudgetExceeded);
    REQUIRE(terrain::TryRaycastTerrainTile(tile, cornerRay, 4, hit) == TerrainTileStatus::Success);
    CHECK(hit.PositionTile == math::Vector3d{2, 1, 2});
    CHECK(hit.NormalTerrain == math::Vector3d{0, 1, 0});
}
