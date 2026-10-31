#pragma once

#include "internal/code_navigation.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QMap>

namespace ludus::editor
{
/// Trusted captured context source; File is empty for generated/rewritten read-only text.
struct CodeNavigationSource final
{
    QString File;
    QString Text;
};
/// Exact URI inventory captured by the compiler context provider; no arbitrary URI reads.
using CodeNavigationSources = QMap<QString, CodeNavigationSource>;

/// Parse bounded LSP navigation results without disk access or editor mutation.
/// Unknown URIs, invalid ranges and generated symbols outside the editable span are
/// excluded from Go to Symbol. Definitions/references can inspect generated text read-only.
[[nodiscard]] QList<CodeNavigationTarget> ReadCodeNavigationResults(CodeNavigationKind kind,
                                                                    const QJsonValue& value,
                                                                    const QString& uri,
                                                                    const QString& unit,
                                                                    const QString& prefix,
                                                                    const QString& source,
                                                                    const QString& file,
                                                                    const CodeNavigationSources& sources,
                                                                    QString& outMessage);
/// Strict zero-based UTF-16 protocol position, preserving outOffset on failure.
[[nodiscard]] bool
GetCodeProtocolOffset(const QString& text, const QJsonObject& position, foundation::int32& outOffset);
} // namespace ludus::editor
