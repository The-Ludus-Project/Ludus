// Token-based formatter. Like clang-format it works on the unpreprocessed token stream, so it keeps
// macros, conditional groups and comments; it never adds, removes or reorders a token.

#include <ludus/tooling/shader_language/formatter.hpp>

#include <ludus/foundation/strings/utf8.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

#include "internal/diagnostic_sink.hpp"
#include "internal/keywords.hpp"
#include "internal/lexer.hpp"

#include <span>

namespace ludus::tooling::shader_language
{
namespace
{
using foundation::Array;
using foundation::uint32;
using foundation::usize;
using internal::ComposeMessage;

/// Spaces per indentation level, matching the repository's IndentWidth.
constexpr usize INDENT_WIDTH = 4;
/// Extra indentation of a continued statement, matching ContinuationIndentWidth.
constexpr usize CONTINUATION_WIDTH = 4;
/// Most line breaks kept between two tokens: one blank line.
constexpr uint32 MAX_LINE_BREAKS = 2;

/// One open bracket while formatting.
struct OpenBracket final
{
    Punctuator Symbol = Punctuator::None;
    /// A `{` that opens a block, rather than an initializer list.
    bool IsBlock = false;
    /// A block whose `case` bodies are indented one extra level.
    bool IsSwitchBody = false;
    /// The switch body is currently inside a `case` clause.
    bool IsInCaseBody = false;
    /// The body of a `do` loop, whose `}` stays on the line of `while`.
    bool IsDoBody = false;
    /// A `[` that starts an attribute list at a statement boundary.
    bool IsAttribute = false;
    /// Output column just after the bracket.
    usize AlignColumn = 0;
    /// Indentation of the line holding the bracket.
    usize LineIndent = 0;
    /// Something follows the bracket on its line, so continuation lines align after it.
    bool HasContentAfter = false;
};

class Formatter final
{
public:
    Formatter(std::string_view text, FormatResult& outResult) noexcept
        : mText(text), mResult(outResult), mSink(outResult.Diagnostics, outResult.DroppedDiagnosticCount)
    {
    }

    [[nodiscard]] Status Run() noexcept
    {
        if (validate() == false)
        {
            return finish(Status::InvalidRequest);
        }
        internal::LexInput input;
        input.Text = mText;
        input.FileIndex = 0;
        input.ShouldKeepComments = true;
        const internal::LexOutcome lexed = internal::LexText(input, mTokens, mSink);
        if (lexed == internal::LexOutcome::OutOfMemory || mSink.HasOutOfMemory() == true)
        {
            return finish(Status::OutOfMemory);
        }
        if (lexed != internal::LexOutcome::Complete || mSink.HasErrors() == true)
        {
            return finish(Status::Failed);
        }
        if (markGenericBrackets() == false || computeEndLines() == false)
        {
            return finish(Status::OutOfMemory);
        }
        usize index = 0;
        while (index < mTokens.GetSize() && mHasFailed == false)
        {
            if (isDirectiveStart(index) == true)
            {
                index = emitDirective(index);
                continue;
            }
            emitToken(index);
            ++index;
        }
        if (mHasFailed == true)
        {
            return finish(mSink.HasOutOfMemory() == true ? Status::OutOfMemory : Status::Failed);
        }
        if (mOutput.IsEmpty() == false && append("\n") == false)
        {
            return finish(mSink.HasOutOfMemory() == true ? Status::OutOfMemory : Status::Failed);
        }
        if (mResult.Text.TryAddRange(mOutput.AsSpan()) == false)
        {
            return finish(Status::OutOfMemory);
        }
        return finish(Status::Ready);
    }

private:
    [[nodiscard]] Status finish(Status outcome) noexcept
    {
        if (outcome != Status::Ready)
        {
            mResult.Text.Clear();
        }
        mResult.Outcome = outcome;
        return outcome;
    }

