// Terrain surface, rivers and water for World extraction. The fields come from Ludus::ContentTerrain; the colours,
// rule thresholds and river coefficients below are the editor's authored starting profile, not measured soil or
// hydrology data. See docs/modules/content/terrain.md and docs/development/editor-workspace.md#s4-scene-authoring.

#include "internal/world_terrain_surface.h"

#include <ludus/content/terrain/terrain_materials.h>
#include <ludus/content/terrain/terrain_noise.h>
#include <ludus/content/terrain/terrain_rivers.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace math = foundation::math;
namespace terrain = content::terrain;

/// Editor river profile: depth and half-width at one square metre of contributing area, in metres.
constexpr float32 RIVER_DEPTH_COEFFICIENT_METRES = 0.04F;
constexpr float32 RIVER_WIDTH_COEFFICIENT_METRES = 0.05F;

/// Baked texels along each grid cell, and the bounds of the baked extent on either axis.
constexpr int32 TEXELS_PER_CELL = 4;
constexpr int32 MIN_SURFACE_TEXELS = 16;
constexpr int32 MAX_SURFACE_TEXELS = 256;
/// Wavelength and strength of the coherent brightness variation added to every texel.
constexpr float64 VARIATION_WAVELENGTH_METRES = 3;
constexpr float32 VARIATION_STRENGTH = 0.12F;
/// Water depth over which a submerged sample blends fully to riverbed gravel.
constexpr float32 RIVERBED_DEPTH_METRES = 0.25F;
/// Width of the snow line's transition, in metres.
constexpr float32 SNOW_FEATHER_METRES = 1;
/// Largest byte value of an 8-bit channel.
constexpr float32 BYTE_MAXIMUM = 255;
constexpr uint32 BYTES_PER_TEXEL = 4;
/// Byte offsets of the RGBA8 channels within a texel.
constexpr uint32 RED_CHANNEL = 0;
constexpr uint32 GREEN_CHANNEL = 1;
constexpr uint32 BLUE_CHANNEL = 2;
constexpr uint32 ALPHA_CHANNEL = 3;

/// Stable material identities of the editor's terrain palette.
constexpr uint16 GRASS = 1;
constexpr uint16 DIRT = 2;
constexpr uint16 ROCK = 3;
constexpr uint16 MUD = 4;
constexpr uint16 SNOW = 5;

/// sRGB base colours of the palette, in [0, 1].
constexpr math::Vector3 GRASS_SRGB{0.33F, 0.42F, 0.18F};
constexpr math::Vector3 DIRT_SRGB{0.45F, 0.36F, 0.26F};
constexpr math::Vector3 ROCK_SRGB{0.48F, 0.47F, 0.45F};
constexpr math::Vector3 MUD_SRGB{0.27F, 0.21F, 0.15F};
constexpr math::Vector3 SNOW_SRGB{0.93F, 0.94F, 0.96F};
constexpr math::Vector3 RIVERBED_SRGB{0.36F, 0.34F, 0.30F};

/// Slope (rise over run) and wetness thresholds of the rules, with the widths of their transitions.
constexpr float32 GRASS_MAXIMUM_SLOPE = 0.55F;
constexpr float32 DIRT_MINIMUM_SLOPE = 0.35F;
constexpr float32 DIRT_MAXIMUM_SLOPE = 0.9F;
constexpr float32 ROCK_MINIMUM_SLOPE = 0.8F;
constexpr float32 SNOW_MAXIMUM_SLOPE = 1;
constexpr float32 MUD_MAXIMUM_SLOPE = 0.5F;
constexpr float32 SLOPE_FEATHER = 0.25F;
constexpr float32 GRASS_MAXIMUM_WETNESS = 0.8F;
constexpr float32 MUD_MINIMUM_WETNESS = 0.65F;
constexpr float32 WETNESS_FEATHER = 0.15F;
/// Relative rule strengths: mud and snow override the soils they cover.
constexpr float32 DIRT_WEIGHT = 0.6F;
constexpr float32 MUD_WEIGHT = 1.5F;
constexpr float32 SNOW_WEIGHT = 4;

/// Two triangles over a cell's corners (x, z), (x + 1, z), (x, z + 1), (x + 1, z + 1), wound like the terrain mesh.
constexpr usize WATER_CELL_INDEX_COUNT = 6;
constexpr StaticArray<uint32, WATER_CELL_INDEX_COUNT> WATER_CELL_INDICES = {0, 2, 1, 1, 2, 3};

