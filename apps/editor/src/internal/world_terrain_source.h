#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/vector.hpp>

#include "world_terrain_brush.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ludus::editor
{
inline constexpr foundation::int32 TERRAIN_HEIGHTMAP_MAX_SIDE = 2048;
inline constexpr foundation::int64 TERRAIN_HEIGHTMAP_MAX_FILE_BYTES = foundation::int64{12} * 1024 * 1024;

struct TerrainHeightmap final
{
    QImage Image;
    QString Sha256;

    /// Sample a decoded grayscale raster at normalized local X/Z, including both endpoints.
    [[nodiscard]] foundation::float64 Sample(foundation::float64 u, foundation::float64 v) const noexcept;
};

/// Inspect an existing project-relative 8- or 16-bit grayscale PNG within the heightmap budgets.
/// expectedDigest is empty when choosing a source. Failure preserves outHeightmap.
[[nodiscard]] bool LoadTerrainHeightmap(const QString& root,
                                        const QString& relative,
                                        const QString& expectedDigest,
                                        TerrainHeightmap& outHeightmap,
                                        QString& outError);

/// Strip the built-in optional source metadata before validating ordinary schema properties.
[[nodiscard]] QJsonObject GetTerrainSchemaFields(QJsonObject recipe);

/// Validate the optional heightmap descriptor and bounded ordered stroke recipe without reading a file.
[[nodiscard]] bool ValidateTerrainSource(const QJsonObject& recipe, QString& outError);

/// Append a stroke using local X/Z points in metres, preserving the input recipe on failure.
[[nodiscard]] bool
AppendTerrainStroke(const TerrainBrush& brush, const QJsonArray& points, QJsonObject& inoutRecipe, QString& outError);

/// Compose the operations into scratch positions before rebuilding normals. Discard scratch on failure.
[[nodiscard]] bool ApplyTerrainStrokes(const QJsonObject& recipe,
                                       foundation::uint32 samples,
                                       QVector<foundation::math::Vector3>& inoutPositionsMesh,
                                       QString& outError);

/// Return the strokes whose previous terrain coverage would be clipped by the new footprint.
[[nodiscard]] QJsonArray FindClippedTerrainStrokes(const QJsonObject& previous, const QJsonObject& replacement);
/// Remove the named stroke indices from a draft. The committed World still owns the original.
void RemoveTerrainStrokes(const QJsonArray& indices, QJsonObject& inoutRecipe);
} // namespace ludus::editor
