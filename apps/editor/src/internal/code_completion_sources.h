#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include "internal/code_editor.h"

#include <QLatin1StringView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace ludus::editor
{
/// Where a completion suggestion comes from (code editor M4, CE10). Equal scores
/// list server items first, then snippets, then words.
enum class CodeCompletionSource : foundation::uint8
{
    Server,
    Snippet,
    Word,
};

/// A built-in snippet: the prefix that completes to it, what it inserts, and an
/// LSP 3.17 snippet body with four-space indentation and `\n` line breaks.
struct CodeSnippetTemplate final
{
    QLatin1StringView Prefix;
    QLatin1StringView Description;
    QLatin1StringView Body;
};

/// Words shorter than this are not suggested.
inline constexpr foundation::int32 CODE_MIN_COMPLETION_WORD_UNITS = 3;
/// Words longer than this are not suggested.
inline constexpr foundation::int32 CODE_MAX_COMPLETION_WORD_UNITS = 128;
/// Distinct words suggested per request, in the order they are found.
inline constexpr foundation::int32 CODE_MAX_COMPLETION_WORDS = 4096;
/// UTF-16 units read across all open buffers per request; larger buffers are skipped.
inline constexpr foundation::int32 CODE_MAX_COMPLETION_WORD_SCAN_UNITS = 1 << 20;

/// The built-in snippets for `language`; empty when it has none. Valid for the
/// process lifetime.
[[nodiscard]] foundation::BorrowedView<const CodeSnippetTemplate>
GetCodeSnippetTemplates(CodeLanguage language) noexcept;

/// `body` with every line after the first indented by `indentation`, so a
/// multi-line snippet lines up with the line it is inserted on.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the text, then what each new line starts with.
[[nodiscard]] QString IndentCodeSnippetBody(QStringView body, QStringView indentation);

/// The distinct identifiers in `texts` that fuzzily match `prefix`, other than
/// `prefix` itself, in the order they first appear: at least
/// CODE_MIN_COMPLETION_WORD_UNITS and at most CODE_MAX_COMPLETION_WORD_UNITS long,
/// at most CODE_MAX_COMPLETION_WORDS of them. A text that would take the reading
/// past CODE_MAX_COMPLETION_WORD_SCAN_UNITS is skipped.
[[nodiscard]] QStringList CollectCodeCompletionWords(const QList<QString>& texts, const QString& prefix);
} // namespace ludus::editor