    [[nodiscard]] bool validate() noexcept
    {
        foundation::Utf8View checked;
        usize errorOffset = 0;
        if (mText.size() > shader_compiler::MAX_SOURCE_BYTES || mText.find('\0') != std::string_view::npos ||
            foundation::ValidateUtf8(mText, checked, errorOffset) != foundation::StringStatus::Ok)
        {
            mSink.ReportError(DiagnosticCode::InvalidRequest,
                              SourceLocation{},
                              SourceLocation{},
                              ComposeMessage("the source must be UTF-8 without zero bytes and at most ",
                                             foundation::uint64{shader_compiler::MAX_SOURCE_BYTES},
                                             " bytes"));
            return false;
        }
        return true;
    }

    // ----- Token facts ---------------------------------------------------------------------------

    [[nodiscard]] std::string_view spell(usize index) const noexcept
    {
        const Token& token = mTokens[index];
        return mText.substr(token.SpellingOffset, token.SpellingSize);
    }

    [[nodiscard]] bool isPunctuator(usize index, Punctuator symbol) const noexcept
    {
        return index < mTokens.GetSize() && mTokens[index].Kind == TokenKind::Punctuator &&
               mTokens[index].Symbol == symbol;
    }

    [[nodiscard]] bool isComment(usize index) const noexcept
    {
        return mTokens[index].Kind == TokenKind::Comment;
    }

    [[nodiscard]] bool isLineComment(usize index) const noexcept
    {
        return isComment(index) == true && spell(index).starts_with("//") == true;
    }

    [[nodiscard]] bool computeEndLines() noexcept
    {
        if (mEndLines.TryResize(mTokens.GetSize()) == false)
        {
            return false;
        }
        for (usize i = 0; i < mTokens.GetSize(); ++i)
        {
            uint32 line = mTokens[i].Location.Line;
            for (const char value : spell(i))
            {
                line += value == '\n' ? 1 : 0;
            }
            mEndLines[i] = line;
        }
        return true;
    }

    /// Next token after `index` that is not a comment, or the token count.
    [[nodiscard]] usize getNextCode(usize index) const noexcept
    {
        usize next = index + 1;
        while (next < mTokens.GetSize() && isComment(next) == true)
        {
            ++next;
        }
        return next;
    }

    /// Mark `<` and `>` pairs that enclose type arguments rather than compare.
    [[nodiscard]] bool markGenericBrackets() noexcept
    {
        if (mIsGeneric.TryResize(mTokens.GetSize(), 0) == false)
        {
            return false;
        }
        for (usize open = 0; open < mTokens.GetSize(); ++open)
        {
            if (isPunctuator(open, Punctuator::Less) == false || open == 0 ||
                mTokens[open - 1].Kind != TokenKind::Identifier)
            {
                continue;
            }
            usize nesting = 0;
            usize index = open;
            bool isGeneric = false;
            while (index < mTokens.GetSize())
            {
                const Token& token = mTokens[index];
                if (token.Kind == TokenKind::Punctuator)
                {
                    if (token.Symbol == Punctuator::Less)
                    {
                        ++nesting;
                    }
                    else if (token.Symbol == Punctuator::Greater || token.Symbol == Punctuator::GreaterGreater)
                    {
                        const usize closed = token.Symbol == Punctuator::Greater ? 1 : 2;
                        if (closed > nesting)
                        {
                            break;
                        }
                        nesting -= closed;
                        if (nesting == 0)
                        {
                            isGeneric = true;
                            break;
                        }
                    }
                    else if (token.Symbol != Punctuator::Comma && token.Symbol != Punctuator::ColonColon)
                    {
                        break;
                    }
                }
                else if (token.Kind != TokenKind::Identifier && token.Kind != TokenKind::Number)
                {
                    break;
                }
                ++index;
            }
            if (isGeneric == true)
            {
                for (usize k = open; k <= index; ++k)
                {
                    if (mTokens[k].Kind == TokenKind::Punctuator)
                    {
                        mIsGeneric[k] = 1;
                    }
                }
                open = index;
            }
        }
        return true;
    }

    [[nodiscard]] bool isGeneric(usize index) const noexcept
    {
        return mIsGeneric[index] != 0;
    }

