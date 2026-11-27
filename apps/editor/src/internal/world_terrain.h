#pragma once

#include "internal/render_scene.h"

#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
class WorldDocument;

/// The default World's built-in terrain component identifier.
[[nodiscard]] QString GetTerrainComponentId();

/// Recipe field that stores the content::terrain::TerrainNoiseLayout version of generated elevation and surface
/// variation. New terrain records TERRAIN_NOISE_LAYOUT_VERSION; a recipe that omits the field, as every World saved
/// before it existed does, keeps layout 1 (the cubic fade) and its exact heights.
[[nodiscard]] QString GetTerrainNoiseLayoutKey();

/// Record the noise layout new terrain generates with in a recipe that has no saved layout yet.
void SetLatestTerrainNoiseLayout(QJsonObject& inoutRecipe);

/// Optional built-in payloads validated separately from editable scalar properties.
[[nodiscard]] qsizetype CountTerrainPayloadFields(const QJsonObject& recipe);

/// Read one bounded 8- or 16-bit grayscale PNG into the World-owned normalized sample payload.
[[nodiscard]] bool ImportTerrainHeightmap(const QString& path, QJsonObject& outHeightmap, QString& outError);

/// Upgrade or reduce the recipe version to match its source and stroke payloads.
void SetTerrainRecipeVersion(QJsonObject& inoutRecipe);

/// Brush positions are local XZ pairs. The limits bound interactive preview and persisted stroke work.
inline constexpr qsizetype MAX_TERRAIN_STROKE_POINTS = 64;
inline constexpr foundation::float64 MIN_TERRAIN_BRUSH_RADIUS_METRES = 0.1;
inline constexpr foundation::float64 MAX_TERRAIN_BRUSH_RADIUS_METRES = 32;
inline constexpr foundation::float64 MAX_TERRAIN_BRUSH_HEIGHT_METRES = 128;

/// Remove strokes whose support would be cut by a smaller footprint. Returns the count for commit confirmation.
[[nodiscard]] qsizetype RemoveClippedTerrainStrokes(const QJsonObject& previous, QJsonObject& inoutRecipe);

enum class TerrainBrush : foundation::uint8
{
    Off,
    Flatten,
    RaiseLower,
    Smooth,
};

/// Append an ordered stroke. Value is local metres for Flatten and Raise/Lower, or a blend weight for Smooth.
[[nodiscard]] bool AppendTerrainStroke(QJsonObject& inoutRecipe,
                                       const QJsonArray& points,
                                       foundation::float32 radius,
                                       TerrainBrush brush,
                                       foundation::float32 value,
                                       QString& outError);

/// Check the relationships between the schema-validated recipe fields.
[[nodiscard]] bool ValidateTerrainRecipe(const QJsonObject& recipe, QString& outError);

/// Whether a terrain property was added after the first built-in schema, so older Worlds may omit it and
/// readers use its schema default.
[[nodiscard]] bool IsTerrainSurfaceProperty(const QString& property);

/// Object identifier of a terrain's generated water: the entity key plus a suffix entity keys cannot contain.
[[nodiscard]] QString GetTerrainWaterObjectId(const QString& entityKey);

/// Whether a preview object is generated terrain water. Picking, sculpting and colliders skip it.
[[nodiscard]] bool IsTerrainWaterObjectId(const QString& objectId);

/// Roughness of a generated terrain surface.
inline constexpr foundation::float32 TERRAIN_SURFACE_ROUGHNESS = 0.9F;
/// Roughness of generated terrain water.
inline constexpr foundation::float32 TERRAIN_WATER_ROUGHNESS = 0.05F;
/// Linear reflectance of generated terrain water.
inline constexpr foundation::math::Vector4 TERRAIN_WATER_TINT{0.02F, 0.06F, 0.08F, 1};

/// A terrain's geometry and generated presentation.
struct TerrainBuild final
{
    /// The carved and sculpted terrain mesh.
    PreviewMesh Mesh;
    /// sRGB base colour baked from the surface material rules over the mesh UVs; null for a Plain surface.
    QImage BaseColor;
    /// Linear mean of BaseColor's RGB, for consumers that sample no texture.
    foundation::math::Vector3 BaseColorMean{1, 1, 1};
    /// Water over carved channels, in the terrain's local space; empty without rivers or water.
    PreviewMesh Water;
    /// Sample grid of Mesh's positions, which are row-major with columns along +X.
    content::terrain::TerrainFieldGrid Grid{};
};

/// Generate the terrain mesh, its baked surface and its river water from one versioned recipe.
///
/// Rivers are carved into the generated or imported base before sculpt strokes, as the generation pipeline orders
/// them; slope, drainage, wetness and the surface materials are derived from the final surface. Failure preserves
/// outBuild.
[[nodiscard]] bool BuildTerrain(const QJsonObject& recipe, TerrainBuild& outBuild, QString& outError);

/// Generate an indexed, Y-up mesh from one versioned recipe, without its surface or water. Failure preserves
/// outMesh.
[[nodiscard]] bool BuildTerrainMesh(const QJsonObject& recipe, PreviewMesh& outMesh, QString& outError);

/// Generate the mesh like BuildTerrainMesh and report the sample grid of its positions. Failure preserves both
/// outputs.
[[nodiscard]] bool BuildTerrainMesh(const QJsonObject& recipe,
                                    PreviewMesh& outMesh,
                                    content::terrain::TerrainFieldGrid& outGrid,
                                    QString& outError);

/// Create or update terrain as one edit. An empty key uses the draft transform, or the defaults when it is empty.
[[nodiscard]] bool EditWorldTerrain(WorldDocument& inoutWorld,
                                    const foundation::identity::EntropySource& entropy,
                                    const QString& key,
                                    const QJsonObject& recipe,
                                    QString& outKey,
                                    QString& outError,
                                    const QJsonObject& draftTransform = {});
} // namespace ludus::editor
