#pragma once

#include "internal/code_file_limits.h"

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
/// The dominant line ending of a loaded document, preserved byte for byte in
/// untouched text (CE04). A file whose endings are not uniform is Mixed; the
/// host keeps every original ending and only normalizes the ending it writes for
/// newly typed lines to the dominant kind.
enum class CodeLineEnding : foundation::uint8
{
    /// Line feed only ("\n"); the default for a document with no line break.
    Lf,
    /// Carriage return and line feed ("\r\n").
    CrLf,
    /// A file that mixes LF and CRLF endings; both are preserved as read.
    Mixed,
};

/// The byte encoding of a loaded document. Only UTF-8 is editable; anything that
/// is not well-formed UTF-8 opens read-only with an explanation (CE04). Encodings
/// other than UTF-8 are an explicit non-goal.
enum class CodeTextEncoding : foundation::uint8
{
    /// Well-formed UTF-8 with no byte-order mark.
    Utf8,
    /// Well-formed UTF-8 preceded by the UTF-8 byte-order mark (EF BB BF).
    Utf8Bom,
    /// Not well-formed UTF-8; the document is read-only.
    InvalidUtf8,
};

/// Whether detected indentation uses spaces or tabs.
enum class CodeIndentStyle : foundation::uint8
{
    /// Indentation is spaces; the width is the detected or configured size.
    Spaces,
    /// Indentation is tabs; the width is the tab stop.
    Tabs,
};

/// How a document's indentation and line endings were decided. Detection reads
/// the loaded bytes; EditorConfig then overrides either field (CE04).
enum class CodeTextProfileSource : foundation::uint8
{
    /// The field came from scanning the loaded bytes.
    Detected,
    /// The field was overridden by a matching .editorconfig section.
    EditorConfig,
};

/// The smallest and largest indentation widths the host will detect or accept
/// from EditorConfig, so a malformed value cannot produce a degenerate width.
inline constexpr foundation::uint32 CODE_INDENT_WIDTH_MIN = 1;
inline constexpr foundation::uint32 CODE_INDENT_WIDTH_MAX = 16;
/// The default indentation width when neither detection nor EditorConfig decides.
inline constexpr foundation::uint32 CODE_INDENT_WIDTH_DEFAULT = 4;

/// Detected or configured indentation for a document.
struct CodeIndentProfile final
{
    CodeIndentStyle Style = CodeIndentStyle::Spaces;
    /// Width in columns (spaces) or the tab stop (tabs); clamped to [min, max].
    foundation::uint32 Width = CODE_INDENT_WIDTH_DEFAULT;
    CodeTextProfileSource Source = CodeTextProfileSource::Detected;
    [[nodiscard]] bool operator==(const CodeIndentProfile& other) const = default;
};

/// The full text profile of a loaded document: encoding, line endings and
/// indentation, plus the size mode that governs feature degradation (CE22).
/// Qt-free, so the background loader and the host share it without a widget.
struct CodeTextProfile final
{
    CodeTextEncoding Encoding = CodeTextEncoding::Utf8;
    CodeLineEnding LineEnding = CodeLineEnding::Lf;
    CodeTextProfileSource LineEndingSource = CodeTextProfileSource::Detected;
    CodeIndentProfile Indent;
    CodeFileSizeMode SizeMode = CodeFileSizeMode::Normal;
    /// True once a size threshold or invalid UTF-8 forced the document read-only.
    bool IsReadOnly = false;
    /// The decoded line count, used for the line-count degradation decision.
    foundation::usize LineCount = 1;
    [[nodiscard]] bool operator==(const CodeTextProfile& other) const = default;
};
} // namespace ludus::editor