    // ----- Output --------------------------------------------------------------------------------

    [[nodiscard]] bool append(std::string_view text) noexcept
    {
        if (text.size() > shader_compiler::MAX_SOURCE_BYTES - mOutput.GetSize())
        {
            if (mHasFailed == false)
            {
                mSink.ReportError(DiagnosticCode::FormattedSourceLimit,
                                  SourceLocation{},
                                  SourceLocation{},
                                  ComposeMessage("the formatted source would exceed ",
                                                 foundation::uint64{shader_compiler::MAX_SOURCE_BYTES},
                                                 " bytes"));
            }
            mHasFailed = true;
            return false;
        }
        if (mOutput.TryAddRange(std::span<const char>(text.data(), text.size())) == false)
        {
            mSink.ReportOutOfMemory();
            mHasFailed = true;
            return false;
        }
        for (const char value : text)
        {
            mColumn = value == '\n' ? 0 : mColumn + 1;
        }
        return true;
    }

    void appendSpaces(usize count) noexcept
    {
        for (usize i = 0; i < count && mHasFailed == false; ++i)
        {
            (void)append(" ");
        }
    }

    /// End the current output line, adding one blank line when `isBlankLineKept`.
    void breakLine(bool isBlankLineKept) noexcept
    {
        if (mOutput.IsEmpty() == true)
        {
            return;
        }
        (void)append(isBlankLineKept == true ? "\n\n" : "\n");
        mIsLineEmpty = true;
    }

    // ----- Directives ----------------------------------------------------------------------------

    /// A `#` that is the first code on its line starts a directive.
    [[nodiscard]] bool isDirectiveStart(usize index) const noexcept
    {
        if (isPunctuator(index, Punctuator::Hash) == false)
        {
            return false;
        }
        if (mTokens[index].IsAtLineStart == true)
        {
            return true;
        }
        // Comments may precede the '#' on its line.
        usize previous = index;
        while (previous > 0)
        {
            --previous;
            if (isComment(previous) == false || mEndLines[previous] != mTokens[index].Location.Line)
            {
                return false;
            }
            if (mTokens[previous].IsAtLineStart == true)
            {
                return true;
            }
        }
        return true;
    }

    /// Emit the directive starting at `start` verbatim after its normalized `#`; return the next token.
    [[nodiscard]] usize emitDirective(usize start) noexcept
    {
        usize end = start + 1;
        while (end < mTokens.GetSize() && mTokens[end].IsAtLineStart == false)
        {
            ++end;
        }
        const usize last = end - 1;
        const bool hasName = end > start + 1 && mTokens[start + 1].Kind == TokenKind::Identifier;
        const std::string_view name = hasName == true ? spell(start + 1) : std::string_view{};
        if (name == "endif" || name == "else" || name == "elif")
        {
            mDirectiveDepth = mDirectiveDepth > 0 ? mDirectiveDepth - 1 : 0;
        }
        startLine(mTokens[start], 0);
        (void)append("#");
        if (end > start + 1)
        {
            appendSpaces(mDirectiveDepth * INDENT_WIDTH);
            const usize from = mTokens[start + 1].SpellingOffset;
            const usize to = mTokens[last].SpellingOffset + mTokens[last].SpellingSize;
            (void)append(mText.substr(from, to - from));
        }
        if (name == "if" || name == "ifdef" || name == "ifndef" || name == "else" || name == "elif")
        {
            ++mDirectiveDepth;
        }
        mIsLineEmpty = false;
        mHasForcedBreak = true;
        mPreviousToken = last;
        return end;
    }

    // ----- Line layout ---------------------------------------------------------------------------

