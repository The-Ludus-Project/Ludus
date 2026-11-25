#pragma once

#include <ludus/foundation/base/types.h>

#include <QList>
#include <QString>

namespace ludus::editor
{
/// One tab stop or placeholder of an expanded snippet: UTF-16 offsets
/// [Start, End) in CodeSnippet::Text. Index 0 is the final caret position.
struct CodeSnippetStop final
{
    foundation::int32 Index = 0;
    foundation::int32 Start = 0;
    foundation::int32 End = 0;
    [[nodiscard]] bool operator==(const CodeSnippetStop& other) const = default;
};

/// An LSP snippet expanded to the text it inserts and its stops in parse order
/// (an inner placeholder precedes the one that contains it).
struct CodeSnippet final
{
    QString Text;
    QList<CodeSnippetStop> Stops;
};

/// Stops kept per snippet; a snippet with more is rejected.
inline constexpr foundation::int32 CODE_MAX_SNIPPET_STOPS = 128;
/// Placeholder nesting kept per snippet; a deeper snippet is rejected.
inline constexpr foundation::int32 CODE_MAX_SNIPPET_DEPTH = 16;

/// Expand an LSP 3.17 snippet (`InsertTextFormat.Snippet`): `$1`, `${1}`,
/// `${1:placeholder}` (nested), `${1|one,two|}` (inserts the first choice) and
/// `$0`, with `\$`, `\}` and `\\` escapes. A bare `$N` after `${N:text}` repeats
/// that text (a mirror before its placeholder starts empty). Variables insert their default text,
/// or nothing for the `TM_` editor variables and their own name otherwise. A
/// construct that does not parse is literal text, as in VS Code. Returns false,
/// preserving `outSnippet`, for a transform (`${1/a/b/}`), which is not
/// supported, or beyond CODE_MAX_SNIPPET_STOPS or CODE_MAX_SNIPPET_DEPTH.
[[nodiscard]] bool ParseCodeSnippet(const QString& source, CodeSnippet& outSnippet);
} // namespace ludus::editor
