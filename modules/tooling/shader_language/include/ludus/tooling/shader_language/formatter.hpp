#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/status.hpp>

#include <string_view>

/// @file
/// The canonical LUSL source formatter; docs/modules/tooling/shader-language.md describes its rules.
namespace ludus::tooling::shader_language
{
/// @brief Output of FormatSource.
struct FormatResult final
{
    /// Final outcome.
    Status Outcome = Status::InvalidRequest;
    /// The formatted source on Ready, ending in one line break unless empty; empty otherwise.
    foundation::Array<char> Text;
    /// Lexical problems that prevented formatting. Locations use file index zero.
    foundation::Array<Diagnostic> Diagnostics;
    /// Number of diagnostics found after `Diagnostics` reached MAX_DIAGNOSTICS.
    foundation::usize DroppedDiagnosticCount = 0;
};

/// @brief Format one LUSL source file in the Ludus style.
///
/// Works on the unpreprocessed text, so macros, conditional groups and comments are kept. Only
/// whitespace changes: lexing the result gives the same tokens and comments in the same order, and
/// formatting it again changes nothing. The style follows the repository's C++ formatting:
/// four-space indentation, braces on their own lines, indented `case` labels, single spaces around
/// binary operators, directives indented after `#` by conditional depth, and at most one blank
/// line. Line breaks inside statements are kept as written; long lines are not wrapped.
///
/// Deterministic and reentrant.
/// @param text UTF-8 source without zero bytes, at most `shader_compiler::MAX_SOURCE_BYTES` bytes.
/// @param outResult Receives the result; any previous contents are replaced.
/// @return The same value as `outResult.Outcome`: InvalidRequest for text outside those bounds,
/// Failed when the source does not lex (an unterminated comment or string, for example) or the
/// formatted text would exceed the byte bound.
[[nodiscard]] Status FormatSource(std::string_view text, FormatResult& outResult) noexcept;
} // namespace ludus::tooling::shader_language