    /// Indentation of a statement at the current block nesting.
    [[nodiscard]] usize getStatementIndent(bool isCaseLabel) const noexcept
    {
        usize indent = 0;
        usize innermostBlock = mBrackets.GetSize();
        for (usize i = 0; i < mBrackets.GetSize(); ++i)
        {
            if (mBrackets[i].IsBlock == true)
            {
                innermostBlock = i;
            }
        }
        for (usize i = 0; i < mBrackets.GetSize(); ++i)
        {
            const OpenBracket& bracket = mBrackets[i];
            if (bracket.IsBlock == false)
            {
                continue;
            }
            indent += INDENT_WIDTH;
            const bool isLabelLine = isCaseLabel == true && i == innermostBlock;
            if (bracket.IsSwitchBody == true && bracket.IsInCaseBody == true && isLabelLine == false)
            {
                indent += INDENT_WIDTH;
            }
        }
        return indent;
    }

    /// True when the previous code token ends a statement, block, label or attribute list.
    [[nodiscard]] bool isAtStatementBoundary() const noexcept
    {
        if (mPreviousCode == NO_TOKEN_INDEX)
        {
            return true;
        }
        const Token& previous = mTokens[mPreviousCode];
        if (previous.Kind != TokenKind::Punctuator)
        {
            return false;
        }
        return previous.Symbol == Punctuator::Semicolon || previous.Symbol == Punctuator::LeftBrace ||
               previous.Symbol == Punctuator::RightBrace || mPreviousCode == mLastLabelColon ||
               mPreviousCode == mLastAttributeClose;
    }

    /// Start a new output line for `token` at `indent` columns.
    void startLine(const Token& token, usize indent) noexcept
    {
        bool isBlankLineKept = false;
        if (mPreviousToken != NO_TOKEN_INDEX && mIsBlockJustOpened == false &&
            (token.Kind != TokenKind::Punctuator || token.Symbol != Punctuator::RightBrace))
        {
            const uint32 gap = token.Location.Line - mEndLines[mPreviousToken];
            isBlankLineKept = gap >= MAX_LINE_BREAKS;
        }
        breakLine(isBlankLineKept);
        appendSpaces(indent);
        mIsLineEmpty = true;
    }

    [[nodiscard]] usize getContinuationIndent() const noexcept
    {
        for (usize i = mBrackets.GetSize(); i > 0; --i)
        {
            const OpenBracket& bracket = mBrackets[i - 1];
            if (bracket.IsBlock == true)
            {
                break;
            }
            return bracket.HasContentAfter == true ? bracket.AlignColumn : bracket.LineIndent + CONTINUATION_WIDTH;
        }
        return getStatementIndent(false) + CONTINUATION_WIDTH;
    }

    // ----- Tokens --------------------------------------------------------------------------------

    /// True when the `{` being emitted opens a block rather than an initializer list.
    [[nodiscard]] bool isBlockBrace() const noexcept
    {
        if (mPreviousCode == NO_TOKEN_INDEX)
        {
            return true;
        }
        const Token& previous = mTokens[mPreviousCode];
        if (previous.Kind == TokenKind::Identifier)
        {
            return spell(mPreviousCode) != "return";
        }
        if (previous.Kind != TokenKind::Punctuator)
        {
            return true;
        }
        switch (previous.Symbol)
        {
            case Punctuator::Equal:
            case Punctuator::LeftParenthesis:
            case Punctuator::LeftBracket:
            case Punctuator::Comma:
            case Punctuator::Question:
                return false;
            case Punctuator::LeftBrace:
                return mBrackets.IsEmpty() == true || mBrackets.GetLast().IsBlock == true;
            case Punctuator::Colon:
                return mPreviousCode == mLastLabelColon;
            default:
                return true;
        }
    }

    /// True when `-`, `+` or `~` at the current position is a prefix operator.
    [[nodiscard]] bool isUnaryPosition() const noexcept
    {
        if (mPreviousCode == NO_TOKEN_INDEX)
        {
            return true;
        }
        const Token& previous = mTokens[mPreviousCode];
        if (previous.Kind == TokenKind::Identifier)
        {
            return internal::IsKeyword(spell(mPreviousCode)) == true && spell(mPreviousCode) != "true" &&
                   spell(mPreviousCode) != "false";
        }
        if (previous.Kind != TokenKind::Punctuator)
        {
            return false;
        }
        if (isGeneric(mPreviousCode) == true && previous.Symbol != Punctuator::Less)
        {
            return false;
        }
        return previous.Symbol != Punctuator::RightParenthesis && previous.Symbol != Punctuator::RightBracket &&
               (previous.Symbol != Punctuator::PlusPlus || mIsPreviousPrefix == true) &&
               (previous.Symbol != Punctuator::MinusMinus || mIsPreviousPrefix == true);
    }