constexpr usize RULE_COUNT = 5;
using RuleSet = StaticArray<terrain::TerrainMaterialRule, RULE_COUNT>;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the arguments follow TerrainMaterialRange's member order.
[[nodiscard]] terrain::TerrainMaterialRange MakeRange(float32 minimum, float32 maximum, float32 featherWidth)
{
    terrain::TerrainMaterialRange range;
    range.Minimum = minimum;
    range.Maximum = maximum;
    range.FeatherWidth = featherWidth;
    return range;
}

[[nodiscard]] terrain::TerrainMaterialRange Below(float32 maximum, float32 featherWidth)
{
    return MakeRange(-terrain::TERRAIN_MATERIAL_RANGE_UNBOUNDED, maximum, featherWidth);
}

[[nodiscard]] terrain::TerrainMaterialRange Above(float32 minimum, float32 featherWidth)
{
    return MakeRange(minimum, terrain::TERRAIN_MATERIAL_RANGE_UNBOUNDED, featherWidth);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the arguments follow TerrainMaterialRule's member order.
[[nodiscard]] terrain::TerrainMaterialRule MakeRule(uint16 materialId,
                                                    float32 weight,
                                                    terrain::TerrainMaterialRange elevationMetres,
                                                    terrain::TerrainMaterialRange slope,
                                                    terrain::TerrainMaterialRange wetness)
{
    terrain::TerrainMaterialRule rule;
    rule.MaterialId = materialId;
    rule.Weight = weight;
    rule.ElevationMetres = elevationMetres;
    rule.Slope = slope;
    rule.Wetness = wetness;
    return rule;
}

[[nodiscard]] RuleSet MakeRules(float32 snowLineMetres)
{
    const terrain::TerrainMaterialRange any;
    const terrain::TerrainMaterialRange belowSnow = Below(snowLineMetres, SNOW_FEATHER_METRES);
    return {
        MakeRule(GRASS,
                 1,
                 belowSnow,
                 Below(GRASS_MAXIMUM_SLOPE, SLOPE_FEATHER),
                 Below(GRASS_MAXIMUM_WETNESS, WETNESS_FEATHER)),
        MakeRule(DIRT, DIRT_WEIGHT, belowSnow, MakeRange(DIRT_MINIMUM_SLOPE, DIRT_MAXIMUM_SLOPE, SLOPE_FEATHER), any),
        MakeRule(ROCK, 1, any, Above(ROCK_MINIMUM_SLOPE, SLOPE_FEATHER), any),
        MakeRule(MUD,
                 MUD_WEIGHT,
                 belowSnow,
                 Below(MUD_MAXIMUM_SLOPE, SLOPE_FEATHER),
                 Above(MUD_MINIMUM_WETNESS, WETNESS_FEATHER)),
        MakeRule(SNOW,
                 SNOW_WEIGHT,
                 Above(snowLineMetres, SNOW_FEATHER_METRES),
                 Below(SNOW_MAXIMUM_SLOPE, SLOPE_FEATHER),
                 any),
    };
}

[[nodiscard]] math::Vector3 DecodeSrgb(math::Vector3 colour)
{
    return {math::DecodeSrgb(colour.X), math::DecodeSrgb(colour.Y), math::DecodeSrgb(colour.Z)};
}

[[nodiscard]] math::Vector3 GetMaterialLinear(uint16 materialId)
{
    switch (materialId)
    {
        case GRASS:
            return DecodeSrgb(GRASS_SRGB);
        case DIRT:
            return DecodeSrgb(DIRT_SRGB);
        case ROCK:
            return DecodeSrgb(ROCK_SRGB);
        case MUD:
            return DecodeSrgb(MUD_SRGB);
        default:
            break;
    }
    return DecodeSrgb(SNOW_SRGB);
}

[[nodiscard]] uint8 EncodeByte(float32 linear)
{
    const float32 encoded = math::EncodeSrgb(math::Clamp(linear, 0.0F, 1.0F));
    return static_cast<uint8>(math::Floor(static_cast<float64>(encoded * BYTE_MAXIMUM) + 0.5));
}

/// Drainage of one surface; every view borrows these lists.
struct Drainage final
{
    QList<float32> Filled;
    QList<terrain::TerrainFlowDirection> Direction;
    QList<uint32> Accumulation;
};

[[nodiscard]] bool
TryDrain(const terrain::TerrainFieldGrid& grid, const QList<float32>& elevation, Drainage& outDrainage)
{
    const usize samples = terrain::GetTerrainSampleCount(grid);
    Drainage drainage;
    drainage.Filled.resize(static_cast<qsizetype>(samples));
    drainage.Direction.resize(static_cast<qsizetype>(samples));
    drainage.Accumulation.resize(static_cast<qsizetype>(samples));
    terrain::TerrainDrainageSummary summary;
    if (terrain::TryComputeTerrainDrainage(grid,
                                           {elevation.constData(), samples},
                                           {drainage.Filled.data(), samples},
                                           {drainage.Direction.data(), samples},
                                           {drainage.Accumulation.data(), samples},
                                           summary) != terrain::TerrainFieldStatus::Success)
    {
        return false;
    }
    outDrainage = Move(drainage);
    return true;
}

[[nodiscard]] QList<float32> GetHeights(const QList<math::Vector3>& positionsMesh)
{
    QList<float32> heights;
    heights.reserve(positionsMesh.size());
    for (const math::Vector3 positionMesh : positionsMesh)
    {
        heights.append(positionMesh.Y);
    }
    return heights;
}

[[nodiscard]] math::Vector3
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): columns then rows, and X then Z, the grid order.
SampleBilinear(const QList<math::Vector3>& values, uint32 columns, uint32 rows, float32 gridX, float32 gridZ)
{
    const float32 x = math::Clamp(gridX, 0.0F, static_cast<float32>(columns - 1));
    const float32 z = math::Clamp(gridZ, 0.0F, static_cast<float32>(rows - 1));
    const uint32 x0 = math::Min(static_cast<uint32>(x), columns - 2);
    const uint32 z0 = math::Min(static_cast<uint32>(z), rows - 2);
    const float32 tx = x - static_cast<float32>(x0);
    const float32 tz = z - static_cast<float32>(z0);
    const math::Vector3 near = values[z0 * columns + x0] * (1 - tx) + values[z0 * columns + x0 + 1] * tx;
    const math::Vector3 far = values[(z0 + 1) * columns + x0] * (1 - tx) + values[(z0 + 1) * columns + x0 + 1] * tx;
    return near * (1 - tz) + far * tz;
}

