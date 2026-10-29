#pragma once

#include <ludus/foundation/identity/uuid.hpp>

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
class WorldDocument;
/// True only for the editor-owned default schema; project schemas keep their own contracts.
[[nodiscard]] bool IsBuiltInWorld(const WorldDocument& world);
/// Read a bundled default-World asset. Failure preserves outBytes.
[[nodiscard]] bool ReadBuiltInWorldAsset(const QString& name, QByteArray& outBytes, QString& outError);
/// Create one bundled geometry/light preset as one validated history transaction.
/// Failure preserves the document and outId. Must run on the editor thread.
[[nodiscard]] bool CreateWorldObject(WorldDocument& inoutWorld,
                                     const foundation::identity::EntropySource& entropy,
                                     const QString& preset,
                                     QString& outId,
                                     QString& outError);
} // namespace ludus::editor