    /// Whether a space separates the previous token from token `index` on one line.
    [[nodiscard]] bool needsSpace(usize index) const noexcept
    {
        if (mIsLineEmpty == true)
        {
            return false;
        }
        const Token& current = mTokens[index];
        if (current.Kind == TokenKind::Comment || isComment(mPreviousToken) == true)
        {
            return true;
        }
        const Token& previous = mTokens[mPreviousToken];
        const bool isPreviousPunctuator = previous.Kind == TokenKind::Punctuator;
        const bool isCurrentPunctuator = current.Kind == TokenKind::Punctuator;
        const Punctuator before = isPreviousPunctuator == true ? previous.Symbol : Punctuator::None;
        const Punctuator symbol = isCurrentPunctuator == true ? current.Symbol : Punctuator::None;
        const bool isInInitializerList = mBrackets.IsEmpty() == false && mBrackets.GetLast().IsBlock == false &&
                                         mBrackets.GetLast().Symbol == Punctuator::LeftBrace;
        if (symbol == Punctuator::Period && isInInitializerList == true &&
            (before == Punctuator::LeftBrace || before == Punctuator::Comma))
        {
            // A designated field: `{ .A = a, .B = b }`.
            return true;
        }
        if (symbol == Punctuator::Comma || symbol == Punctuator::Semicolon || symbol == Punctuator::RightParenthesis ||
            symbol == Punctuator::RightBracket || symbol == Punctuator::Period || symbol == Punctuator::ColonColon)
        {
            return false;
        }
        if (before == Punctuator::LeftParenthesis || before == Punctuator::LeftBracket ||
            before == Punctuator::Period || before == Punctuator::ColonColon)
        {
            return false;
        }
        if (mIsPreviousUnary == true)
        {
            return false;
        }
        if (isGeneric(mPreviousToken) == true && before == Punctuator::Less)
        {
            return false;
        }
        if (isGeneric(index) == true && symbol == Punctuator::Less && previous.Kind == TokenKind::Identifier &&
            internal::IsKeyword(spell(mPreviousToken)) == true)
        {
            // `template <...>` keeps its space; a type name does not (`Texture2D<float32x4>`).
            return true;
        }
        if (isGeneric(index) == true &&
            (symbol == Punctuator::Less || symbol == Punctuator::Greater || symbol == Punctuator::GreaterGreater))
        {
            return false;
        }
        const bool isPreviousGenericClose = isGeneric(mPreviousToken) == true && before != Punctuator::Less;
        const bool isPreviousOperand = previous.Kind == TokenKind::Identifier ||
                                       before == Punctuator::RightParenthesis || before == Punctuator::RightBracket ||
                                       isPreviousGenericClose == true;
        if (symbol == Punctuator::LeftParenthesis)
        {
            if (previous.Kind == TokenKind::Identifier)
            {
                const std::string_view word = spell(mPreviousToken);
                return internal::IsKeyword(word) == true && word != "static_assert";
            }
            return isPreviousOperand == false;
        }
        if (symbol == Punctuator::LeftBracket)
        {
            return isPreviousOperand == false && before != Punctuator::LeftBracket;
        }
        if (symbol == Punctuator::PlusPlus || symbol == Punctuator::MinusMinus)
        {
            return isPreviousOperand == false;
        }
        if (before == Punctuator::PlusPlus || before == Punctuator::MinusMinus)
        {
            return mIsPreviousPrefix == false;
        }
        if (symbol == Punctuator::Colon && index == mLastLabelColon)
        {
            return false;
        }
        if (before == Punctuator::LeftBrace && mBrackets.IsEmpty() == false && mBrackets.GetLast().IsBlock == false &&
            symbol == Punctuator::RightBrace)
        {
            // An empty initializer list stays `{}`.
            return false;
        }
        return true;
    }

