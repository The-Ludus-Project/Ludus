#include "internal/lexer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace
{
using ludus::foundation::Array;
using ludus::foundation::usize;
using ludus::tooling::shader_language::Diagnostic;
using ludus::tooling::shader_language::DiagnosticCode;
using ludus::tooling::shader_language::Punctuator;
using ludus::tooling::shader_language::Token;
using ludus::tooling::shader_language::TokenKind;
using ludus::tooling::shader_language::internal::DiagnosticSink;
using ludus::tooling::shader_language::internal::LexInput;
using ludus::tooling::shader_language::internal::LexOutcome;
using ludus::tooling::shader_language::internal::LexText;

/// Lex `text` as file zero and keep the tokens and diagnostics.
struct Lexed final
{
    std::string Text;
    Array<Token> Tokens;
    Array<Diagnostic> Diagnostics;
    usize DroppedCount = 0;
    LexOutcome Outcome = LexOutcome::Complete;

    explicit Lexed(std::string text) : Text(std::move(text))
    {
        DiagnosticSink sink(Diagnostics, DroppedCount);
        LexInput input;
        input.Text = Text;
        input.FileIndex = 0;
        Outcome = LexText(input, Tokens, sink);
    }

    [[nodiscard]] std::string_view Spell(usize index) const
    {
        return std::string_view(Text).substr(Tokens[index].SpellingOffset, Tokens[index].SpellingSize);
    }

    [[nodiscard]] bool HasDiagnostic(DiagnosticCode code) const
    {
        for (const Diagnostic& diagnostic : Diagnostics)
        {
            if (diagnostic.Code == code)
            {
                return true;
            }
        }
        return false;
    }
};
} // namespace

TEST_CASE("LUSL lexer takes the longest punctuator", "[shader-language][lexer]")
{
    const Lexed lexed("a<<=b>>c...d!=e!f##g::h->i");
    REQUIRE(lexed.Outcome == LexOutcome::Complete);
    REQUIRE(lexed.Tokens.GetSize() == 17);
    CHECK(lexed.Tokens[1].Symbol == Punctuator::LessLessEqual);
    CHECK(lexed.Tokens[3].Symbol == Punctuator::GreaterGreater);
    CHECK(lexed.Tokens[5].Symbol == Punctuator::Ellipsis);
    CHECK(lexed.Tokens[7].Symbol == Punctuator::ExclamationEqual);
    CHECK(lexed.Tokens[9].Symbol == Punctuator::Exclamation);
    CHECK(lexed.Tokens[11].Symbol == Punctuator::HashHash);
    CHECK(lexed.Tokens[13].Symbol == Punctuator::ColonColon);
    CHECK(lexed.Tokens[15].Symbol == Punctuator::Arrow);
    CHECK(lexed.Tokens[16].Kind == TokenKind::Identifier);
}

TEST_CASE("LUSL lexer reads preprocessing numbers and strings", "[shader-language][lexer]")
{
    const Lexed lexed("1.5e+3 0x1F .5 2.0h \"a\\\"b\\\\c\\n\" \"\xc3\xa9\"");
    REQUIRE(lexed.Outcome == LexOutcome::Complete);
    REQUIRE(lexed.Tokens.GetSize() == 6);
    CHECK(lexed.Spell(0) == "1.5e+3");
    CHECK(lexed.Spell(1) == "0x1F");
    CHECK(lexed.Spell(2) == ".5");
    CHECK(lexed.Spell(3) == "2.0h");
    CHECK(lexed.Tokens[4].Kind == TokenKind::String);
    CHECK(lexed.Spell(4) == "\"a\\\"b\\\\c\\n\"");
    CHECK(lexed.Tokens[5].Kind == TokenKind::String);
}

TEST_CASE("LUSL lexer records lines, columns and spacing", "[shader-language][lexer]")
{
    const Lexed lexed("a  b\n\tc /* x\n y */ d\n#define E \\\n  f");
    REQUIRE(lexed.Outcome == LexOutcome::Complete);
    REQUIRE(lexed.Tokens.GetSize() == 8);
    CHECK(lexed.Tokens[0].IsAtLineStart == true);
    CHECK(lexed.Tokens[1].HasLeadingSpace == true);
    CHECK(lexed.Tokens[1].Location.Column == 4);
    CHECK(lexed.Tokens[2].Location.Line == 2);
    CHECK(lexed.Tokens[2].Location.Column == 2);
    CHECK(lexed.Tokens[2].IsAtLineStart == true);
    // A block comment spanning lines does not start a new logical line.
    CHECK(lexed.Tokens[3].Location.Line == 3);
    CHECK(lexed.Tokens[3].IsAtLineStart == false);
    CHECK(lexed.Tokens[4].Symbol == Punctuator::Hash);
    CHECK(lexed.Tokens[4].IsAtLineStart == true);
    // A backslash before a line break continues the logical line.
    CHECK(lexed.Tokens[7].Location.Line == 5);
    CHECK(lexed.Tokens[7].IsAtLineStart == false);
    CHECK(lexed.Tokens[7].HasLeadingSpace == true);
}

TEST_CASE("LUSL lexer accepts UTF-8 only in comments and strings", "[shader-language][lexer]")
{
    const Lexed comment("// caf\xc3\xa9\nx /* \xe2\x9c\x93 */");
    CHECK(comment.Outcome == LexOutcome::Complete);
    CHECK(comment.Tokens.GetSize() == 1);

    const Lexed identifier("caf\xc3\xa9 = 1;");
    CHECK(identifier.Outcome == LexOutcome::Failed);
    CHECK(identifier.HasDiagnostic(DiagnosticCode::InvalidCharacter) == true);
    REQUIRE(identifier.Diagnostics.GetSize() == 1);
}

TEST_CASE("LUSL lexer reports invalid tokens and keeps going", "[shader-language][lexer]")
{
    CHECK(Lexed("a @ b").HasDiagnostic(DiagnosticCode::InvalidCharacter) == true);
    CHECK(Lexed("a \\ b").HasDiagnostic(DiagnosticCode::InvalidCharacter) == true);
    CHECK(Lexed("'a'").HasDiagnostic(DiagnosticCode::CharacterLiteral) == true);
    CHECK(Lexed("\"open\nx").HasDiagnostic(DiagnosticCode::UnterminatedString) == true);
    CHECK(Lexed("\"bad \\q\"").HasDiagnostic(DiagnosticCode::InvalidEscape) == true);
    CHECK(Lexed("x /* open").HasDiagnostic(DiagnosticCode::UnterminatedComment) == true);

    const Lexed recovered("a @ b $ c");
    CHECK(recovered.Outcome == LexOutcome::Failed);
    CHECK(recovered.Diagnostics.GetSize() == 2);
    CHECK(recovered.Tokens.GetSize() == 3);
}