[[nodiscard]] QString FormatFieldFailure()
{
    return QStringLiteral("Terrain fields could not be computed. Reduce Samples per side, or set Surface to Plain "
                          "and turn off Carve rivers.");
}
} // namespace

bool CarveTerrainRivers(const terrain::TerrainFieldGrid& grid,
                        float32 sourceShare,
                        QList<math::Vector3>& inoutPositionsMesh,
                        TerrainWaterLevels& outWater,
                        QString& outError)
{
    const usize samples = terrain::GetTerrainSampleCount(grid);
    LUDUS_REQUIRE(static_cast<usize>(inoutPositionsMesh.size()) == samples);
    const QList<float32> elevation = GetHeights(inoutPositionsMesh);
    Drainage drainage;
    if (TryDrain(grid, elevation, drainage) == false)
    {
        outError = FormatFieldFailure();
        return false;
    }
    const float32 areaSquareMetres = static_cast<float32>(grid.Columns - 1) * grid.SpacingXMetres *
                                     static_cast<float32>(grid.Rows - 1) * grid.SpacingZMetres;
    terrain::TerrainRiverParameters parameters;
    parameters.MinimumCatchmentSquareMetres =
        math::Max(sourceShare * areaSquareMetres, grid.SpacingXMetres * grid.SpacingZMetres);
    parameters.DepthCoefficientMetres = RIVER_DEPTH_COEFFICIENT_METRES;
    parameters.WidthCoefficientMetres = RIVER_WIDTH_COEFFICIENT_METRES;
    QList<float32> carved(static_cast<qsizetype>(samples));
    QList<float32> surface(static_cast<qsizetype>(samples));
    QList<float32> depth(static_cast<qsizetype>(samples));
    terrain::TerrainRiverCarveSummary summary;
    if (terrain::TryCarveTerrainRivers(grid,
                                       {elevation.constData(), samples},
                                       {drainage.Filled.constData(), samples},
                                       {drainage.Accumulation.constData(), samples},
                                       parameters,
                                       {carved.data(), samples},
                                       {surface.data(), samples},
                                       {depth.data(), samples},
                                       summary) != terrain::TerrainRiverStatus::Success)
    {
        outError = QStringLiteral("Rivers could not be carved. Raise River sources or turn off Carve rivers.");
        return false;
    }
    TerrainWaterLevels water;
    water.LevelMesh = Move(surface);
    water.IsWet.resize(static_cast<qsizetype>(samples));
    for (usize sample = 0; sample < samples; ++sample)
    {
        water.IsWet[static_cast<qsizetype>(sample)] = depth[static_cast<qsizetype>(sample)] > 0 ? 1 : 0;
    }
    for (usize sample = 0; sample < samples; ++sample)
    {
        inoutPositionsMesh[static_cast<qsizetype>(sample)].Y = carved[static_cast<qsizetype>(sample)];
    }
    outWater = Move(water);
    return true;
}