    /// Indentation of a new line that starts with token `index`.
    [[nodiscard]] usize
    getLineIndent(usize index, bool isBlockOpen, bool isBlockClose, bool isCaseLabelStart) const noexcept
    {
        if (isBlockOpen == true || isBlockClose == true || isCaseLabelStart == true)
        {
            return getStatementIndent(isCaseLabelStart);
        }
        const bool isClosing = isPunctuator(index, Punctuator::RightParenthesis) == true ||
                               isPunctuator(index, Punctuator::RightBracket) == true ||
                               isPunctuator(index, Punctuator::RightBrace) == true;
        if (isClosing == true && mBrackets.IsEmpty() == false && mBrackets.GetLast().IsBlock == false)
        {
            // A closing bracket on its own line returns to the line that opened it.
            return mBrackets.GetLast().LineIndent;
        }
        if (isAtStatementBoundary() == true)
        {
            return getStatementIndent(false);
        }
        return getContinuationIndent();
    }

    /// @brief True when token `index` can follow the previous token without a space.
    ///
    /// Relexes the two spellings joined: `. 1` would become the number `.1`, `- -` the operator `--`
    /// and `/ /` a comment. A space is required whenever joining changes the tokens.
    [[nodiscard]] bool isJoinSafe(usize index) noexcept
    {
        if (mIsLineEmpty == true || mPreviousToken == NO_TOKEN_INDEX || isComment(index) == true ||
            isComment(mPreviousToken) == true)
        {
            return true;
        }
        const std::string_view left = spell(mPreviousToken);
        const std::string_view right = spell(index);
        mJoinScratch.Clear();
        if (mJoinScratch.TryAddRange(std::span<const char>(left.data(), left.size())) == false ||
            mJoinScratch.TryAddRange(std::span<const char>(right.data(), right.size())) == false)
        {
            // Keeping the space is always safe.
            return false;
        }
        internal::LexInput input;
        input.Text = std::string_view(mJoinScratch.GetData(), mJoinScratch.GetSize());
        input.FileIndex = 0;
        mJoinTokens.Clear();
        mJoinDiagnostics.Clear();
        usize dropped = 0;
        internal::DiagnosticSink scratchSink(mJoinDiagnostics, dropped);
        const internal::LexOutcome lexed = internal::LexText(input, mJoinTokens, scratchSink);
        return lexed == internal::LexOutcome::Complete && mJoinTokens.GetSize() == 2 &&
               mJoinTokens[0].SpellingSize == left.size();
    }

