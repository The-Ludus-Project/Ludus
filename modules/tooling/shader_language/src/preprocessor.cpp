#include <ludus/tooling/shader_language/preprocessor.hpp>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/strings/utf8.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

#include "internal/condition.hpp"
#include "internal/diagnostic_sink.hpp"
#include "internal/hide_set.hpp"
#include "internal/include_path.hpp"
#include "internal/lexer.hpp"
#include "internal/macro_expander.hpp"
#include "internal/macro_table.hpp"
#include "internal/token_text.hpp"

namespace ludus::tooling::shader_language
{
namespace
{
using foundation::Array;
using foundation::uint16;
using foundation::uint32;
using foundation::usize;
using internal::BodyToken;
using internal::ConditionToken;
using internal::DiagnosticSink;
using internal::MacroDefinition;
using internal::MacroTableOutcome;
using internal::WorkToken;

/// Suffix of a compilable LUSL module; the root file must have it.
constexpr std::string_view MODULE_SUFFIX = ".lusl";
/// Suffix of an include-only LUSL header; `#include` accepts only these.
constexpr std::string_view HEADER_SUFFIX = ".luslh";
/// First synthesized bytes: the spellings `defined` is replaced by.
constexpr std::string_view DEFINED_RESULTS = "01";
/// Offset of "0" in DEFINED_RESULTS.
constexpr uint32 FALSE_SPELLING_OFFSET = 0;
/// Offset of "1" in DEFINED_RESULTS.
constexpr uint32 TRUE_SPELLING_OFFSET = 1;
/// Tokens of a directive with exactly one operand: `#`, the directive name and the operand.
constexpr usize ONE_OPERAND_DIRECTIVE_TOKENS = 3;

[[nodiscard]] std::string_view GetView(const foundation::BorrowedView<const char>& view) noexcept
{
    return view.IsValid() == true ? std::string_view(view.GetDataOrNull(), view.GetSize()) : std::string_view{};
}

[[nodiscard]] bool IsPunctuator(const Token& token, Punctuator symbol) noexcept
{
    return token.Kind == TokenKind::Punctuator && token.Symbol == symbol;
}

[[nodiscard]] bool IsIdentifierText(std::string_view text) noexcept
{
    if (text.empty() == true)
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        const char value = text[i];
        const bool isLetter = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
        const bool isDigit = value >= '0' && value <= '9';
        if (isLetter == false && (i == 0 || isDigit == false))
        {
            return false;
        }
    }
    return true;
}

/// One `#if` group: whether its enclosing region is active and which branch was taken.
struct ConditionalFrame final
{
    SourceLocation Location;
    bool IsParentActive = false;
    bool IsActive = false;
    bool HasTakenBranch = false;
    bool HasSeenElse = false;
};

/// One file on the include stack.
struct IncludeFrame final
{
    uint16 FileIndex = 0;
    usize Position = 0;
    usize ConditionalBase = 0;
};

/// Lexed tokens and include state of one snapshot file.
struct FileState final
{
    Array<Token> Tokens;
    bool IsLexed = false;
    bool HasPragmaOnce = false;
    bool IsOnStack = false;
};

/// Tokens of one directive line: `#` at Start, the name at Start + 1.
struct DirectiveLine final
{
    const Array<Token>& Tokens;
    usize Start = 0;
    usize End = 0;
    uint16 FileIndex = 0;

    [[nodiscard]] usize GetCount() const noexcept
    {
        return End - Start;
    }

    [[nodiscard]] const Token& Get(usize offset) const noexcept
    {
        return Tokens[Start + offset];
    }
};

enum class ConditionalKind : foundation::uint8
{
    If,
    IfDefined,
    IfNotDefined,
};

class Preprocessor final
{
public:
    Preprocessor(const PreprocessRequest& request, PreprocessResult& outResult) noexcept
        : mRequest(request), mResult(outResult), mSink(outResult.Diagnostics, outResult.DroppedDiagnosticCount),
          mText(request.Files, outResult.SynthesizedText),
          mExpander(mMacros,
                    mHideSets,
                    mText,
                    mSink,
                    internal::ExpansionOutputs{outResult.Tokens, outResult.Expansions, outResult.ConfigurationReadMask})
    {
    }

    [[nodiscard]] Status Run() noexcept
    {
        if (validateRequest() == false)
        {
            return finish();
        }
        uint32 offset = 0;
        if (mResult.SourceReadMask.TryResize(mRequest.Files.GetSize(), 0) == false ||
            mResult.ConfigurationReadMask.TryResize(mRequest.Configuration.GetSize(), 0) == false ||
            mFiles.TryResize(mRequest.Files.GetSize()) == false || mMacros.Initialize() != MacroTableOutcome::Ok ||
            mText.TryAppend(DEFINED_RESULTS, offset) != internal::TextOutcome::Ok)
        {
            mSink.ReportOutOfMemory();
            return finish();
        }
        if (defineConfiguration() == false)
        {
            return finish();
        }
        enterFile(mRootIndex, Token{});
        while (mIsFatal == false && mSink.HasOutOfMemory() == false && mIncludes.IsEmpty() == false)
        {
            step();
        }
        return finish();
    }

private:
    // ----- Request -----------------------------------------------------------------------------

    [[nodiscard]] bool rejectRequest(const DiagnosticMessage& message) noexcept
    {
        mSink.ReportError(DiagnosticCode::InvalidRequest, SourceLocation{}, SourceLocation{}, message);
        mIsInvalidRequest = true;
        return false;
    }