bool BakeTerrainSurface(const terrain::TerrainFieldGrid& grid,
                        const QList<math::Vector3>& positionsMesh,
                        const TerrainWaterLevels* waterOrNull,
                        float32 snowLineMetres,
                        math::RandomKey variationKey,
                        QImage& outImage,
                        math::Vector3& outMeanLinear,
                        QString& outError)
{
    const usize samples = terrain::GetTerrainSampleCount(grid);
    LUDUS_REQUIRE(static_cast<usize>(positionsMesh.size()) == samples);
    const QList<float32> elevation = GetHeights(positionsMesh);
    Drainage drainage;
    QList<float32> slope(static_cast<qsizetype>(samples));
    QList<float32> wetness(static_cast<qsizetype>(samples));
    if (TryDrain(grid, elevation, drainage) == false ||
        terrain::TryComputeTerrainSlope(grid, {elevation.constData(), samples}, {slope.data(), samples}) !=
            terrain::TerrainFieldStatus::Success ||
        terrain::TryComputeTerrainWetness(grid,
                                          {slope.constData(), samples},
                                          {drainage.Accumulation.constData(), samples},
                                          {},
                                          {wetness.data(), samples}) != terrain::TerrainFieldStatus::Success)
    {
        outError = FormatFieldFailure();
        return false;
    }
    const RuleSet rules = MakeRules(snowLineMetres);
    QList<float32> ruleWeights(static_cast<qsizetype>(RULE_COUNT * samples));
    terrain::TerrainMaterialWeightSummary weightSummary;
    const terrain::TerrainMaterialFields fields
    {
        .ElevationMetres = {elevation.constData(), samples},
        .Slope = {slope.constData(), samples},
        .Wetness = {wetness.constData(), samples},
    };
    if (terrain::TryEvaluateTerrainMaterialWeights(grid,
                                                   {rules.GetData(), RULE_COUNT},
                                                   fields,
                                                   {ruleWeights.data(), RULE_COUNT * samples},
                                                   weightSummary) != terrain::TerrainMaterialStatus::Success)
    {
        outError = FormatFieldFailure();
        return false;
    }
    const math::Vector3 riverbed = DecodeSrgb(RIVERBED_SRGB);
    QList<math::Vector3> colours(static_cast<qsizetype>(samples));
    for (usize sample = 0; sample < samples; ++sample)
    {
        math::Vector3 colour{};
        for (usize rule = 0; rule < RULE_COUNT; ++rule)
        {
            colour =
                colour + GetMaterialLinear(rules[rule].MaterialId) * ruleWeights.constData()[rule * samples + sample];
        }
        if (waterOrNull != nullptr && waterOrNull->IsWet[static_cast<qsizetype>(sample)] != 0)
        {
            const float32 depth =
                waterOrNull->LevelMesh[static_cast<qsizetype>(sample)] - elevation[static_cast<qsizetype>(sample)];
            colour = colour + (riverbed - colour) * math::SmoothStep01(depth / RIVERBED_DEPTH_METRES);
        }
        colours[static_cast<qsizetype>(sample)] = colour;
    }
    const uint32 cellsX = grid.Columns - 1;
    const uint32 cellsZ = grid.Rows - 1;
    const int32 width =
        math::Clamp(static_cast<int32>(cellsX) * TEXELS_PER_CELL, MIN_SURFACE_TEXELS, MAX_SURFACE_TEXELS);
    const int32 height =
        math::Clamp(static_cast<int32>(cellsZ) * TEXELS_PER_CELL, MIN_SURFACE_TEXELS, MAX_SURFACE_TEXELS);
    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull() == true)
    {
        outError = QStringLiteral("The terrain surface image could not be allocated. Set Surface to Plain.");
        return false;
    }
    const float64 extentX = static_cast<float64>(cellsX) * static_cast<float64>(grid.SpacingXMetres);
    const float64 extentZ = static_cast<float64>(cellsZ) * static_cast<float64>(grid.SpacingZMetres);
    math::Vector3 total{};
    for (int32 row = 0; row < height; ++row)
    {
        uint8* pixels = image.scanLine(row);
        const float32 v = (static_cast<float32>(row) + 0.5F) / static_cast<float32>(height);
        for (int32 column = 0; column < width; ++column)
        {
            const float32 u = (static_cast<float32>(column) + 0.5F) / static_cast<float32>(width);
            math::Vector3 colour = SampleBilinear(colours,
                                                  grid.Columns,
                                                  grid.Rows,
                                                  u * static_cast<float32>(cellsX),
                                                  v * static_cast<float32>(cellsZ));
            const float64 positionX = static_cast<float64>(u) * extentX;
            const float64 positionZ = static_cast<float64>(v) * extentZ;
            const float32 variation =
                static_cast<float32>(terrain::SampleTerrainNoise(variationKey,
                                                                 0,
                                                                 positionX / VARIATION_WAVELENGTH_METRES,
                                                                 positionZ / VARIATION_WAVELENGTH_METRES));
            colour = colour * (1 + VARIATION_STRENGTH * variation);
            total = total + colour;
            uint8* texel = pixels + static_cast<usize>(column) * BYTES_PER_TEXEL;
            texel[RED_CHANNEL] = EncodeByte(colour.X);
            texel[GREEN_CHANNEL] = EncodeByte(colour.Y);
            texel[BLUE_CHANNEL] = EncodeByte(colour.Z);
            texel[ALPHA_CHANNEL] = static_cast<uint8>(BYTE_MAXIMUM);
        }
    }
    outImage = Move(image);
    outMeanLinear = total * (1 / static_cast<float32>(width * height));
    return true;
}

