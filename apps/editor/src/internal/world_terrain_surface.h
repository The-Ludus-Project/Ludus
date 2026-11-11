#pragma once

#include "internal/render_scene.h"

#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QImage>
#include <QList>
#include <QString>

// Generated presentation of World terrain: river carving of the base surface, the baked surface material and the
// river water mesh. The kernels are Ludus::ContentTerrain's (docs/modules/content/terrain.md); this file owns the
// editor's parameters, colours and mesh layout. World extraction calls it through BuildTerrain.

namespace ludus::editor
{
/// Water a river carve leaves on the base surface, one entry per terrain sample.
struct TerrainWaterLevels final
{
    /// Water surface elevation in local metres where IsWet is set.
    QList<foundation::float32> LevelMesh;
    /// One where the carve left water; strokes applied afterwards can still raise the ground above it.
    QList<foundation::uint8> IsWet;
};

/// Carve river channels into a terrain's base elevation.
///
/// @param grid The terrain's sample grid; positions are row-major in the same order.
/// @param sourceShare Share of the terrain's area that must drain to a sample to start a channel, 0 to 1.
/// @param inoutPositionsMesh Base positions; their heights become the carved surface on success.
/// @param outWater Water levels and wet samples; written only on success.
/// @param outError The cause and the next step on failure, which leaves both outputs unchanged.
[[nodiscard]] bool CarveTerrainRivers(const content::terrain::TerrainFieldGrid& grid,
                                      foundation::float32 sourceShare,
                                      QList<foundation::math::Vector3>& inoutPositionsMesh,
                                      TerrainWaterLevels& outWater,
                                      QString& outError);

/// Bake a terrain's sRGB base colour from slope, wetness, elevation and water over its mesh UVs.
///
/// Grass, dirt, rock, mud and snow follow the terrain material rules; samples under water blend to riverbed
/// gravel. Each texel adds a small coherent brightness variation so large areas of one material do not read flat.
/// @param grid The terrain's sample grid.
/// @param positionsMesh The final terrain positions, row-major over the grid.
/// @param waterOrNull Water from CarveTerrainRivers, or null without rivers.
/// @param snowLineMetres Local elevation above which snow settles on gentle slopes.
/// @param variationKey Random key of the surface variation domain.
/// @param outImage RGBA8 sRGB pixels whose rows run along +Z; written only on success.
/// @param outMeanLinear Linear mean of the baked RGB; written only on success.
/// @param outError The cause and the next step on failure.
[[nodiscard]] bool BakeTerrainSurface(const content::terrain::TerrainFieldGrid& grid,
                                      const QList<foundation::math::Vector3>& positionsMesh,
                                      const TerrainWaterLevels* waterOrNull,
                                      foundation::float32 snowLineMetres,
                                      foundation::math::RandomKey variationKey,
                                      QImage& outImage,
                                      foundation::math::Vector3& outMeanLinear,
                                      QString& outError);

/// Build the water surface over every grid cell with a wet corner that still lies below its water level.
///
/// Dry corners of a wet cell take the cell's highest water level, so the surface ends under the bank and the
/// terrain's depth test draws the shoreline. An empty mesh means no water.
/// @param grid The terrain's sample grid.
/// @param positionsMesh The final terrain positions, row-major over the grid.
/// @param uvs The terrain UVs, one per position.
/// @param water Water from CarveTerrainRivers.
/// @param outWater Indexed, Y-up water mesh; its Id is left unchanged.
void BuildTerrainWaterMesh(const content::terrain::TerrainFieldGrid& grid,
                           const QList<foundation::math::Vector3>& positionsMesh,
                           const QList<foundation::math::Vector2>& uvs,
                           const TerrainWaterLevels& water,
                           PreviewMesh& outWater);
} // namespace ludus::editor