    [[nodiscard]] bool validateRequest() noexcept
    {
        if (mRequest.Files.IsValid() == false || mRequest.Source.IsValid() == false ||
            mRequest.Configuration.IsValid() == false || mRequest.IncludeDirectories.IsValid() == false)
        {
            return rejectRequest(internal::ComposeMessage("a request view is invalid"));
        }
        if (mRequest.IncludeDirectories.GetSize() > MAX_INCLUDE_DIRECTORIES)
        {
            return rejectRequest(internal::ComposeMessage("a request names at most ",
                                                          foundation::uint64{MAX_INCLUDE_DIRECTORIES},
                                                          " include directories"));
        }
        for (usize i = 0; i < mRequest.IncludeDirectories.GetSize(); ++i)
        {
            const foundation::BorrowedView<const char> directory = mRequest.IncludeDirectories[i];
            if (directory.IsValid() == false || internal::IsSnapshotPath(GetView(directory)) == false)
            {
                return rejectRequest(
                    internal::ComposeMessage("'", GetView(directory), "' is not a snapshot directory path"));
            }
        }
        const usize fileCount = mRequest.Files.GetSize();
        if (fileCount == 0 || fileCount > shader_compiler::MAX_SOURCE_FILES)
        {
            return rejectRequest(internal::ComposeMessage("a snapshot holds 1 to ",
                                                          foundation::uint64{shader_compiler::MAX_SOURCE_FILES},
                                                          " files"));
        }
        usize totalBytes = 0;
        bool hasRoot = false;
        for (usize i = 0; i < fileCount; ++i)
        {
            const shader_compiler::SourceFile& file = mRequest.Files[i];
            if (file.Path.IsValid() == false || file.Text.IsValid() == false)
            {
                return rejectRequest(internal::ComposeMessage("a file view is invalid"));
            }
            const std::string_view path = GetView(file.Path);
            const std::string_view text = GetView(file.Text);
            if (internal::IsSnapshotPath(path) == false)
            {
                return rejectRequest(internal::ComposeMessage("'", path, "' is not a relative snapshot path"));
            }
            for (usize j = 0; j < i; ++j)
            {
                if (GetView(mRequest.Files[j].Path) == path)
                {
                    return rejectRequest(internal::ComposeMessage("'", path, "' appears twice in the snapshot"));
                }
            }
            if (text.size() > shader_compiler::MAX_SOURCE_BYTES - totalBytes)
            {
                return rejectRequest(internal::ComposeMessage("the snapshot exceeds ",
                                                              foundation::uint64{shader_compiler::MAX_SOURCE_BYTES},
                                                              " bytes"));
            }
            totalBytes += text.size();
            if (text.find('\0') != std::string_view::npos)
            {
                return rejectRequest(internal::ComposeMessage("'", path, "' contains a zero byte"));
            }
            if (path == GetView(mRequest.Source))
            {
                mRootIndex = static_cast<uint16>(i);
                hasRoot = true;
            }
        }
        if (hasRoot == false)
        {
            return rejectRequest(
                internal::ComposeMessage("the root file '", GetView(mRequest.Source), "' is not in the snapshot"));
        }
        if (internal::HasSuffix(GetView(mRequest.Source), MODULE_SUFFIX) == false)
        {
            return rejectRequest(
                internal::ComposeMessage("the root file '", GetView(mRequest.Source), "' is not a .lusl module"));
        }
        const usize valueCount = mRequest.Configuration.GetSize();
        if (valueCount > MAX_CONFIGURATION_VALUES)
        {
            return rejectRequest(internal::ComposeMessage("more than ",
                                                          foundation::uint64{MAX_CONFIGURATION_VALUES},
                                                          " configuration values"));
        }
        for (usize i = 0; i < valueCount; ++i)
        {
            const ConfigurationValue& value = mRequest.Configuration[i];
            if (value.Name.IsValid() == false || value.Value.IsValid() == false)
            {
                return rejectRequest(internal::ComposeMessage("a configuration view is invalid"));
            }
            const std::string_view name = GetView(value.Name);
            const std::string_view text = GetView(value.Value);
            if (name.size() > MAX_CONFIGURATION_TEXT_BYTES || text.size() > MAX_CONFIGURATION_TEXT_BYTES)
            {
                return rejectRequest(internal::ComposeMessage("configuration names and values hold at most ",
                                                              foundation::uint64{MAX_CONFIGURATION_TEXT_BYTES},
                                                              " bytes"));
            }
            if (IsIdentifierText(name) == false || name == "defined")
            {
                return rejectRequest(internal::ComposeMessage("'", name, "' is not a valid configuration name"));
            }
            for (usize j = 0; j < i; ++j)
            {
                if (GetView(mRequest.Configuration[j].Name) == name)
                {
                    return rejectRequest(internal::ComposeMessage("configuration value '", name, "' appears twice"));
                }
            }
            foundation::Utf8View checked;
            usize errorOffset = 0;
            if (text.find('\n') != std::string_view::npos || text.find('\0') != std::string_view::npos ||
                foundation::ValidateUtf8(text, checked, errorOffset) != foundation::StringStatus::Ok)
            {
                return rejectRequest(
                    internal::ComposeMessage("configuration value '", name, "' is not one line of UTF-8 text"));
            }
        }
        return true;
    }

