#pragma once

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
/// Describe every program using sourceFile in the draft build manifest. This is
/// authored interface metadata, not compiler reflection or graph resource ports.
/// Invalid manifests and unregistered sources produce an explicit unavailable message.
[[nodiscard]] QString DescribeShaderInterface(const QByteArray& manifest, const QString& sourceFile);
} // namespace ludus::editor
