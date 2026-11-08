#pragma once

#include <ludus/foundation/identity/uuid.hpp>

#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
struct PreviewMesh;
class WorldDocument;

/// The default World's built-in terrain component identifier.
[[nodiscard]] QString GetTerrainComponentId();

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