    [[nodiscard]] bool defineConfiguration() noexcept
    {
        for (usize i = 0; i < mRequest.Configuration.GetSize(); ++i)
        {
            const ConfigurationValue& value = mRequest.Configuration[i];
            const std::string_view name = GetView(value.Name);
            const std::string_view text = GetView(value.Value);
            uint32 nameOffset = 0;
            uint32 valueOffset = 0;
            if (mText.TryAppend(name, nameOffset) != internal::TextOutcome::Ok ||
                mText.TryAppend(text, valueOffset) != internal::TextOutcome::Ok)
            {
                return rejectRequest(internal::ComposeMessage("configuration values exceed ",
                                                              foundation::uint64{MAX_SYNTHESIZED_BYTES},
                                                              " bytes"));
            }
            Token nameToken;
            nameToken.Kind = TokenKind::Identifier;
            nameToken.IsSynthesized = true;
            nameToken.SpellingOffset = nameOffset;
            nameToken.SpellingSize = static_cast<uint32>(name.size());
            nameToken.Location.FileIndex = CONFIGURATION_SOURCE;
            nameToken.Location.Line = static_cast<uint32>(i + 1);
            nameToken.Location.Column = 1;

            internal::LexInput input;
            input.Text = mText.GetSynthesized().substr(valueOffset, text.size());
            input.FileIndex = CONFIGURATION_SOURCE;
            input.FirstLine = static_cast<uint32>(i + 1);
            input.IsSynthesized = true;
            input.SpellingBase = valueOffset;
            Array<Token> tokens;
            const internal::LexOutcome lexed = internal::LexText(input, tokens, mSink);
            if (lexed == internal::LexOutcome::OutOfMemory)
            {
                return false;
            }
            if (lexed != internal::LexOutcome::Complete)
            {
                return rejectRequest(
                    internal::ComposeMessage("configuration value '", name, "' is not valid LUSL text"));
            }
            Array<BodyToken> body;
            if (body.TryEnsureCapacity(tokens.GetSize()) == false)
            {
                mSink.ReportOutOfMemory();
                return false;
            }
            for (const Token& token : tokens)
            {
                body.Add(BodyToken{token, internal::NO_PARAMETER});
            }
            if (validateBody(body, false) == false)
            {
                return rejectRequest(
                    internal::ComposeMessage("configuration value '", name, "' cannot begin or end with '##'"));
            }
            uint32 macroIndex = 0;
            if (checkMacroOutcome(mMacros.TryIntern(name, nameToken, macroIndex), nameToken) == false)
            {
                return false;
            }
            MacroDefinition definition;
            definition.Name = nameToken;
            definition.IsDefined = true;
            definition.ConfigurationIndex = static_cast<uint32>(i);
            if (checkMacroOutcome(mMacros.TryDefine(macroIndex, definition, body), nameToken) == false)
            {
                return false;
            }
        }
        return true;
    }

    // ----- Files ---------------------------------------------------------------------------------

    void lexFile(uint16 fileIndex) noexcept
    {
        FileState& file = mFiles[fileIndex];
        file.IsLexed = true;
        mResult.SourceReadMask[fileIndex] = 1;
        const std::string_view text = GetView(mRequest.Files[fileIndex].Text);
        foundation::Utf8View checked;
        usize errorOffset = 0;
        if (foundation::ValidateUtf8(text, checked, errorOffset) != foundation::StringStatus::Ok)
        {
            mSink.ReportError(
                DiagnosticCode::InvalidUtf8,
                internal::LocateByte(fileIndex, text, static_cast<uint32>(errorOffset)),
                SourceLocation{},
                internal::ComposeMessage("'", GetView(mRequest.Files[fileIndex].Path), "' is not well-formed UTF-8"));
            return;
        }
        internal::LexInput input;
        input.Text = text;
        input.FileIndex = fileIndex;
        if (internal::LexText(input, file.Tokens, mSink) == internal::LexOutcome::OutOfMemory)
        {
            mIsFatal = true;
        }
    }

    void enterFile(uint16 fileIndex, const Token& directive) noexcept
    {
        if (mIncludes.GetSize() >= MAX_INCLUDE_DEPTH)
        {
            report(DiagnosticCode::IncludeDepthLimit,
                   directive,
                   internal::ComposeMessage("includes nest deeper than ", foundation::uint64{MAX_INCLUDE_DEPTH}));
            return;
        }
        if (mFiles[fileIndex].IsLexed == false)
        {
            lexFile(fileIndex);
        }
        IncludeFrame frame;
        frame.FileIndex = fileIndex;
        frame.ConditionalBase = mConditionals.GetSize();
        if (mIncludes.TryAdd(frame) == false)
        {
            mSink.ReportOutOfMemory();
            return;
        }
        mFiles[fileIndex].IsOnStack = true;
    }

    void leaveFile() noexcept
    {
        const IncludeFrame frame = mIncludes.GetLast();
        while (mConditionals.GetSize() > frame.ConditionalBase)
        {
            mSink.ReportError(DiagnosticCode::UnterminatedConditional,
                              mConditionals.GetLast().Location,
                              SourceLocation{},
                              internal::ComposeMessage("this group has no '#endif' in its file"));
            mConditionals.RemoveLast();
        }
        mFiles[frame.FileIndex].IsOnStack = false;
        mIncludes.RemoveLast();
    }

    [[nodiscard]] static usize findLineEnd(const Array<Token>& tokens, usize start) noexcept
    {
        usize end = start + 1;
        while (end < tokens.GetSize() && tokens[end].IsAtLineStart == false)
        {
            ++end;
        }
        return end;
    }

