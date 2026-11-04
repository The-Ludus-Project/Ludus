#pragma once

#include "internal/code_text_profile.h"

#include <ludus/foundation/base/types.h>

#include <QString>

namespace ludus::editor
{
/// The subset of EditorConfig the file document host applies. A field is set
/// only when a matching section declared it; unset fields leave the detected
/// value in place. Qt-free values; QString is only the file path carrier.
struct CodeEditorConfigSettings final
{
    bool HasIndentStyle = false;
    CodeIndentStyle IndentStyle = CodeIndentStyle::Spaces;
    bool HasIndentSize = false;
    foundation::uint32 IndentSize = CODE_INDENT_WIDTH_DEFAULT;
    bool HasTabWidth = false;
    foundation::uint32 TabWidth = CODE_INDENT_WIDTH_DEFAULT;
    bool HasLineEnding = false;
    CodeLineEnding LineEnding = CodeLineEnding::Lf;
    bool HasCharset = false;
    /// True only for a charset of "utf-8" or "utf-8-bom"; any other charset is
    /// recorded as unsupported so the host keeps the file read-only rather than
    /// claiming an encoding it cannot edit.
    bool IsCharsetUtf8 = false;
    bool IsBomExpected = false;
    bool HasInsertFinalNewline = false;
    bool ShouldInsertFinalNewline = false;
    bool HasTrimTrailingWhitespace = false;
    bool ShouldTrimTrailingWhitespace = false;
};

/// Parse one EditorConfig file's bytes and merge every section that matches
/// `relativePath` into `inoutSettings`, in file order (later sections win).
///
/// @param configBytes The raw bytes of a single .editorconfig file.
/// @param configByteCount Number of bytes in configBytes; may be zero.
/// @param relativePath The target file's path relative to this config's
///   directory, using '/' separators. Section globs are matched against it.
/// @param inoutSettings Accumulates matched declarations; preserved on any
///   malformed line, which is skipped rather than failing the whole file.
/// @return True when this file carried `root = true`, so the caller stops
///   walking to parent directories; false otherwise. Never throws.
[[nodiscard]] bool MergeEditorConfigFile(const char* configBytes,
                                         foundation::usize configByteCount,
                                         const QString& relativePath,
                                         CodeEditorConfigSettings& inoutSettings) noexcept;

/// Resolve the effective EditorConfig settings for `absoluteFilePath` by walking
/// from its directory up to the filesystem root, reading each `.editorconfig`,
/// and stopping at the first `root = true`. Missing files are skipped. Reads are
/// bounded and performed on the calling (background) thread; never on the UI
/// thread during typing. Returns the merged settings; an empty result when no
/// config applies leaves every field unset.
[[nodiscard]] CodeEditorConfigSettings ResolveEditorConfig(const QString& absoluteFilePath) noexcept;

/// Apply resolved EditorConfig settings over a detected text profile, overriding
/// indentation and line endings where the config declared them (CE04). Charset,
/// final-newline and trailing-whitespace handling are carried for the save path.
void ApplyEditorConfig(const CodeEditorConfigSettings& settings, CodeTextProfile& inoutProfile) noexcept;
} // namespace ludus::editor
