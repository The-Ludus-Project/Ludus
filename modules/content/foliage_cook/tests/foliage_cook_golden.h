#pragma once

// Golden foliage cook shared by the native FoliageCook tests and the WebAssembly
// math probe (tools/web-math-probe), so native and browser builds must cook
// identical LUFOL bytes from the same terrain and recipe. Header-only and free of
// the test framework.
//
// The fixture is a 2 x 2 tile terrain around the sample origin with a domain
// origin off it, so pages and cells cross the negative-coordinate seam. It has a
// steep ridge, a hole, wetness and a mask, and three strata: trees, shrubs that
// avoid tree canopies and steep ground, and wetness-limited grass that avoids
// trunks.

#include <ludus/foundation/base/core.h>

#include <ludus/content/foliage_cook/foliage_cook.h>
#include <ludus/content/terrain/terrain_tile.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::content::foliage_cook::golden
{
using foundation::float32;

/// Cells on each tile edge.
inline constexpr uint32 CELLS_PER_SIDE = 16;
/// Tiles on each grid axis.
inline constexpr uint32 TILES_PER_AXIS = 2;
/// Tiles in the fixture.
inline constexpr usize TILE_COUNT = usize{TILES_PER_AXIS} * TILES_PER_AXIS;
/// Address of the first tile; the grid covers samples [-16, 16] on both axes.
inline constexpr terrain::TerrainTileAddress MINIMUM_TILE = { .X = -1, .Z = -1 };
/// Samples on each grid axis, including the shared positive edge.
inline constexpr int64 SAMPLES_PER_AXIS = int64{CELLS_PER_SIDE} * TILES_PER_AXIS + 1;
/// Lowest global sample on each axis.
inline constexpr int64 MINIMUM_SAMPLE = -int64{CELLS_PER_SIDE};
/// Terrain revision of the fixture.
inline constexpr uint64 TERRAIN_REVISION = 11;
/// Sample spacing in metres.
inline constexpr float32 SPACING_METRES = 0.5F;
/// Height of code zero in metres.
inline constexpr float32 HEIGHT_ORIGIN_METRES = -10.0F;
/// Height per code in metres; not a dyadic fraction, so conversion rounds.
inline constexpr float32 HEIGHT_QUANTUM_METRES = 0.01F;
/// Persistent terrain domain of the fixture ("FOLCOOK" and 1).
inline constexpr uint64 DOMAIN_ID = 0x464f4c434f4f4b01ULL;
/// Terrain sample column of the lattice origin.
inline constexpr int64 ORIGIN_X = 3;
/// Terrain sample row of the lattice origin.
inline constexpr int64 ORIGIN_Z = -2;
/// Page edge: eight terrain samples.
inline constexpr uint32 PAGE_SIZE = 2048;

/// Base height code.
inline constexpr int64 BASE_CODE = 2000;
/// Height codes per sample along X.
inline constexpr int64 SLOPE_X_CODES = 23;
/// Height codes per sample along Z.
inline constexpr int64 SLOPE_Z_CODES = 9;
/// Period of the fixture's bumps, in samples.
inline constexpr int64 BUMP_PERIOD = 13;
/// Codes per bump step.
inline constexpr int64 BUMP_CODES = 2;
/// First sample column of the steep ridge.
inline constexpr int64 RIDGE_START = 7;
/// Height codes per sample on the ridge, a slope of three at this spacing.
inline constexpr int64 RIDGE_CODES = 150;
/// Hole cells: columns [HOLE_FIRST_X, HOLE_LAST_X] and rows [HOLE_FIRST_Z, HOLE_LAST_Z].
inline constexpr int64 HOLE_FIRST_X = -6;
inline constexpr int64 HOLE_LAST_X = -2;
inline constexpr int64 HOLE_FIRST_Z = 3;
inline constexpr int64 HOLE_LAST_Z = 6;
/// Wetness per sample row above the minimum row.
inline constexpr int64 WETNESS_PER_ROW = 2048;
/// ID of the fixture's mask.
inline constexpr uint16 MASK_ID = 7;
/// Mask value per sample column above MASK_ZERO_COLUMN.
inline constexpr int64 MASK_PER_COLUMN = 16384;
/// Last sample column where the mask is zero.
inline constexpr int64 MASK_ZERO_COLUMN = -10;

/// Layer IDs of the fixture.
inline constexpr uint16 TREE_LAYER = 1;
inline constexpr uint16 SHRUB_LAYER = 2;
inline constexpr uint16 GRASS_LAYER = 3;
/// Species indices of the fixture.
inline constexpr uint16 TREE_SPECIES = 0;
inline constexpr uint16 SHRUB_SPECIES = 1;
inline constexpr uint16 GRASS_SPECIES = 2;
/// Exclusion classes of the fixture.
inline constexpr usize TRUNK_CLASS = 0;
inline constexpr usize CANOPY_CLASS = 1;

/// Pinned byte count of the golden page set for FOLIAGE_COOK_GENERATOR_VERSION 1.
inline constexpr usize ENCODED_BYTES = 41332;
/// Pinned FNV-1a digest of the golden page set. A different value changes every cooked page set and requires a
/// new generator version, never an updated constant.
inline constexpr uint64 ENCODED_DIGEST = 0x35ada98dbe704a32ULL;

/// Height code of a global sample.
[[nodiscard]] constexpr int64 ComputeCode(int64 x, int64 z) noexcept
{
    int64 code = BASE_CODE + SLOPE_X_CODES * x + SLOPE_Z_CODES * z;
    const int64 bump = (x * x + 3 * z * z) % BUMP_PERIOD;
    code += BUMP_CODES * bump;
    if (x > RIDGE_START)
    {
        code += RIDGE_CODES * (x - RIDGE_START);
    }
    return code;
}

/// Whether a global cell is a hole.
[[nodiscard]] constexpr bool IsHoleCell(int64 x, int64 z) noexcept
{
    return x >= HOLE_FIRST_X && x <= HOLE_LAST_X && z >= HOLE_FIRST_Z && z <= HOLE_LAST_Z;
}

/// Clamp a value into the field range.
[[nodiscard]] constexpr uint16 ClampField(int64 value) noexcept
{
    if (value < 0)
    {
        return 0;
    }
    return value > FOLIAGE_FIELD_UNITS_ONE ? static_cast<uint16>(FOLIAGE_FIELD_UNITS_ONE) : static_cast<uint16>(value);
}

/// Load one fixture tile with codes from a code function of the global sample.
template <typename CodeFunction>
[[nodiscard]] bool TryLoadTile(terrain::TerrainTileAddress address,
                               const CodeFunction& computeCode,
                               terrain::TerrainTile& outTile) noexcept
{
    terrain::TerrainTileDescription description;
    description.Revision = TERRAIN_REVISION;
    description.Address = address;
    description.CellsPerSide = CELLS_PER_SIDE;
    description.SpacingMetres = SPACING_METRES;
    description.Codec.Encoding = terrain::TerrainHeightEncoding::Unsigned16;
    description.Codec.OriginMetres = HEIGHT_ORIGIN_METRES;
    description.Codec.QuantumMetres = HEIGHT_QUANTUM_METRES;
    constexpr usize SAMPLES_PER_TILE_AXIS = usize{CELLS_PER_SIDE} + 1;
    constexpr usize CODE_BYTES = 2;
    foundation::StaticArray<uint8, SAMPLES_PER_TILE_AXIS * SAMPLES_PER_TILE_AXIS * CODE_BYTES> bytes{};
    for (usize row = 0; row < SAMPLES_PER_TILE_AXIS; ++row)
    {
        for (usize column = 0; column < SAMPLES_PER_TILE_AXIS; ++column)
        {
            const int64 x = address.X * CELLS_PER_SIDE + static_cast<int64>(column);
            const int64 z = address.Z * CELLS_PER_SIDE + static_cast<int64>(row);
            const uint16 code = static_cast<uint16>(computeCode(x, z));
            const usize offset = (row * SAMPLES_PER_TILE_AXIS + column) * CODE_BYTES;
            if (foundation::TryWriteLittleEndian(
                    code,
                    foundation::BorrowedView<uint8>{bytes.GetData() + offset, CODE_BYTES}) == false)
            {
                return false;
            }
        }
    }
    return terrain::TryLoadTerrainTile(description, {bytes.GetData(), bytes.GetSize()}, outTile) ==
           terrain::TerrainTileStatus::Success;
}

/// The golden cook input. Its views borrow its own storage, so it is neither copied nor moved; tests edit the
/// fields and call MakeInput again.
struct GoldenCook final
{
    GoldenCook() noexcept = default;
    GoldenCook(const GoldenCook&) = delete;
    GoldenCook& operator=(const GoldenCook&) = delete;

    foundation::StaticArray<terrain::TerrainTile, TILE_COUNT> Tiles{};
    foundation::Array<uint8> Holes;
    foundation::Array<uint16> Wetness;
    foundation::Array<uint16> MaskValues;
    foundation::StaticArray<FoliageMaskChannel, 1> Masks{};
    usize MaskCount = 1;
    foundation::StaticArray<FoliagePaletteEntry, 1> TreePalette{};
    foundation::StaticArray<FoliagePaletteEntry, 1> ShrubPalette{};
    foundation::StaticArray<FoliagePaletteEntry, 2> GrassPalette{};
    foundation::StaticArray<FoliageMaskRule, 1> ShrubMasks{};
    foundation::StaticArray<FoliageCookLayer, 3> Layers{};
    usize LayerCount = 3;
    foundation::StaticArray<FoliageCookSpecies, 3> Species{};
    foundation::StaticArray<FoliageCandidateKey, 4> Suppressions{};
    usize SuppressionCount = 1;
    FoliageCookDomain Domain{};
    bool HasHoles = true;
    bool HasWetness = true;

    /// Input borrowing the fixture's storage.
    [[nodiscard]] FoliageCookInput MakeInput() const noexcept
    {
        FoliageCookInput input;
        input.Domain = Domain;
        input.Terrain.Grid.Tiles = {Tiles.GetData(), Tiles.GetSize()};
        input.Terrain.Grid.MinimumTile = MINIMUM_TILE;
        input.Terrain.Grid.TileColumns = TILES_PER_AXIS;
        input.Terrain.Grid.TileRows = TILES_PER_AXIS;
        if (HasHoles == true)
        {
            input.Terrain.Holes = {Holes.GetData(), Holes.GetSize()};
        }
        if (HasWetness == true)
        {
            input.Terrain.Wetness = {Wetness.GetData(), Wetness.GetSize()};
        }
        input.Terrain.Masks = {Masks.GetData(), MaskCount};
        input.Layers = {Layers.GetData(), LayerCount};
        input.Species = {Species.GetData(), Species.GetSize()};
        input.Suppressions = {Suppressions.GetData(), SuppressionCount};
        return input;
    }
};

/// Fill the fixture's recipe: three strata over three species.
inline void InitializeRecipe(GoldenCook& outCook) noexcept
{
    outCook.Domain = { .Id = DOMAIN_ID, .OriginX = ORIGIN_X, .OriginZ = ORIGIN_Z, .PageSize = PAGE_SIZE };

    constexpr uint16 TREE_MINIMUM_SCALE = 3584;
    constexpr uint16 TREE_MAXIMUM_SCALE = 4608;
    constexpr uint32 TREE_TRUNK = 96;
    constexpr uint32 TREE_CANOPY = 640;
    constexpr uint32 TREE_BOUNDS = 700;
    constexpr uint32 TREE_HEIGHT = 12288;
    constexpr uint32 TREE_VARIANTS = 3;
    FoliageCookSpecies& tree = outCook.Species[TREE_SPECIES];
    tree = {};
    tree.Key = 0x1001;
    tree.PlacementRevision = 4;
    tree.VariantCount = TREE_VARIANTS;
    tree.MinimumScale = TREE_MINIMUM_SCALE;
    tree.MaximumScale = TREE_MAXIMUM_SCALE;
    tree.ExclusionRadii[TRUNK_CLASS] = TREE_TRUNK;
    tree.ExclusionRadii[CANOPY_CLASS] = TREE_CANOPY;
    tree.BoundsRadius = TREE_BOUNDS;
    tree.BoundsHeight = TREE_HEIGHT;

    constexpr uint16 SHRUB_MINIMUM_SCALE = 3072;
    constexpr uint16 SHRUB_MAXIMUM_SCALE = 5120;
    constexpr uint32 SHRUB_TRUNK = 48;
    constexpr uint32 SHRUB_CANOPY = 160;
    constexpr uint32 SHRUB_BOUNDS = 200;
    constexpr uint32 SHRUB_HEIGHT = 1536;
    FoliageCookSpecies& shrub = outCook.Species[SHRUB_SPECIES];
    shrub = {};
    shrub.Key = 0x2002;
    shrub.PlacementRevision = 2;
    shrub.VariantCount = 2;
    shrub.MinimumScale = SHRUB_MINIMUM_SCALE;
    shrub.MaximumScale = SHRUB_MAXIMUM_SCALE;
    shrub.IsAlignedToTerrain = true;
    shrub.ExclusionRadii[TRUNK_CLASS] = SHRUB_TRUNK;
    shrub.ExclusionRadii[CANOPY_CLASS] = SHRUB_CANOPY;
    shrub.BoundsRadius = SHRUB_BOUNDS;
    shrub.BoundsHeight = SHRUB_HEIGHT;

    constexpr uint16 GRASS_MINIMUM_SCALE = 2048;
    constexpr uint16 GRASS_MAXIMUM_SCALE = 6144;
    constexpr uint32 GRASS_TRUNK = 24;
    constexpr uint32 GRASS_BOUNDS = 40;
    constexpr uint32 GRASS_HEIGHT = 400;
    constexpr uint32 GRASS_VARIANTS = 4;
    FoliageCookSpecies& grass = outCook.Species[GRASS_SPECIES];
    grass = {};
    grass.Key = 0x3003;
    grass.PlacementRevision = 1;
    grass.VariantCount = GRASS_VARIANTS;
    grass.MinimumScale = GRASS_MINIMUM_SCALE;
    grass.MaximumScale = GRASS_MAXIMUM_SCALE;
    grass.IsAlignedToTerrain = true;
    grass.ExclusionRadii[TRUNK_CLASS] = GRASS_TRUNK;
    grass.BoundsRadius = GRASS_BOUNDS;
    grass.BoundsHeight = GRASS_HEIGHT;

    constexpr uint32 GRASS_WEIGHT = 3;
    outCook.TreePalette[0] = { .Species = TREE_SPECIES, .Weight = 1 };
    outCook.ShrubPalette[0] = { .Species = SHRUB_SPECIES, .Weight = 1 };
    outCook.GrassPalette[0] = { .Species = SHRUB_SPECIES, .Weight = 1 };
    outCook.GrassPalette[1] = { .Species = GRASS_SPECIES, .Weight = GRASS_WEIGHT };
    outCook.ShrubMasks[0] = { .MaskId = MASK_ID, .IsRequired = false };

    constexpr uint32 TREE_CELL = 1024;
    constexpr uint32 TREE_DENSITY = 9830;
    constexpr uint64 TREE_SEED = 0x7265650000000001ULL;
    FoliageCookLayer& trees = outCook.Layers[0];
    trees = {};
    trees.Id = TREE_LAYER;
    trees.Stratum = 0;
    trees.SlotsPerCell = 2;
    trees.CellSize = TREE_CELL;
    trees.Seed = TREE_SEED;
    trees.Density = TREE_DENSITY;
    trees.Palette = {outCook.TreePalette.GetData(), outCook.TreePalette.GetSize()};

    constexpr uint32 SHRUB_CELL = 512;
    constexpr uint32 SHRUB_DENSITY = 39321;
    constexpr uint16 SHRUB_SLOTS = 4;
    constexpr uint64 SHRUB_SEED = 0x7368720000000002ULL;
    constexpr int64 SHRUB_MAXIMUM_SLOPE = FOLIAGE_SLOPE_UNITS_PER_ONE;
    constexpr uint32 SHRUB_SLOPE_FEATHER = FOLIAGE_SLOPE_UNITS_PER_ONE / 2;
    FoliageCookLayer& shrubs = outCook.Layers[1];
    shrubs = {};
    shrubs.Id = SHRUB_LAYER;
    shrubs.Stratum = 1;
    shrubs.SlotsPerCell = SHRUB_SLOTS;
    shrubs.CellSize = SHRUB_CELL;
    shrubs.Seed = SHRUB_SEED;
    shrubs.Density = SHRUB_DENSITY;
    shrubs.Palette = {outCook.ShrubPalette.GetData(), outCook.ShrubPalette.GetSize()};
    shrubs.Slope = { .IsEnabled = true, .Minimum = 0, .Maximum = SHRUB_MAXIMUM_SLOPE, .Feather = SHRUB_SLOPE_FEATHER };
    shrubs.Masks = {outCook.ShrubMasks.GetData(), outCook.ShrubMasks.GetSize()};

    constexpr uint32 GRASS_CELL = 256;
    constexpr uint32 GRASS_DENSITY = 524288;
    constexpr uint16 GRASS_SLOTS = 4;
    constexpr uint64 GRASS_SEED = 0x6772610000000003ULL;
    constexpr int64 GRASS_MINIMUM_HEIGHT = int64{5} * foliage_format::ROOT_HEIGHT_UNITS_PER_METRE;
    constexpr int64 GRASS_MAXIMUM_HEIGHT = int64{30} * foliage_format::ROOT_HEIGHT_UNITS_PER_METRE;
    constexpr uint32 GRASS_HEIGHT_FEATHER = 2 * foliage_format::ROOT_HEIGHT_UNITS_PER_METRE;
    constexpr int64 GRASS_MINIMUM_WETNESS = 16384;
    constexpr uint32 GRASS_WETNESS_FEATHER = 8192;
    FoliageCookLayer& grassLayer = outCook.Layers[2];
    grassLayer = {};
    grassLayer.Id = GRASS_LAYER;
    grassLayer.Stratum = 2;
    grassLayer.SlotsPerCell = GRASS_SLOTS;
    grassLayer.CellSize = GRASS_CELL;
    grassLayer.Seed = GRASS_SEED;
    grassLayer.Density = GRASS_DENSITY;
    grassLayer.Palette = {outCook.GrassPalette.GetData(), outCook.GrassPalette.GetSize()};
    grassLayer.Height =
    {
        .IsEnabled = true,
        .Minimum = GRASS_MINIMUM_HEIGHT,
        .Maximum = GRASS_MAXIMUM_HEIGHT,
        .Feather = GRASS_HEIGHT_FEATHER,
    };
    grassLayer.Wetness =
    {
        .IsEnabled = true,
        .Minimum = GRASS_MINIMUM_WETNESS,
        .Maximum = FOLIAGE_FIELD_UNITS_ONE,
        .Feather = GRASS_WETNESS_FEATHER,
    };
    grassLayer.IsWetnessRequired = true;

    outCook.LayerCount = 3;
    outCook.Suppressions[0] = { .Layer = GRASS_LAYER, .Slot = 1, .CellX = 0, .CellZ = 0 };
    outCook.SuppressionCount = 1;
    outCook.HasHoles = true;
    outCook.HasWetness = true;
    outCook.MaskCount = 1;
}

/// Load the fixture's terrain and channels with a code function, and fill its recipe.
template <typename CodeFunction>
[[nodiscard]] bool TryInitializeGoldenCook(GoldenCook& outCook, const CodeFunction& computeCode) noexcept
{
    for (usize i = 0; i < TILE_COUNT; ++i)
    {
        const terrain::TerrainTileAddress address
        {
            .X = MINIMUM_TILE.X + static_cast<int64>(i % TILES_PER_AXIS),
            .Z = MINIMUM_TILE.Z + static_cast<int64>(i / TILES_PER_AXIS),
        };
        if (TryLoadTile(address, computeCode, outCook.Tiles[i]) == false)
        {
            return false;
        }
    }
    constexpr int64 CELLS_PER_AXIS = SAMPLES_PER_AXIS - 1;
    if (outCook.Holes.TryResize(static_cast<usize>(CELLS_PER_AXIS * CELLS_PER_AXIS)) == false ||
        outCook.Wetness.TryResize(static_cast<usize>(SAMPLES_PER_AXIS * SAMPLES_PER_AXIS)) == false ||
        outCook.MaskValues.TryResize(static_cast<usize>(SAMPLES_PER_AXIS * SAMPLES_PER_AXIS)) == false)
    {
        return false;
    }
    for (int64 row = 0; row < CELLS_PER_AXIS; ++row)
    {
        for (int64 column = 0; column < CELLS_PER_AXIS; ++column)
        {
            const bool isHole = IsHoleCell(MINIMUM_SAMPLE + column, MINIMUM_SAMPLE + row);
            outCook.Holes[static_cast<usize>(row * CELLS_PER_AXIS + column)] = isHole == true ? 1 : 0;
        }
    }
    for (int64 row = 0; row < SAMPLES_PER_AXIS; ++row)
    {
        for (int64 column = 0; column < SAMPLES_PER_AXIS; ++column)
        {
            const usize index = static_cast<usize>(row * SAMPLES_PER_AXIS + column);
            outCook.Wetness[index] = ClampField(row * WETNESS_PER_ROW);
            outCook.MaskValues[index] = ClampField((MINIMUM_SAMPLE + column - MASK_ZERO_COLUMN) * MASK_PER_COLUMN);
        }
    }
    outCook.Masks[0] = { .Id = MASK_ID, .Values = {outCook.MaskValues.GetData(), outCook.MaskValues.GetSize()} };
    InitializeRecipe(outCook);
    return true;
}

/// Load the golden fixture.
[[nodiscard]] inline bool TryInitializeGoldenCook(GoldenCook& outCook) noexcept
{
    return TryInitializeGoldenCook(outCook, [](int64 x, int64 z) noexcept { return ComputeCode(x, z); });
}

/// Cook every terrain page of an input and encode one page set.
[[nodiscard]] inline FoliageCookStatus TryCookAll(const FoliageCookInput& input,
                                                  foundation::Array<uint8>& outBytes,
                                                  FoliageCookDiagnostic& outDiagnostic) noexcept
{
    foundation::Array<FoliagePageAddress> pages;
    FoliageCookStatus status = TryGetFoliageTerrainPages(input, pages, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    FoliageCookedPages cooked;
    status = TryCookFoliagePages(input, {pages.GetData(), pages.GetSize()}, cooked, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    return TryWriteFoliageCookedPageSet(input, cooked, outBytes, outDiagnostic);
}
} // namespace ludus::content::foliage_cook::golden