    void step() noexcept
    {
        IncludeFrame& frame = mIncludes.GetLast();
        const uint16 fileIndex = frame.FileIndex;
        const Array<Token>& tokens = mFiles[fileIndex].Tokens;
        if (frame.Position >= tokens.GetSize())
        {
            leaveFile();
            return;
        }
        const Token& token = tokens[frame.Position];
        if (token.IsAtLineStart == true && IsPunctuator(token, Punctuator::Hash) == true)
        {
            const usize start = frame.Position;
            const usize end = findLineEnd(tokens, start);
            frame.Position = end;
            // Directives may push an include, which invalidates `frame`.
            handleDirective(DirectiveLine{tokens, start, end, fileIndex});
            return;
        }
        if (isActive() == false)
        {
            frame.Position = findLineEnd(tokens, frame.Position);
            return;
        }
        if (mExpander.ExpandFileText(tokens, frame.Position) == false)
        {
            mIsFatal = true;
        }
    }

    // ----- Directives -----------------------------------------------------------------------------

    [[nodiscard]] bool isActive() const noexcept
    {
        return mConditionals.IsEmpty() == true || mConditionals.GetLast().IsActive == true;
    }

    void handleDirective(const DirectiveLine& line) noexcept
    {
        if (line.GetCount() == 1)
        {
            return;
        }
        const Token& name = line.Get(1);
        const bool isLineActive = isActive();
        if (name.Kind != TokenKind::Identifier)
        {
            if (isLineActive == true)
            {
                report(DiagnosticCode::MalformedDirective,
                       name,
                       internal::ComposeMessage("expected a directive name after '#'"));
            }
            return;
        }
        const std::string_view directive = mText.Spell(name);
        if (directive == "if" || directive == "ifdef" || directive == "ifndef")
        {
            const ConditionalKind kind = directive == "if"      ? ConditionalKind::If
                                         : directive == "ifdef" ? ConditionalKind::IfDefined
                                                                : ConditionalKind::IfNotDefined;
            handleIf(line, kind);
            return;
        }
        if (directive == "elif")
        {
            handleElif(line);
            return;
        }
        if (directive == "else" || directive == "endif")
        {
            handleElseOrEndif(line, directive == "else");
            return;
        }
        if (isLineActive == false)
        {
            return;
        }
        if (directive == "define")
        {
            handleDefine(line);
        }
        else if (directive == "undef")
        {
            handleUndef(line);
        }
        else if (directive == "include")
        {
            handleInclude(line);
        }
        else if (directive == "pragma")
        {
            handlePragma(line);
        }
        else if (directive == "error")
        {
            handleError(line);
        }
        else if (directive == "line")
        {
            report(DiagnosticCode::LineDirective,
                   name,
                   internal::ComposeMessage("'#line' is reserved for generated code"));
        }
        else
        {
            report(DiagnosticCode::UnknownDirective,
                   name,
                   internal::ComposeMessage("'#", directive, "' is not a LUSL directive"));
        }
    }

    [[nodiscard]] bool hasOpenConditional() const noexcept
    {
        return mConditionals.GetSize() > mIncludes.GetLast().ConditionalBase;
    }

    void handleIf(const DirectiveLine& line, ConditionalKind kind) noexcept
    {
        const Token& name = line.Get(1);
        if (mConditionals.GetSize() >= MAX_CONDITIONAL_DEPTH)
        {
            report(
                DiagnosticCode::ConditionalDepthLimit,
                name,
                internal::ComposeMessage("'#if' groups nest deeper than ", foundation::uint64{MAX_CONDITIONAL_DEPTH}));
            mIsFatal = true;
            return;
        }
        const bool isParentActive = isActive();
        bool isTrue = false;
        if (isParentActive == true)
        {
            if (kind == ConditionalKind::If)
            {
                isTrue = evaluateCondition(line);
            }
            else if (line.GetCount() != ONE_OPERAND_DIRECTIVE_TOKENS || line.Get(2).Kind != TokenKind::Identifier)
            {
                report(DiagnosticCode::MalformedDirective,
                       name,
                       internal::ComposeMessage("'#", mText.Spell(name), "' takes exactly one macro name"));
            }
            else
            {
                bool isKnown = false;
                const bool isDefined = testDefined(line.Get(2), isKnown);
                isTrue = isKnown == true && (kind == ConditionalKind::IfDefined ? isDefined : isDefined == false);
            }
        }
        ConditionalFrame frame;
        frame.Location = name.Location;
        frame.IsParentActive = isParentActive;
        frame.IsActive = isParentActive == true && isTrue == true;
        frame.HasTakenBranch = isTrue;
        if (mConditionals.TryAdd(frame) == false)
        {
            mSink.ReportOutOfMemory();
        }
    }

    void handleElif(const DirectiveLine& line) noexcept
    {
        const Token& name = line.Get(1);
        if (hasOpenConditional() == false || mConditionals.GetLast().HasSeenElse == true)
        {
            report(DiagnosticCode::UnmatchedConditional,
                   name,
                   internal::ComposeMessage("'#elif' has no matching '#if', or follows '#else'"));
            return;
        }
        ConditionalFrame& frame = mConditionals.GetLast();
        if (frame.IsParentActive == false || frame.HasTakenBranch == true)
        {
            frame.IsActive = false;
            return;
        }
        const bool isTrue = evaluateCondition(line);
        // Evaluation does not change the conditional stack, so the frame is still current.
        ConditionalFrame& current = mConditionals.GetLast();
        current.IsActive = isTrue;
        current.HasTakenBranch = isTrue;
    }

