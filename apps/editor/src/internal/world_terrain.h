#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/identity/uuid.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
struct PreviewMesh;
class WorldDocument;

/// The default World's built-in terrain component identifier.
[[nodiscard]] QString GetTerrainComponentId();

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

/// Append one validated, ordered flatten stroke. Failure preserves inoutRecipe.
[[nodiscard]] bool AppendTerrainFlattenStroke(QJsonObject& inoutRecipe,
                                              const QJsonArray& points,
                                              foundation::float32 radius,
                                              foundation::float32 height,
                                              QString& outError);

/// Check the relationships between the schema-validated recipe fields.
[[nodiscard]] bool ValidateTerrainRecipe(const QJsonObject& recipe, QString& outError);

/// Generate an indexed, Y-up mesh from one versioned recipe. Failure preserves outMesh.
[[nodiscard]] bool BuildTerrainMesh(const QJsonObject& recipe, PreviewMesh& outMesh, QString& outError);

/// Create or update terrain as one edit. An empty key uses the draft transform, or the defaults when it is empty.
[[nodiscard]] bool EditWorldTerrain(WorldDocument& inoutWorld,
                                    const foundation::identity::EntropySource& entropy,
                                    const QString& key,
                                    const QJsonObject& recipe,
                                    QString& outKey,
                                    QString& outError,
                                    const QJsonObject& draftTransform = {});
} // namespace ludus::editor
