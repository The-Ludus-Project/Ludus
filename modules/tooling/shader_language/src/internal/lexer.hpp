#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/token.hpp>

#include "diagnostic_sink.hpp"

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Text to lex and where its tokens are spelled and located.
struct LexInput final
{
    /// Bytes to lex; already validated as UTF-8 without zero bytes.
    std::string_view Text;
    /// SourceLocation::FileIndex for the produced tokens.
    foundation::uint16 FileIndex = NO_SOURCE;
    /// Line number of the first byte; columns of that line start at one.
    foundation::uint32 FirstLine = 1;
    /// True when `Text` is a region of the result's synthesized text.
    bool IsSynthesized = false;
    /// Offset of `Text` in its storage, added to every SpellingOffset.
    foundation::uint32 SpellingBase = 0;
    /// Offset of `Text` from the start of the located source, added to every ByteOffset.
    foundation::uint32 LocationBase = 0;
};

/// Result of LexText.
enum class LexOutcome : foundation::uint8
{
    /// Every byte was lexed; tokens were appended.
    Complete,
    /// Lexical errors were reported; tokens for the valid parts were appended.
    Failed,
    /// Appending a token failed for lack of memory.
    OutOfMemory,
    /// More than MAX_FILE_TOKENS tokens; a diagnostic was reported.
    TokenLimit,
};

/// @brief Append the tokens of `input.Text` to `inoutTokens`, dropping whitespace and comments.
///
/// A backslash immediately before a line break joins the lines. Reports lexical errors to
/// `inoutSink` and keeps lexing after them.
[[nodiscard]] LexOutcome
LexText(const LexInput& input, foundation::Array<Token>& inoutTokens, DiagnosticSink& inoutSink) noexcept;

/// @brief Return the location of byte `byteOffset` of `text`, counting lines from one.
[[nodiscard]] SourceLocation
LocateByte(foundation::uint16 fileIndex, std::string_view text, foundation::uint32 byteOffset) noexcept;
} // namespace ludus::tooling::shader_language::internal
