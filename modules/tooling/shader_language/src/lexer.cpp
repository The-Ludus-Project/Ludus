#include "internal/lexer.hpp"

#include "internal/utf8_bytes.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::uint32;
using foundation::usize;

struct PunctuatorSpelling final
{
    std::string_view Spelling;
    Punctuator Symbol = Punctuator::None;
};

/// Number of punctuator spellings; every Punctuator except None.
constexpr usize PUNCTUATOR_COUNT = 49;

// Longest spellings first so the first match is the longest match.
constexpr foundation::StaticArray<PunctuatorSpelling, PUNCTUATOR_COUNT> PUNCTUATORS = {{
    {"...", Punctuator::Ellipsis},
    {"<<=", Punctuator::LessLessEqual},
    {">>=", Punctuator::GreaterGreaterEqual},
    {"::", Punctuator::ColonColon},
    {"==", Punctuator::EqualEqual},
    {"!=", Punctuator::ExclamationEqual},
    {"<=", Punctuator::LessEqual},
    {">=", Punctuator::GreaterEqual},
    {"<<", Punctuator::LessLess},
    {">>", Punctuator::GreaterGreater},
    {"&&", Punctuator::AmpersandAmpersand},
    {"||", Punctuator::PipePipe},
    {"+=", Punctuator::PlusEqual},
    {"-=", Punctuator::MinusEqual},
    {"*=", Punctuator::StarEqual},
    {"/=", Punctuator::SlashEqual},
    {"%=", Punctuator::PercentEqual},
    {"&=", Punctuator::AmpersandEqual},
    {"|=", Punctuator::PipeEqual},
    {"^=", Punctuator::CaretEqual},
    {"++", Punctuator::PlusPlus},
    {"--", Punctuator::MinusMinus},
    {"->", Punctuator::Arrow},
    {"##", Punctuator::HashHash},
    {"{", Punctuator::LeftBrace},
    {"}", Punctuator::RightBrace},
    {"[", Punctuator::LeftBracket},
    {"]", Punctuator::RightBracket},
    {"(", Punctuator::LeftParenthesis},
    {")", Punctuator::RightParenthesis},
    {";", Punctuator::Semicolon},
    {",", Punctuator::Comma},
    {".", Punctuator::Period},
    {":", Punctuator::Colon},
    {"?", Punctuator::Question},
    {"=", Punctuator::Equal},
    {"!", Punctuator::Exclamation},
    {"<", Punctuator::Less},
    {">", Punctuator::Greater},
    {"+", Punctuator::Plus},
    {"-", Punctuator::Minus},
    {"*", Punctuator::Star},
    {"/", Punctuator::Slash},
    {"%", Punctuator::Percent},
    {"&", Punctuator::Ampersand},
    {"|", Punctuator::Pipe},
    {"^", Punctuator::Caret},
    {"~", Punctuator::Tilde},
    {"#", Punctuator::Hash},
}};

[[nodiscard]] bool IsIdentifierStart(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] bool IsDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] bool IsIdentifierContinue(char value) noexcept
{
    return IsIdentifierStart(value) == true || IsDigit(value) == true;
}

[[nodiscard]] bool IsHorizontalSpace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] bool IsExponentMark(char value) noexcept
{
    return value == 'e' || value == 'E' || value == 'p' || value == 'P';
}

/// Lexer state over one input; positions are offsets into `Input.Text`.
class Lexer final
{
public:
    Lexer(const LexInput& input, foundation::Array<Token>& inoutTokens, DiagnosticSink& inoutSink) noexcept
        : mInput(input), mTokens(inoutTokens), mSink(inoutSink), mFirstToken(inoutTokens.GetSize()),
          mLine(input.FirstLine)
    {
    }

    [[nodiscard]] LexOutcome Run() noexcept
    {
        const std::string_view text = mInput.Text;
        while (mPosition < text.size())
        {
            const char current = text[mPosition];
            if (current == '\n')
            {
                beginLine(mPosition + 1);
                continue;
            }
            if (IsHorizontalSpace(current) == true)
            {
                mHasSpace = true;
                ++mPosition;
                continue;
            }
            if (current == '\\')
            {
                if (skipLineContinuation() == true)
                {
                    continue;
                }
                reportText(DiagnosticCode::InvalidCharacter,
                           mPosition,
                           "a backslash is only valid before a line break");
                ++mPosition;
                continue;
            }
            if (current == '/' && peek(1) == '/')
            {
                while (mPosition < text.size() && text[mPosition] != '\n')
                {
                    ++mPosition;
                }
                mHasSpace = true;
                continue;
            }
            if (current == '/' && peek(1) == '*')
            {
                if (skipBlockComment() == false)
                {
                    return finish();
                }
                continue;
            }
            const LexOutcome tokenOutcome = lexToken(current);
            if (tokenOutcome != LexOutcome::Complete)
            {
                return tokenOutcome;
            }
        }
        return finish();
    }

private:
    [[nodiscard]] char peek(usize distance) const noexcept
    {
        const usize position = mPosition + distance;
        return position < mInput.Text.size() ? mInput.Text[position] : '\0';
    }