    void handleElseOrEndif(const DirectiveLine& line, bool isElse) noexcept
    {
        const Token& name = line.Get(1);
        if (hasOpenConditional() == false || (isElse == true && mConditionals.GetLast().HasSeenElse == true))
        {
            report(DiagnosticCode::UnmatchedConditional,
                   name,
                   internal::ComposeMessage("'#", mText.Spell(name), "' has no matching '#if', or follows '#else'"));
            return;
        }
        ConditionalFrame& frame = mConditionals.GetLast();
        if (line.GetCount() != 2 && frame.IsParentActive == true)
        {
            report(DiagnosticCode::MalformedDirective,
                   line.Get(2),
                   internal::ComposeMessage("'#", mText.Spell(name), "' takes no operands"));
        }
        if (isElse == false)
        {
            mConditionals.RemoveLast();
            return;
        }
        frame.IsActive = frame.IsParentActive == true && frame.HasTakenBranch == false;
        frame.HasTakenBranch = true;
        frame.HasSeenElse = true;
    }

    /// Whether `name` is defined; reports and leaves `outIsKnown` false for an unknown name.
    [[nodiscard]] bool testDefined(const Token& name, bool& outIsKnown) noexcept
    {
        outIsKnown = false;
        const std::string_view spelling = mText.Spell(name);
        const uint32 macroIndex = mMacros.Find(spelling);
        if (macroIndex == internal::NO_MACRO)
        {
            report(DiagnosticCode::UnknownMacroName,
                   name,
                   internal::ComposeMessage("'",
                                            spelling,
                                            "' is neither a configuration value nor a macro defined or undefined "
                                            "earlier"));
            return false;
        }
        outIsKnown = true;
        const MacroDefinition& definition = mMacros.Get(macroIndex);
        if (definition.ConfigurationIndex != internal::NO_CONFIGURATION)
        {
            mResult.ConfigurationReadMask[definition.ConfigurationIndex] = 1;
        }
        return definition.IsDefined;
    }

    /// Evaluate the expression of an `#if` or `#elif` line; false on any error.
    [[nodiscard]] bool evaluateCondition(const DirectiveLine& line) noexcept
    {
        Array<WorkToken> input;
        usize i = 2;
        while (i < line.GetCount())
        {
            const Token& token = line.Get(i);
            if (token.Kind != TokenKind::Identifier || mText.Spell(token) != "defined")
            {
                WorkToken plain;
                plain.Value = token;
                if (input.TryAdd(plain) == false)
                {
                    mSink.ReportOutOfMemory();
                    return false;
                }
                ++i;
                continue;
            }
            const bool isParenthesized =
                i + 1 < line.GetCount() && IsPunctuator(line.Get(i + 1), Punctuator::LeftParenthesis) == true;
            const usize nameOffset = isParenthesized == true ? i + 2 : i + 1;
            const bool hasName = nameOffset < line.GetCount() && line.Get(nameOffset).Kind == TokenKind::Identifier;
            const bool isClosed = isParenthesized == false ||
                                  (nameOffset + 1 < line.GetCount() &&
                                   IsPunctuator(line.Get(nameOffset + 1), Punctuator::RightParenthesis) == true);
            if (hasName == false || isClosed == false)
            {
                report(DiagnosticCode::InvalidCondition,
                       token,
                       internal::ComposeMessage("'defined' takes a macro name, as 'defined NAME' or 'defined(NAME)'"));
                return false;
            }
            bool isKnown = false;
            const bool isDefined = testDefined(line.Get(nameOffset), isKnown);
            if (isKnown == false)
            {
                return false;
            }
            WorkToken result;
            result.Value = token;
            result.Value.Kind = TokenKind::Number;
            result.Value.IsSynthesized = true;
            result.Value.SpellingOffset = isDefined == true ? TRUE_SPELLING_OFFSET : FALSE_SPELLING_OFFSET;
            result.Value.SpellingSize = 1;
            if (input.TryAdd(result) == false)
            {
                mSink.ReportOutOfMemory();
                return false;
            }
            i = isParenthesized == true ? nameOffset + 2 : nameOffset + 1;
        }
        Array<WorkToken> expanded;
        if (mExpander.ExpandIsolated(input, expanded) == false)
        {
            mIsFatal = true;
            return false;
        }
        Array<ConditionToken> condition;
        if (condition.TryEnsureCapacity(expanded.GetSize()) == false)
        {
            mSink.ReportOutOfMemory();
            return false;
        }
        for (const WorkToken& token : expanded)
        {
            ConditionToken converted;
            converted.Kind = token.Value.Kind;
            converted.Symbol = token.Value.Symbol;
            converted.Spelling = mText.Spell(token.Value);
            converted.Location = token.Value.Location;
            converted.ExpansionLocation = getExpansionLocation(token.Value);
            if (converted.Kind == TokenKind::Identifier && converted.Spelling == "defined")
            {
                mSink.ReportError(DiagnosticCode::DefinedFromExpansion,
                                  converted.Location,
                                  converted.ExpansionLocation,
                                  internal::ComposeMessage("'defined' must be written in the condition itself, not "
                                                           "produced by a macro"));
                return false;
            }
            condition.Add(converted);
        }
        ConditionToken directive;
        directive.Kind = line.Get(1).Kind;
        directive.Spelling = mText.Spell(line.Get(1));
        directive.Location = line.Get(1).Location;
        bool isTrue = false;
        if (internal::EvaluateCondition(condition, directive, mSink, isTrue) == false)
        {
            return false;
        }
        return isTrue;
    }

