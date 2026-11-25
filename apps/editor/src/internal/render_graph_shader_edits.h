#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>

namespace ludus::editor
{
/// Return a quoted-include path supported by the target language, or empty if the files cannot be linked.
[[nodiscard]] QString GetSharedShaderIncludePath(const QString& target, const QString& header);

/// Add a direct include, preserving existing text and newline style. Returns the input if already included.
[[nodiscard]] QByteArray InsertShaderInclude(const QByteArray& source, const QString& path);

/// Read candidate scene Buffer callbacks for the source from the draft's program table.
[[nodiscard]] QJsonArray FindShaderBufferCallbacks(const QByteArray& manifest, const QString& source);

/// Update one conventional scene callback. Empty output and outError indicate a signature requiring manual edits.
[[nodiscard]] QByteArray
AddShaderBufferParameter(const QByteArray& source, const QString& function, bool isLusl, QString& outError);
} // namespace ludus::editor
