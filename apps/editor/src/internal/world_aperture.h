#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
struct PreviewMesh;
class WorldDocument;

// A default World's aperture entity is an opaque screen with a centred slit whose sizes are
// component fields, so Properties edits the opening and World extraction regenerates the
// screen. Scene source `aperture` objects import as these entities (scene-source-format.md).

/// The default World's built-in aperture component identifier.
[[nodiscard]] QString GetApertureComponentId();

/// Check the relationships between the schema-validated aperture fields: the opening narrower
/// than the screen and the slit no longer than it. outError names the field to change.
[[nodiscard]] bool ValidateApertureParameters(const QJsonObject& values, QString& outError);

/// Generate the screen in the entity's frame: the local XY plane, front face toward +Z, the
/// slit along local Y. Each quad of content::scene_source::BuildApertureQuads becomes two
/// triangles fanned from its first corner, as a scene source import cooks them. outMesh keeps
/// its Id. Failure preserves outMesh.
[[nodiscard]] bool BuildApertureMesh(const QJsonObject& values, PreviewMesh& outMesh, QString& outError);

/// Linear reflectance of the aperture's screen (r, g, b; alpha one) from validated values.
[[nodiscard]] foundation::math::Vector4 GetApertureReflectance(const QJsonObject& values);

/// @brief Default-World entities for the `aperture` objects of a scene source.
struct ImportedApertures final
{
    /// Entity records ({id, name, components: {transform, aperture}}) for WorldDocument::ImportScene.
    QJsonArray Entities;
    /// One line per approximation, such as a non-diffuse or emitting screen material.
    QStringList Warnings;
};

/// Read a scene source and describe its aperture objects as entities of `world`.
///
/// Each entity's transform places the aperture frame (centre, across, up, normal) and its
/// aperture component holds the source sizes and the material's base colour. The entity ID
/// is derived from levelId, entryName and the object name, so reimporting the same source into
/// the same World finds the entity it created and keeps its edits. Pure: safe on a worker thread.
/// @param sourceBytes The complete `.luscene.json` document that the import cooked.
/// @param world A default World (HasBuiltInAperture); supplies the component defaults.
/// @param levelId The World scene level's `id`.
/// @param entryName The source's file name.
/// @param outApertures Replaced on success; empty when the source has no apertures.
/// @param outError Why the source could not be read; failure preserves outApertures.
[[nodiscard]] bool MakeImportedApertures(const QByteArray& sourceBytes,
                                         const WorldDocument& world,
                                         const QString& levelId,
                                         const QString& entryName,
                                         ImportedApertures& outApertures,
                                         QString& outError);
} // namespace ludus::editor