    void handleDefine(const DirectiveLine& line) noexcept
    {
        const Token& directive = line.Get(1);
        if (line.GetCount() < ONE_OPERAND_DIRECTIVE_TOKENS || line.Get(2).Kind != TokenKind::Identifier)
        {
            report(DiagnosticCode::MalformedDirective,
                   directive,
                   internal::ComposeMessage("expected a macro name after '#define'"));
            return;
        }
        const Token& nameToken = line.Get(2);
        const std::string_view name = mText.Spell(nameToken);
        if (name == "defined")
        {
            report(DiagnosticCode::ReservedMacroName,
                   nameToken,
                   internal::ComposeMessage("'defined' cannot be a macro name"));
            return;
        }
        // Parameters or the replacement list follow the macro name.
        usize i = ONE_OPERAND_DIRECTIVE_TOKENS;
        const bool isFunctionLike = i < line.GetCount() &&
                                    IsPunctuator(line.Get(i), Punctuator::LeftParenthesis) == true &&
                                    line.Get(i).HasLeadingSpace == false;
        Array<Token> parameters;
        if (isFunctionLike == true && parseParameters(line, i, parameters) == false)
        {
            return;
        }
        Array<BodyToken> body;
        if (body.TryEnsureCapacity(line.GetCount() - i) == false)
        {
            mSink.ReportOutOfMemory();
            return;
        }
        for (; i < line.GetCount(); ++i)
        {
            BodyToken bodyToken{line.Get(i), internal::NO_PARAMETER};
            if (bodyToken.Value.Kind == TokenKind::Identifier)
            {
                const std::string_view spelling = mText.Spell(bodyToken.Value);
                for (usize p = 0; p < parameters.GetSize(); ++p)
                {
                    if (mText.Spell(parameters[p]) == spelling)
                    {
                        bodyToken.ParameterIndex = static_cast<uint32>(p);
                        break;
                    }
                }
            }
            body.Add(bodyToken);
        }
        if (validateBody(body, isFunctionLike) == false)
        {
            return;
        }
        uint32 macroIndex = 0;
        if (checkMacroOutcome(mMacros.TryIntern(name, nameToken, macroIndex), nameToken) == false)
        {
            return;
        }
        const MacroDefinition& existing = mMacros.Get(macroIndex);
        if (existing.ConfigurationIndex != internal::NO_CONFIGURATION)
        {
            report(DiagnosticCode::ConfigurationModified,
                   nameToken,
                   internal::ComposeMessage("'", name, "' is a configuration value and cannot be redefined"));
            return;
        }
        if (existing.IsDefined == true)
        {
            if (isSameDefinition(macroIndex, isFunctionLike, static_cast<uint32>(parameters.GetSize()), body) == false)
            {
                report(DiagnosticCode::MacroRedefined,
                       nameToken,
                       internal::ComposeMessage("'", name, "' is already defined differently; '#undef' it first"));
            }
            return;
        }
        MacroDefinition definition;
        definition.Name = nameToken;
        definition.IsDefined = true;
        definition.IsFunctionLike = isFunctionLike;
        definition.ParameterCount = static_cast<uint32>(parameters.GetSize());
        (void)checkMacroOutcome(mMacros.TryDefine(macroIndex, definition, body), nameToken);
    }

    /// Parse `(a, b)` starting at the `(` at `inoutIndex`; leaves `inoutIndex` after the `)`.
    [[nodiscard]] bool
    parseParameters(const DirectiveLine& line, usize& inoutIndex, Array<Token>& outParameters) noexcept
    {
        const Token& open = line.Get(inoutIndex);
        ++inoutIndex;
        if (inoutIndex < line.GetCount() && IsPunctuator(line.Get(inoutIndex), Punctuator::RightParenthesis) == true)
        {
            ++inoutIndex;
            return true;
        }
        while (true)
        {
            if (inoutIndex >= line.GetCount())
            {
                report(DiagnosticCode::MalformedDirective,
                       open,
                       internal::ComposeMessage("the parameter list has no closing ')'"));
                return false;
            }
            const Token& parameter = line.Get(inoutIndex);
            if (IsPunctuator(parameter, Punctuator::Ellipsis) == true)
            {
                report(DiagnosticCode::VariadicMacro,
                       parameter,
                       internal::ComposeMessage("LUSL macros are not variadic"));
                return false;
            }
            if (parameter.Kind != TokenKind::Identifier)
            {
                report(DiagnosticCode::MalformedDirective,
                       parameter,
                       internal::ComposeMessage("expected a parameter name"));
                return false;
            }
            const std::string_view spelling = mText.Spell(parameter);
            for (const Token& previous : outParameters)
            {
                if (mText.Spell(previous) == spelling)
                {
                    report(DiagnosticCode::DuplicateMacroParameter,
                           parameter,
                           internal::ComposeMessage("parameter '", spelling, "' appears twice"));
                    return false;
                }
            }
            if (outParameters.GetSize() >= MAX_MACRO_PARAMETERS)
            {
                report(DiagnosticCode::MacroLimit,
                       parameter,
                       internal::ComposeMessage("a macro has at most ",
                                                foundation::uint64{MAX_MACRO_PARAMETERS},
                                                " parameters"));
                return false;
            }
            if (outParameters.TryAdd(parameter) == false)
            {
                mSink.ReportOutOfMemory();
                return false;
            }
            ++inoutIndex;
            if (inoutIndex < line.GetCount() &&
                IsPunctuator(line.Get(inoutIndex), Punctuator::RightParenthesis) == true)
            {
                ++inoutIndex;
                return true;
            }
            if (inoutIndex < line.GetCount() && IsPunctuator(line.Get(inoutIndex), Punctuator::Comma) == true)
            {
                ++inoutIndex;
                continue;
            }
            if (inoutIndex < line.GetCount() && IsPunctuator(line.Get(inoutIndex), Punctuator::Ellipsis) == true)
            {
                report(DiagnosticCode::VariadicMacro,
                       line.Get(inoutIndex),
                       internal::ComposeMessage("LUSL macros are not variadic"));
                return false;
            }
            report(DiagnosticCode::MalformedDirective,
                   inoutIndex < line.GetCount() ? line.Get(inoutIndex) : parameter,
                   internal::ComposeMessage("expected ',' or ')' in the parameter list"));
            return false;
        }
    }