    void beginLine(usize nextPosition) noexcept
    {
        ++mLine;
        mLineStart = nextPosition;
        mPosition = nextPosition;
        mIsAtLineStart = true;
        mHasSpace = false;
    }

    [[nodiscard]] bool skipLineContinuation() noexcept
    {
        usize next = mPosition + 1;
        if (next < mInput.Text.size() && mInput.Text[next] == '\r')
        {
            ++next;
        }
        if (next >= mInput.Text.size() || mInput.Text[next] != '\n')
        {
            return false;
        }
        // The joined line continues the logical line, so line-start state is kept.
        ++mLine;
        mLineStart = next + 1;
        mPosition = next + 1;
        mHasSpace = true;
        return true;
    }

    [[nodiscard]] bool skipBlockComment() noexcept
    {
        const usize start = mPosition;
        mPosition += 2;
        while (mPosition + 1 < mInput.Text.size())
        {
            if (mInput.Text[mPosition] == '*' && mInput.Text[mPosition + 1] == '/')
            {
                mPosition += 2;
                mHasSpace = true;
                return true;
            }
            if (mInput.Text[mPosition] == '\n')
            {
                ++mLine;
                mLineStart = mPosition + 1;
            }
            ++mPosition;
        }
        mPosition = mInput.Text.size();
        reportText(DiagnosticCode::UnterminatedComment, start, "this comment has no closing '*/'");
        return false;
    }

    [[nodiscard]] LexOutcome lexToken(char current) noexcept
    {
        const usize start = mPosition;
        if (IsIdentifierStart(current) == true)
        {
            while (mPosition < mInput.Text.size() && IsIdentifierContinue(mInput.Text[mPosition]) == true)
            {
                ++mPosition;
            }
            return emit(TokenKind::Identifier, Punctuator::None, start);
        }
        if (IsDigit(current) == true || (current == '.' && IsDigit(peek(1)) == true))
        {
            lexNumber();
            return emit(TokenKind::Number, Punctuator::None, start);
        }
        if (current == '"')
        {
            if (lexString() == false)
            {
                return LexOutcome::Complete;
            }
            return emit(TokenKind::String, Punctuator::None, start);
        }
        if (current == '\'')
        {
            skipCharacterLiteral();
            reportText(DiagnosticCode::CharacterLiteral, start, "LUSL has no character literals; use an integer");
            return LexOutcome::Complete;
        }
        for (const PunctuatorSpelling& candidate : PUNCTUATORS)
        {
            if (mInput.Text.substr(mPosition).starts_with(candidate.Spelling) == true)
            {
                mPosition += candidate.Spelling.size();
                return emit(TokenKind::Punctuator, candidate.Symbol, start);
            }
        }
        ++mPosition;
        if (IsNonAscii(current) == true)
        {
            // Skip the rest of the UTF-8 sequence so one character produces one diagnostic.
            while (mPosition < mInput.Text.size() && IsUtf8Continuation(mInput.Text[mPosition]) == true)
            {
                ++mPosition;
            }
            reportText(DiagnosticCode::InvalidCharacter, start, "non-ASCII text is only valid in comments and strings");
            return LexOutcome::Complete;
        }
        reportAt(DiagnosticCode::InvalidCharacter,
                 start,
                 ComposeMessage("'", mInput.Text.substr(start, 1), "' cannot start a token"));
        return LexOutcome::Complete;
    }

    void lexNumber() noexcept
    {
        ++mPosition;
        while (mPosition < mInput.Text.size())
        {
            const char value = mInput.Text[mPosition];
            if ((value == '+' || value == '-') && IsExponentMark(mInput.Text[mPosition - 1]) == true)
            {
                ++mPosition;
                continue;
            }
            if (IsIdentifierContinue(value) == false && value != '.')
            {
                return;
            }
            ++mPosition;
        }
    }