void BuildTerrainWaterMesh(const terrain::TerrainFieldGrid& grid,
                           const QList<math::Vector3>& positionsMesh,
                           const QList<math::Vector2>& uvs,
                           const TerrainWaterLevels& water,
                           PreviewMesh& outWater)
{
    constexpr uint32 CORNERS = 4;
    const uint32 columns = grid.Columns;
    PreviewMesh mesh;
    mesh.Id = outWater.Id;
    for (uint32 z = 0; z + 1 < grid.Rows; ++z)
    {
        for (uint32 x = 0; x + 1 < columns; ++x)
        {
            const StaticArray<uint32, CORNERS> corners = {z * columns + x,
                                                          z * columns + x + 1,
                                                          (z + 1) * columns + x,
                                                          (z + 1) * columns + x + 1};
            bool isWet = false;
            float32 highestLevel = 0;
            for (const uint32 corner : corners)
            {
                const qsizetype index = static_cast<qsizetype>(corner);
                if (water.IsWet[index] != 0 && water.LevelMesh[index] > positionsMesh[index].Y)
                {
                    highestLevel =
                        isWet == true ? math::Max(highestLevel, water.LevelMesh[index]) : water.LevelMesh[index];
                    isWet = true;
                }
            }
            if (isWet == false)
            {
                continue;
            }
            const uint32 first = static_cast<uint32>(mesh.Positions.size());
            for (const uint32 corner : corners)
            {
                const qsizetype index = static_cast<qsizetype>(corner);
                const bool isCornerWet = water.IsWet[index] != 0 && water.LevelMesh[index] > positionsMesh[index].Y;
                const math::Vector3 positionMesh = positionsMesh[index];
                mesh.Positions.append(
                    {positionMesh.X, isCornerWet == true ? water.LevelMesh[index] : highestLevel, positionMesh.Z});
                mesh.Uvs.append(uvs[index]);
                mesh.Normals.append({0, 1, 0});
            }
            for (const uint32 offset : WATER_CELL_INDICES)
            {
                mesh.Indices.append(first + offset);
            }
        }
    }
    outWater = Move(mesh);
}
} // namespace ludus::editor
