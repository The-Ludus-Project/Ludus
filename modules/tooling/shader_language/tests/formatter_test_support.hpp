#pragma once

#include <ludus/tooling/shader_language/formatter.hpp>

#include "internal/lexer.hpp"

#include <string>
#include <string_view>

namespace ludus::tooling::shader_language::test
{
/// Format `text`; returns the text on Ready and an empty string otherwise.
inline std::string FormatText(std::string_view text, Status& outStatus)
{
    FormatResult result;
    outStatus = FormatSource(text, result);
    return std::string(result.Text.GetData(), result.Text.GetSize());
}

/// Token and comment spellings of `text` joined by line breaks, or empty when it does not lex.
inline std::string SpellTokens(std::string_view text)
{
    foundation::Array<Token> tokens;
    foundation::Array<Diagnostic> diagnostics;
    foundation::usize droppedCount = 0;
    internal::DiagnosticSink sink(diagnostics, droppedCount);
    internal::LexInput input;
    input.Text = text;
    input.FileIndex = 0;
    input.ShouldKeepComments = true;
    if (internal::LexText(input, tokens, sink) != internal::LexOutcome::Complete)
    {
        return {};
    }
    std::string spelled;
    for (const Token& token : tokens)
    {
        spelled += text.substr(token.SpellingOffset, token.SpellingSize);
        spelled += '\n';
    }
    return spelled;
}
} // namespace ludus::tooling::shader_language::test