    [[nodiscard]] bool validateBody(const Array<BodyToken>& body, bool isFunctionLike) noexcept
    {
        if (body.IsEmpty() == true)
        {
            return true;
        }
        if (IsPunctuator(body.GetFirst().Value, Punctuator::HashHash) == true ||
            IsPunctuator(body.GetLast().Value, Punctuator::HashHash) == true)
        {
            const Token& at = IsPunctuator(body.GetFirst().Value, Punctuator::HashHash) == true ? body.GetFirst().Value
                                                                                                : body.GetLast().Value;
            report(DiagnosticCode::PasteAtEdge, at, internal::ComposeMessage("'##' needs an operand on both sides"));
            return false;
        }
        if (isFunctionLike == false)
        {
            return true;
        }
        for (usize i = 0; i < body.GetSize(); ++i)
        {
            if (IsPunctuator(body[i].Value, Punctuator::Hash) == true &&
                (i + 1 >= body.GetSize() || body[i + 1].ParameterIndex == internal::NO_PARAMETER))
            {
                report(DiagnosticCode::StringifyNotParameter,
                       body[i].Value,
                       internal::ComposeMessage("'#' must be followed by a macro parameter"));
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool isSameDefinition(uint32 macroIndex,
                                        bool isFunctionLike,
                                        uint32 parameterCount,
                                        const Array<BodyToken>& body) const noexcept
    {
        const MacroDefinition& existing = mMacros.Get(macroIndex);
        if (existing.IsFunctionLike != isFunctionLike || existing.ParameterCount != parameterCount ||
            existing.BodyTokenCount != body.GetSize())
        {
            return false;
        }
        for (uint32 i = 0; i < existing.BodyTokenCount; ++i)
        {
            const BodyToken& left = mMacros.GetBodyToken(macroIndex, i);
            const BodyToken& right = body[i];
            if (left.ParameterIndex != right.ParameterIndex || left.Value.Kind != right.Value.Kind ||
                left.Value.Symbol != right.Value.Symbol || mText.Spell(left.Value) != mText.Spell(right.Value) ||
                (i != 0 && left.Value.HasLeadingSpace != right.Value.HasLeadingSpace))
            {
                return false;
            }
        }
        return true;
    }

    void handleUndef(const DirectiveLine& line) noexcept
    {
        const Token& directive = line.Get(1);
        if (line.GetCount() != ONE_OPERAND_DIRECTIVE_TOKENS || line.Get(2).Kind != TokenKind::Identifier)
        {
            report(DiagnosticCode::MalformedDirective,
                   directive,
                   internal::ComposeMessage("'#undef' takes exactly one macro name"));
            return;
        }
        const Token& nameToken = line.Get(2);
        const std::string_view name = mText.Spell(nameToken);
        if (name == "defined")
        {
            report(DiagnosticCode::ReservedMacroName,
                   nameToken,
                   internal::ComposeMessage("'defined' cannot be a macro name"));
            return;
        }
        uint32 macroIndex = 0;
        if (checkMacroOutcome(mMacros.TryIntern(name, nameToken, macroIndex), nameToken) == false)
        {
            return;
        }
        if (mMacros.Get(macroIndex).ConfigurationIndex != internal::NO_CONFIGURATION)
        {
            report(DiagnosticCode::ConfigurationModified,
                   nameToken,
                   internal::ComposeMessage("'", name, "' is a configuration value and cannot be undefined"));
            return;
        }
        mMacros.Undefine(macroIndex);
    }

    [[nodiscard]] bool findFile(std::string_view path, uint16& outIndex) const noexcept
    {
        for (usize i = 0; i < mRequest.Files.GetSize(); ++i)
        {
            if (GetView(mRequest.Files[i].Path) == path)
            {
                outIndex = static_cast<uint16>(i);
                return true;
            }
        }
        return false;
    }

    void handleInclude(const DirectiveLine& line) noexcept
    {
        const Token& directive = line.Get(1);
        if (line.GetCount() != ONE_OPERAND_DIRECTIVE_TOKENS || line.Get(2).Kind != TokenKind::String)
        {
            report(DiagnosticCode::MalformedDirective,
                   directive,
                   internal::ComposeMessage("'#include' takes exactly one quoted path"));
            return;
        }
        const Token& pathToken = line.Get(2);
        const std::string_view quoted = mText.Spell(pathToken);
        const std::string_view path = quoted.substr(1, quoted.size() - 2);
        const std::string_view includingFile = GetView(mRequest.Files[line.FileIndex].Path);
        uint16 fileIndex = 0;
        const internal::IncludePathOutcome found =
            internal::FindSnapshotFile(mRequest, includingFile, path, mPathScratch, fileIndex);
        if (found == internal::IncludePathOutcome::OutOfMemory)
        {
            mSink.ReportOutOfMemory();
            return;
        }
        if (found == internal::IncludePathOutcome::Invalid)
        {
            report(DiagnosticCode::InvalidIncludePath,
                   pathToken,
                   internal::ComposeMessage("'", path, "' must be relative, use '/', and stay inside the snapshot"));
            return;
        }
        if (found == internal::IncludePathOutcome::NotFound)
        {
            report(DiagnosticCode::IncludeNotFound,
                   pathToken,
                   internal::ComposeMessage("no snapshot file matches '", path, "'"));
            return;
        }
        if (internal::HasSuffix(GetView(mRequest.Files[fileIndex].Path), HEADER_SUFFIX) == false)
        {
            report(DiagnosticCode::IncludeNotHeader,
                   pathToken,
                   internal::ComposeMessage("'",
                                            path,
                                            "' is not a .luslh header; declare Slang code with 'extern slang', and "
                                            "compile .lusl modules separately"));
            return;
        }
        const FileState& file = mFiles[fileIndex];
        if (file.HasPragmaOnce == true)
        {
            return;
        }
        if (file.IsOnStack == true)
        {
            report(DiagnosticCode::RecursiveInclude,
                   pathToken,
                   internal::ComposeMessage("'", path, "' includes itself; add '#pragma once'"));
            return;
        }
        enterFile(fileIndex, pathToken);
    }

    void handlePragma(const DirectiveLine& line) noexcept
    {
        const Token& directive = line.Get(1);
        if (line.GetCount() == ONE_OPERAND_DIRECTIVE_TOKENS && line.Get(2).Kind == TokenKind::Identifier &&
            mText.Spell(line.Get(2)) == "once")
        {
            mFiles[line.FileIndex].HasPragmaOnce = true;
            return;
        }
        report(DiagnosticCode::UnknownPragma,
               directive,
               internal::ComposeMessage("'#pragma once' is the only LUSL pragma"));
    }

    void handleError(const DirectiveLine& line) noexcept
    {
        mPathScratch.Clear();
        bool isOk = true;
        for (usize i = 2; i < line.GetCount() && isOk == true; ++i)
        {
            const Token& token = line.Get(i);
            if (i != 2 && token.HasLeadingSpace == true)
            {
                isOk = mPathScratch.TryAdd(' ');
            }
            const std::string_view spelling = mText.Spell(token);
            isOk = isOk == true &&
                   mPathScratch.TryAddRange(foundation::BorrowedView<const char>(spelling.data(), spelling.size()));
        }
        if (isOk == false)
        {
            mSink.ReportOutOfMemory();
            return;
        }
        report(DiagnosticCode::ErrorDirective,
               line.Get(1),
               internal::ComposeMessage("#error ", std::string_view(mPathScratch.GetData(), mPathScratch.GetSize())));
    }

    // ----- Reporting ------------------------------------------------------------------------------

    [[nodiscard]] SourceLocation getExpansionLocation(const Token& token) const noexcept
    {
        if (token.ExpansionIndex == NO_EXPANSION || token.ExpansionIndex >= mResult.Expansions.GetSize())
        {
            return SourceLocation{};
        }
        return mResult.Expansions[token.ExpansionIndex];
    }

    void report(DiagnosticCode code, const Token& token, const DiagnosticMessage& message) noexcept
    {
        mSink.ReportError(code, token.Location, getExpansionLocation(token), message);
    }

    [[nodiscard]] bool checkMacroOutcome(MacroTableOutcome outcome, const Token& name) noexcept
    {
        if (outcome == MacroTableOutcome::Ok)
        {
            return true;
        }
        if (outcome == MacroTableOutcome::OutOfMemory)
        {
            mSink.ReportOutOfMemory();
            return false;
        }
        report(DiagnosticCode::MacroLimit,
               name,
               internal::ComposeMessage("more than ", foundation::uint64{MAX_MACROS}, " macro names"));
        mIsFatal = true;
        return false;
    }

    [[nodiscard]] Status finish() noexcept
    {
        Status outcome = Status::Ready;
        if (mSink.HasOutOfMemory() == true)
        {
            outcome = Status::OutOfMemory;
        }
        else if (mIsInvalidRequest == true)
        {
            outcome = Status::InvalidRequest;
        }
        else if (mSink.HasErrors() == true)
        {
            outcome = Status::Failed;
        }
        if (outcome != Status::Ready)
        {
            mResult.Tokens.Clear();
            mResult.SynthesizedText.Clear();
            mResult.Expansions.Clear();
        }
        mResult.Outcome = outcome;
        return outcome;
    }

    const PreprocessRequest& mRequest;
    PreprocessResult& mResult;
    DiagnosticSink mSink;
    internal::TokenText mText;
    internal::MacroTable mMacros;
    internal::HideSetTable mHideSets;
    internal::MacroExpander mExpander;
    Array<FileState> mFiles;
    Array<IncludeFrame> mIncludes;
    Array<ConditionalFrame> mConditionals;
    Array<char> mPathScratch;
    uint16 mRootIndex = 0;
    bool mIsFatal = false;
    bool mIsInvalidRequest = false;
};
} // namespace

Status Preprocess(const PreprocessRequest& request, PreprocessResult& outResult) noexcept
{
    outResult = PreprocessResult{};
    Preprocessor preprocessor(request, outResult);
    return preprocessor.Run();
}

std::string_view
GetTokenSpelling(const PreprocessRequest& request, const PreprocessResult& result, const Token& token) noexcept
{
    return internal::SpellToken(request.Files,
                                std::string_view(result.SynthesizedText.GetData(), result.SynthesizedText.GetSize()),
                                token);
}
} // namespace ludus::tooling::shader_language
