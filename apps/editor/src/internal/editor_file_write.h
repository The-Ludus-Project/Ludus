#pragma once

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
/// Atomically publish a bounded editor buffer. On failure the accepted file
/// remains in place and outError explains the failed operation.
[[nodiscard]] bool WriteEditorFileAtomically(const QString& path, const QByteArray& bytes, QString& outError);
} // namespace ludus::editor