    [[nodiscard]] bool lexString() noexcept
    {
        const usize start = mPosition;
        ++mPosition;
        while (mPosition < mInput.Text.size())
        {
            const char value = mInput.Text[mPosition];
            if (value == '"')
            {
                ++mPosition;
                return true;
            }
            if (value == '\n')
            {
                break;
            }
            if (value == '\\')
            {
                const char escaped = peek(1);
                if (escaped == '\n' || escaped == '\0')
                {
                    ++mPosition;
                    break;
                }
                if (escaped != '\\' && escaped != '"' && escaped != 'n' && escaped != 'r' && escaped != 't')
                {
                    reportText(DiagnosticCode::InvalidEscape,
                               mPosition,
                               "only the escapes \\\\, \\\", \\n, \\r and \\t are valid");
                }
                mPosition += 2;
                continue;
            }
            ++mPosition;
        }
        reportText(DiagnosticCode::UnterminatedString, start, "this string has no closing quote on its line");
        return false;
    }

    void skipCharacterLiteral() noexcept
    {
        ++mPosition;
        while (mPosition < mInput.Text.size() && mInput.Text[mPosition] != '\n')
        {
            const char value = mInput.Text[mPosition];
            ++mPosition;
            if (value == '\'')
            {
                return;
            }
            if (value == '\\' && mPosition < mInput.Text.size() && mInput.Text[mPosition] != '\n')
            {
                ++mPosition;
            }
        }
    }

    [[nodiscard]] SourceLocation locate(usize position) const noexcept
    {
        SourceLocation location;
        location.FileIndex = mInput.FileIndex;
        location.ByteOffset = mInput.LocationBase + static_cast<uint32>(position);
        location.Line = mLine;
        location.Column = static_cast<uint32>(position - mLineStart) + 1;
        return location;
    }

    void reportAt(DiagnosticCode code, usize position, const DiagnosticMessage& message) noexcept
    {
        mSink.ReportError(code, locate(position), SourceLocation{}, message);
        mHasErrors = true;
    }

    void reportText(DiagnosticCode code, usize position, std::string_view message) noexcept
    {
        reportAt(code, position, ComposeMessage(message));
    }

    [[nodiscard]] LexOutcome emit(TokenKind kind, Punctuator symbol, usize start) noexcept
    {
        if (mTokens.GetSize() - mFirstToken >= MAX_FILE_TOKENS)
        {
            reportAt(DiagnosticCode::FileTokenLimit,
                     start,
                     ComposeMessage("more than ", foundation::uint64{MAX_FILE_TOKENS}, " tokens in one source"));
            return LexOutcome::TokenLimit;
        }
        Token token;
        token.Kind = kind;
        token.Symbol = symbol;
        token.IsAtLineStart = mIsAtLineStart;
        token.HasLeadingSpace = mHasSpace;
        token.IsSynthesized = mInput.IsSynthesized;
        token.SpellingOffset = mInput.SpellingBase + static_cast<uint32>(start);
        token.SpellingSize = static_cast<uint32>(mPosition - start);
        token.Location = locate(start);
        if (mTokens.TryAdd(token) == false)
        {
            mSink.ReportOutOfMemory();
            return LexOutcome::OutOfMemory;
        }
        mIsAtLineStart = false;
        mHasSpace = false;
        return LexOutcome::Complete;
    }

    [[nodiscard]] LexOutcome finish() const noexcept
    {
        return mHasErrors == true ? LexOutcome::Failed : LexOutcome::Complete;
    }

    const LexInput& mInput;
    foundation::Array<Token>& mTokens;
    DiagnosticSink& mSink;
    usize mFirstToken = 0;
    usize mPosition = 0;
    usize mLineStart = 0;
    uint32 mLine = 1;
    bool mIsAtLineStart = true;
    bool mHasSpace = false;
    bool mHasErrors = false;
};
} // namespace

LexOutcome LexText(const LexInput& input, foundation::Array<Token>& inoutTokens, DiagnosticSink& inoutSink) noexcept
{
    Lexer lexer(input, inoutTokens, inoutSink);
    return lexer.Run();
}

SourceLocation LocateByte(foundation::uint16 fileIndex, std::string_view text, foundation::uint32 byteOffset) noexcept
{
    SourceLocation location;
    location.FileIndex = fileIndex;
    location.ByteOffset = byteOffset;
    location.Line = 1;
    usize lineStart = 0;
    const usize end = byteOffset < text.size() ? byteOffset : text.size();
    for (usize i = 0; i < end; ++i)
    {
        if (text[i] == '\n')
        {
            ++location.Line;
            lineStart = i + 1;
        }
    }
    location.Column = static_cast<uint32>(end - lineStart) + 1;
    return location;
}
} // namespace ludus::tooling::shader_language::internal