    void emitToken(usize index) noexcept
    {
        const Token& token = mTokens[index];
        const bool isCode = token.Kind != TokenKind::Comment;
        const bool isOriginalBreak =
            mPreviousToken != NO_TOKEN_INDEX && token.Location.Line > mEndLines[mPreviousToken];
        const bool isTrailingComment = isCode == false && isOriginalBreak == false && mPreviousToken != NO_TOKEN_INDEX;

        // Classify brackets and labels before layout decisions use them.
        const bool isBlockOpen = isPunctuator(index, Punctuator::LeftBrace) == true && isBlockBrace() == true;
        const bool isBlockClose = isPunctuator(index, Punctuator::RightBrace) == true && mBrackets.IsEmpty() == false &&
                                  mBrackets.GetLast().IsBlock == true;
        const bool isCaseLabelStart =
            token.Kind == TokenKind::Identifier && (spell(index) == "case" || spell(index) == "default") &&
            isAtStatementBoundary() == true && mBrackets.IsEmpty() == false && mBrackets.GetLast().IsSwitchBody == true;
        if (isCaseLabelStart == true)
        {
            mIsInCaseLabel = true;
        }
        if (isPunctuator(index, Punctuator::Colon) == true && mIsInCaseLabel == true &&
            (mBrackets.IsEmpty() == true || mBrackets.GetLast().IsBlock == true))
        {
            mLastLabelColon = index;
            mIsInCaseLabel = false;
        }

        // A token starts a line after a forced break, at a block brace, or where the author broke it.
        bool isNewLine =
            mPreviousToken != NO_TOKEN_INDEX && isTrailingComment == false &&
            (mHasForcedBreak == true || isBlockOpen == true || isBlockClose == true || isOriginalBreak == true);
        // A '#' outside a directive must not start a line, where it would become one.
        if (isNewLine == true && isOriginalBreak == false &&
            (isPunctuator(index, Punctuator::Hash) == true || isPunctuator(index, Punctuator::HashHash) == true))
        {
            isNewLine = false;
        }
        // Keep `};` after a struct and `} while` after a do body on the closing line.
        if (isNewLine == true && mWasPreviousBlockClose == true && isOriginalBreak == false)
        {
            const bool isStructEnd = isPunctuator(index, Punctuator::Semicolon) == true;
            const bool isDoWhile =
                mWasPreviousDoBody == true && token.Kind == TokenKind::Identifier && spell(index) == "while";
            if (isStructEnd == true || isDoWhile == true)
            {
                isNewLine = false;
            }
        }

        if (isBlockClose == true)
        {
            mIsClosedDoBody = mBrackets.GetLast().IsDoBody;
            mBrackets.RemoveLast();
        }
        if (isNewLine == true)
        {
            startLine(token, getLineIndent(index, isBlockOpen, isBlockClose, isCaseLabelStart));
        }
        else if (needsSpace(index) == true || isJoinSafe(index) == false)
        {
            (void)append(" ");
        }
        if (isNewLine == false && mBrackets.IsEmpty() == false && mBrackets.GetLast().IsBlock == false &&
            mLastOpenBracket != NO_TOKEN_INDEX && mPreviousToken == mLastOpenBracket)
        {
            mBrackets.GetLast().HasContentAfter = true;
        }

        (void)append(spell(index));
        mIsLineEmpty = false;
        mIsBlockJustOpened = false;
        mHasForcedBreak = false;
        mWasPreviousBlockClose = false;
        mWasPreviousDoBody = false;
        if (isCode == false)
        {
            mHasForcedBreak = isLineComment(index) == true;
            mPreviousToken = index;
            return;
        }
        if (isBlockClose == true)
        {
            mHasForcedBreak = true;
            mWasPreviousBlockClose = true;
            mWasPreviousDoBody = mIsClosedDoBody;
        }
        else
        {
            updateBrackets(index, isBlockOpen);
        }
        if (isPunctuator(index, Punctuator::Semicolon) == true && isInsideParentheses() == false)
        {
            mHasForcedBreak = true;
        }
        if (index == mLastLabelColon)
        {
            mHasForcedBreak = true;
            // A label left open across the switch's `}` has no switch body to mark.
            if (mBrackets.IsEmpty() == false && mBrackets.GetLast().IsSwitchBody == true)
            {
                mBrackets.GetLast().IsInCaseBody = true;
            }
        }
        const bool isSign =
            isPunctuator(index, Punctuator::Minus) == true || isPunctuator(index, Punctuator::Plus) == true ||
            isPunctuator(index, Punctuator::Tilde) == true || isPunctuator(index, Punctuator::Exclamation) == true;
        const bool isIncrement =
            isPunctuator(index, Punctuator::PlusPlus) == true || isPunctuator(index, Punctuator::MinusMinus) == true;
        const bool isUnary = (isSign == true || isIncrement == true) && isUnaryPosition() == true;
        mIsPreviousUnary = isSign == true && isUnary == true;
        mIsPreviousPrefix = isIncrement == true && isUnary == true;
        mPreviousToken = index;
        mPreviousCode = index;
    }

    [[nodiscard]] bool isInsideParentheses() const noexcept
    {
        for (const OpenBracket& bracket : mBrackets)
        {
            if (bracket.Symbol == Punctuator::LeftParenthesis)
            {
                return true;
            }
        }
        return false;
    }

    void updateBrackets(usize index, bool isBlockOpen) noexcept
    {
        const Token& token = mTokens[index];
        if (token.Kind != TokenKind::Punctuator)
        {
            if (token.Kind == TokenKind::Identifier)
            {
                const std::string_view word = spell(index);
                if (word == "switch")
                {
                    mIsSwitchPending = true;
                }
                else if (word == "do")
                {
                    mIsDoPending = true;
                }
            }
            return;
        }
        switch (token.Symbol)
        {
            case Punctuator::LeftParenthesis:
            case Punctuator::LeftBracket:
            case Punctuator::LeftBrace: {
                OpenBracket bracket;
                bracket.Symbol = token.Symbol;
                bracket.IsBlock = isBlockOpen;
                bracket.IsAttribute = token.Symbol == Punctuator::LeftBracket && isAtStatementBoundary() == true;
                bracket.AlignColumn = mColumn;
                bracket.LineIndent = getCurrentLineIndent();
                if (isBlockOpen == true)
                {
                    bracket.IsSwitchBody = mIsSwitchPending;
                    bracket.IsDoBody = mIsDoPending;
                    mIsSwitchPending = false;
                    mIsDoPending = false;
                    mHasForcedBreak = true;
                    mIsBlockJustOpened = true;
                }
                if (mBrackets.TryAdd(bracket) == false)
                {
                    mSink.ReportOutOfMemory();
                    mHasFailed = true;
                }
                mLastOpenBracket = index;
                return;
            }
            case Punctuator::RightParenthesis:
            case Punctuator::RightBracket:
            case Punctuator::RightBrace: {
                if (mBrackets.IsEmpty() == false && mBrackets.GetLast().IsBlock == false)
                {
                    if (mBrackets.GetLast().IsAttribute == true)
                    {
                        mLastAttributeClose = index;
                    }
                    mBrackets.RemoveLast();
                }
                return;
            }
            default:
                return;
        }
    }

    [[nodiscard]] usize getCurrentLineIndent() const noexcept
    {
        usize start = mOutput.GetSize();
        while (start > 0 && mOutput[start - 1] != '\n')
        {
            --start;
        }
        usize indent = 0;
        while (start + indent < mOutput.GetSize() && mOutput[start + indent] == ' ')
        {
            ++indent;
        }
        return indent;
    }

    static constexpr usize NO_TOKEN_INDEX = ~usize{0};

    std::string_view mText;
    FormatResult& mResult;
    internal::DiagnosticSink mSink;
    Array<Token> mTokens;
    Array<uint32> mEndLines;
    Array<foundation::uint8> mIsGeneric;
    Array<OpenBracket> mBrackets;
    Array<char> mOutput;
    Array<char> mJoinScratch;
    Array<Token> mJoinTokens;
    Array<Diagnostic> mJoinDiagnostics;
    usize mColumn = 0;
    usize mDirectiveDepth = 0;
    usize mPreviousToken = NO_TOKEN_INDEX;
    usize mPreviousCode = NO_TOKEN_INDEX;
    usize mLastLabelColon = NO_TOKEN_INDEX;
    usize mLastAttributeClose = NO_TOKEN_INDEX;
    usize mLastOpenBracket = NO_TOKEN_INDEX;
    bool mIsLineEmpty = true;
    bool mHasForcedBreak = false;
    bool mIsBlockJustOpened = false;
    bool mWasPreviousBlockClose = false;
    bool mWasPreviousDoBody = false;
    bool mIsClosedDoBody = false;
    bool mIsSwitchPending = false;
    bool mIsDoPending = false;
    bool mIsInCaseLabel = false;
    bool mIsPreviousUnary = false;
    bool mIsPreviousPrefix = false;
    bool mHasFailed = false;
};
} // namespace

Status FormatSource(std::string_view text, FormatResult& outResult) noexcept
{
    outResult = FormatResult{};
    Formatter formatter(text, outResult);
    return formatter.Run();
}
} // namespace ludus::tooling::shader_language
