// Macro expansion follows Dave Prosser's hide-set algorithm, written for the ANSI C committee:
// "Complete macro expansion algorithm", X3J11/86-196, 4 December 1986
// (https://www.spinellis.gr/blog/20060626/x3J11-86-196.pdf). We adopt its token hide sets,
// the intersection rule for function-like invocations and argument pre-expansion; empty `##`
// operands use the C99 placemarkers (ISO/IEC 9899:1999, 6.10.3.3) instead of the original
// special cases. The recursive formulation is replaced by explicit pending stacks.

#include "internal/macro_expander.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

#include "internal/array_growth.hpp"
#include "internal/lexer.hpp"

#include <span>

namespace ludus::tooling::shader_language::internal
{
using foundation::Array;
using foundation::uint32;
using foundation::usize;

namespace
{
/// Number of distinct incoming hide sets remembered while one replacement is finalized.
constexpr usize UNION_CACHE_ENTRIES = 8;

[[nodiscard]] bool IsPunctuator(const Token& token, Punctuator symbol) noexcept
{
    return token.Kind == TokenKind::Punctuator && token.Symbol == symbol;
}
} // namespace

/// Tokens still to scan, in reverse order, and optionally the file they continue into.
struct MacroExpander::Frame final
{
    Array<WorkToken> Pending;
    const Array<Token>* FileTokensOrNull = nullptr;
    usize* FilePositionOrNull = nullptr;
};

MacroExpander::MacroExpander(const MacroTable& macros,
                             HideSetTable& inoutHideSets,
                             TokenText& inoutText,
                             DiagnosticSink& inoutSink,
                             const ExpansionOutputs& outputs) noexcept
    : mMacros(macros), mHideSets(inoutHideSets), mText(inoutText), mSink(inoutSink), mOutputs(outputs)
{
}

bool MacroExpander::ExpandFileText(const Array<Token>& tokens, usize& inoutPosition) noexcept
{
    Frame frame;
    frame.FileTokensOrNull = &tokens;
    frame.FilePositionOrNull = &inoutPosition;
    return runFrame(frame, nullptr, 0);
}

bool MacroExpander::ExpandIsolated(const Array<WorkToken>& input, Array<WorkToken>& outTokens) noexcept
{
    Frame frame;
    if (frame.Pending.TryEnsureCapacity(input.GetSize()) == false)
    {
        return failOutOfMemory();
    }
    for (usize i = input.GetSize(); i > 0; --i)
    {
        frame.Pending.Add(input[i - 1]);
    }
    return runFrame(frame, &outTokens, 0);
}

bool MacroExpander::runFrame(Frame& inoutFrame, Array<WorkToken>* nestedOutputOrNull, uint32 depth) noexcept
{
    WorkToken token;
    while (mIsFatal == false && readToken(inoutFrame, true, token) == ReadResult::Token)
    {
        if (processToken(inoutFrame, token, nestedOutputOrNull, depth) == false)
        {
            return false;
        }
    }
    return mIsFatal == false;
}

MacroExpander::ReadResult
MacroExpander::readToken(Frame& inoutFrame, bool isConsuming, WorkToken& outToken) const noexcept
{
    if (inoutFrame.Pending.IsEmpty() == false)
    {
        outToken = inoutFrame.Pending.GetLast();
        if (isConsuming == true)
        {
            inoutFrame.Pending.RemoveLast();
        }
        return ReadResult::Token;
    }
    if (inoutFrame.FileTokensOrNull == nullptr || inoutFrame.FilePositionOrNull == nullptr)
    {
        return ReadResult::End;
    }
    const Array<Token>& tokens = *inoutFrame.FileTokensOrNull;
    usize& position = *inoutFrame.FilePositionOrNull;
    if (position >= tokens.GetSize())
    {
        return ReadResult::End;
    }
    const Token& next = tokens[position];
    if (next.IsAtLineStart == true && IsPunctuator(next, Punctuator::Hash) == true)
    {
        return ReadResult::Directive;
    }
    outToken = WorkToken{};
    outToken.Value = next;
    if (isConsuming == true)
    {
        ++position;
    }
    return ReadResult::Token;
}

bool MacroExpander::processToken(Frame& inoutFrame,
                                 const WorkToken& token,
                                 Array<WorkToken>* nestedOutputOrNull,
                                 uint32 depth) noexcept
{
    if (token.IsPlacemarker == true)
    {
        return true;
    }
    if (token.Value.Kind != TokenKind::Identifier)
    {
        return emitToken(token, nestedOutputOrNull);
    }
    const uint32 macroIndex = mMacros.Find(mText.Spell(token.Value));
    if (macroIndex == NO_MACRO || mMacros.Get(macroIndex).IsDefined == false ||
        mHideSets.Contains(token.HideSet, macroIndex) == true)
    {
        return emitToken(token, nestedOutputOrNull);
    }
    const MacroDefinition& definition = mMacros.Get(macroIndex);
    Array<WorkToken> arguments;
    Array<ArgumentRange> ranges;
    uint32 hideSet = HideSetTable::EMPTY_HIDE_SET;
    if (definition.IsFunctionLike == true)
    {
        WorkToken next;
        if (readToken(inoutFrame, false, next) != ReadResult::Token ||
            IsPunctuator(next.Value, Punctuator::LeftParenthesis) == false)
        {
            // A function-like macro name without arguments is an ordinary identifier.
            return emitToken(token, nestedOutputOrNull);
        }
        (void)readToken(inoutFrame, true, next);
        WorkToken close;
        bool isValid = false;
        if (collectArguments(inoutFrame, token, definition, arguments, ranges, close, isValid) == false)
        {
            return false;
        }
        if (isValid == false)
        {
            return true;
        }
        uint32 shared = HideSetTable::EMPTY_HIDE_SET;
        if (checkHideSet(mHideSets.TryIntersect(token.HideSet, close.HideSet, shared), token) == false ||
            checkHideSet(mHideSets.TryInsert(shared, macroIndex, hideSet), token) == false)
        {
            return false;
        }
    }
    else if (checkHideSet(mHideSets.TryInsert(token.HideSet, macroIndex, hideSet), token) == false)
    {
        return false;
    }

    ++mExpansionCount;
    if (mExpansionCount > MAX_MACRO_EXPANSIONS)
    {
        return fail(DiagnosticCode::MacroExpansionLimit,
                    token.Value,
                    ComposeMessage("more than ", foundation::uint64{MAX_MACRO_EXPANSIONS}, " macro expansions"));
    }
    if (definition.ConfigurationIndex != NO_CONFIGURATION)
    {
        mOutputs.ConfigurationReads[definition.ConfigurationIndex] = 1;
    }
    uint32 expansionIndex = token.Value.ExpansionIndex;
    if (expansionIndex == NO_EXPANSION)
    {
        if (mOutputs.Expansions.TryAdd(token.Value.Location) == false)
        {
            return failOutOfMemory();
        }
        expansionIndex = static_cast<uint32>(mOutputs.Expansions.GetSize() - 1);
    }
    Array<WorkToken> replacement;
    const Invocation invocation{macroIndex, hideSet, expansionIndex, depth};
    if (substitute(invocation, token, arguments, ranges, replacement) == false)
    {
        return false;
    }
    if (replacement.GetSize() > MAX_OUTPUT_TOKENS - inoutFrame.Pending.GetSize())
    {
        return fail(
            DiagnosticCode::MacroExpansionLimit,
            token.Value,
            ComposeMessage("macro expansion produces more than ", foundation::uint64{MAX_OUTPUT_TOKENS}, " tokens"));
    }
    if (TryReserveGrowing(inoutFrame.Pending, inoutFrame.Pending.GetSize() + replacement.GetSize()) == false)
    {
        return failOutOfMemory();
    }
    for (usize i = replacement.GetSize(); i > 0; --i)
    {
        inoutFrame.Pending.Add(replacement[i - 1]);
    }
    return true;
}

bool MacroExpander::emitToken(const WorkToken& token, Array<WorkToken>* nestedOutputOrNull) noexcept
{
    if (nestedOutputOrNull != nullptr)
    {
        return nestedOutputOrNull->TryAdd(token) == true ? true : failOutOfMemory();
    }
    if (mOutputs.Tokens.GetSize() >= MAX_OUTPUT_TOKENS)
    {
        return fail(DiagnosticCode::OutputTokenLimit,
                    token.Value,
                    ComposeMessage("the translation unit exceeds ", foundation::uint64{MAX_OUTPUT_TOKENS}, " tokens"));
    }
    return mOutputs.Tokens.TryAdd(token.Value) == true ? true : failOutOfMemory();
}

bool MacroExpander::collectArguments(Frame& inoutFrame,
                                     const WorkToken& name,
                                     const MacroDefinition& definition,
                                     Array<WorkToken>& outArguments,
                                     Array<ArgumentRange>& outRanges,
                                     WorkToken& outClose,
                                     bool& outIsValid) noexcept
{
    outIsValid = false;
    uint32 nesting = 0;
    ArgumentRange current;
    WorkToken token;
    while (true)
    {
        const ReadResult read = readToken(inoutFrame, true, token);
        if (read == ReadResult::End)
        {
            report(DiagnosticCode::UnterminatedMacroInvocation,
                   name.Value,
                   ComposeMessage("'", mText.Spell(name.Value), "' has no closing ')' before the end of its file"));
            return true;
        }
        if (read == ReadResult::Directive)
        {
            report(DiagnosticCode::DirectiveInMacroArguments,
                   name.Value,
                   ComposeMessage("a directive interrupts the arguments of '", mText.Spell(name.Value), "'"));
            return true;
        }
        if (IsPunctuator(token.Value, Punctuator::RightParenthesis) == true && nesting == 0)
        {
            break;
        }
        if (IsPunctuator(token.Value, Punctuator::Comma) == true && nesting == 0)
        {
            if (outRanges.TryAdd(current) == false)
            {
                return failOutOfMemory();
            }
            current = ArgumentRange{static_cast<uint32>(outArguments.GetSize()), 0};
            continue;
        }
        if (IsPunctuator(token.Value, Punctuator::LeftParenthesis) == true)
        {
            ++nesting;
        }
        else if (IsPunctuator(token.Value, Punctuator::RightParenthesis) == true)
        {
            --nesting;
        }
        if (outArguments.GetSize() >= MAX_OUTPUT_TOKENS)
        {
            return fail(DiagnosticCode::MacroExpansionLimit,
                        name.Value,
                        ComposeMessage("macro arguments exceed ", foundation::uint64{MAX_OUTPUT_TOKENS}, " tokens"));
        }
        if (outArguments.TryAdd(token) == false)
        {
            return failOutOfMemory();
        }
        ++current.Count;
    }
    if (outRanges.TryAdd(current) == false)
    {
        return failOutOfMemory();
    }
    outClose = token;
    const bool isEmptyCall = outRanges.GetSize() == 1 && outRanges[0].Count == 0;
    const usize argumentCount = isEmptyCall == true && definition.ParameterCount == 0 ? 0 : outRanges.GetSize();
    if (argumentCount != definition.ParameterCount)
    {
        report(DiagnosticCode::MacroArgumentCount,
               name.Value,
               ComposeMessage("'",
                              mText.Spell(name.Value),
                              "' expects ",
                              foundation::uint64{definition.ParameterCount},
                              " arguments, not ",
                              foundation::uint64{argumentCount}));
        return true;
    }
    outIsValid = true;
    return true;
}

bool MacroExpander::substitute(const Invocation& invocation,
                               const WorkToken& name,
                               const Array<WorkToken>& arguments,
                               const Array<ArgumentRange>& ranges,
                               Array<WorkToken>& outReplacement) noexcept
{
    const uint32 macroIndex = invocation.MacroIndex;
    const uint32 hideSet = invocation.HideSet;
    const uint32 expansionIndex = invocation.ExpansionIndex;
    const uint32 depth = invocation.Depth;
    const MacroDefinition& definition = mMacros.Get(macroIndex);
    Array<WorkToken> output;
    // Fully expanded arguments, computed on first use; argument p occupies expandedRanges[p].
    Array<WorkToken> expandedStorage;
    Array<ArgumentRange> expandedRanges;
    Array<foundation::uint8> isExpanded;
    if (expandedRanges.TryResize(definition.ParameterCount) == false ||
        isExpanded.TryResize(definition.ParameterCount, 0) == false)
    {
        return failOutOfMemory();
    }
    uint32 i = 0;
    while (i < definition.BodyTokenCount)
    {
        const BodyToken& body = mMacros.GetBodyToken(macroIndex, i);
        const bool hasNext = i + 1 < definition.BodyTokenCount;
        const bool isNextPaste = hasNext == true && IsPunctuator(mMacros.GetBodyToken(macroIndex, i + 1).Value,
                                                                 Punctuator::HashHash) == true;
        if (definition.IsFunctionLike == true && IsPunctuator(body.Value, Punctuator::Hash) == true && hasNext == true)
        {
            // The definition guarantees that a parameter follows '#'.
            const uint32 parameter = mMacros.GetBodyToken(macroIndex, i + 1).ParameterIndex;
            WorkToken text;
            if (stringize(arguments, ranges[parameter], body.Value, text) == false || output.TryAdd(text) == false)
            {
                return mIsFatal == true ? false : failOutOfMemory();
            }
            i += 2;
            continue;
        }
        if (IsPunctuator(body.Value, Punctuator::HashHash) == true && hasNext == true)
        {
            const BodyToken& right = mMacros.GetBodyToken(macroIndex, i + 1);
            if (right.ParameterIndex != NO_PARAMETER)
            {
                const ArgumentRange& range = ranges[right.ParameterIndex];
                if (range.Count != 0)
                {
                    if (paste(output, arguments[range.First], body.Value, expansionIndex) == false)
                    {
                        return false;
                    }
                    for (uint32 k = 1; k < range.Count; ++k)
                    {
                        if (output.TryAdd(arguments[range.First + k]) == false)
                        {
                            return failOutOfMemory();
                        }
                    }
                }
            }
            else
            {
                WorkToken operand;
                operand.Value = right.Value;
                if (paste(output, operand, body.Value, expansionIndex) == false)
                {
                    return false;
                }
            }
            i += 2;
            continue;
        }
        if (body.ParameterIndex != NO_PARAMETER)
        {
            const ArgumentRange& range = ranges[body.ParameterIndex];
            if (isNextPaste == true)
            {
                // An operand of '##' is substituted without expansion.
                if (range.Count == 0)
                {
                    WorkToken placemarker;
                    placemarker.Value = body.Value;
                    placemarker.IsPlacemarker = true;
                    if (output.TryAdd(placemarker) == false)
                    {
                        return failOutOfMemory();
                    }
                }
                for (uint32 k = 0; k < range.Count; ++k)
                {
                    if (output.TryAdd(arguments[range.First + k]) == false)
                    {
                        return failOutOfMemory();
                    }
                }
            }
            else
            {
                if (isExpanded[body.ParameterIndex] == 0)
                {
                    const uint32 first = static_cast<uint32>(expandedStorage.GetSize());
                    if (expandArgument(arguments, range, depth + 1, expandedStorage) == false)
                    {
                        return false;
                    }
                    expandedRanges[body.ParameterIndex] =
                        ArgumentRange{first, static_cast<uint32>(expandedStorage.GetSize()) - first};
                    isExpanded[body.ParameterIndex] = 1;
                }
                const ArgumentRange& expanded = expandedRanges[body.ParameterIndex];
                for (uint32 k = 0; k < expanded.Count; ++k)
                {
                    if (output.TryAdd(expandedStorage[expanded.First + k]) == false)
                    {
                        return failOutOfMemory();
                    }
                }
            }
            ++i;
            continue;
        }
        WorkToken plain;
        plain.Value = body.Value;
        if (output.TryAdd(plain) == false)
        {
            return failOutOfMemory();
        }
        ++i;
    }

    // Add the invocation's hide set to every produced token and attribute it to the outermost use.
    struct UnionCacheEntry final
    {
        uint32 Incoming = 0;
        uint32 Result = 0;
    };
    foundation::StaticArray<UnionCacheEntry, UNION_CACHE_ENTRIES> cache{};
    usize cacheSize = 0;
    if (outReplacement.TryEnsureCapacity(output.GetSize()) == false)
    {
        return failOutOfMemory();
    }
    for (const WorkToken& produced : output)
    {
        if (produced.IsPlacemarker == true)
        {
            continue;
        }
        WorkToken finished = produced;
        bool isCached = false;
        for (usize k = 0; k < cacheSize; ++k)
        {
            if (cache[k].Incoming == produced.HideSet)
            {
                finished.HideSet = cache[k].Result;
                isCached = true;
                break;
            }
        }
        if (isCached == false)
        {
            if (checkHideSet(mHideSets.TryUnite(produced.HideSet, hideSet, finished.HideSet), name) == false)
            {
                return false;
            }
            cache[cacheSize % UNION_CACHE_ENTRIES] = UnionCacheEntry{produced.HideSet, finished.HideSet};
            cacheSize = cacheSize < UNION_CACHE_ENTRIES ? cacheSize + 1 : cacheSize;
        }
        finished.Value.ExpansionIndex = expansionIndex;
        if (outReplacement.IsEmpty() == true)
        {
            finished.Value.IsAtLineStart = name.Value.IsAtLineStart;
            finished.Value.HasLeadingSpace = name.Value.HasLeadingSpace;
        }
        outReplacement.Add(finished);
    }
    return true;
}

bool MacroExpander::stringize(const Array<WorkToken>& arguments,
                              const ArgumentRange& range,
                              const Token& operatorToken,
                              WorkToken& outToken) noexcept
{
    mPasteScratch.Clear();
    bool isOk = mPasteScratch.TryAdd('"');
    for (uint32 k = 0; k < range.Count && isOk == true; ++k)
    {
        const Token& part = arguments[range.First + k].Value;
        if (k != 0 && part.HasLeadingSpace == true)
        {
            isOk = mPasteScratch.TryAdd(' ');
        }
        const std::string_view spelling = mText.Spell(part);
        for (const char value : spelling)
        {
            if (part.Kind == TokenKind::String && (value == '"' || value == '\\'))
            {
                isOk = isOk == true && mPasteScratch.TryAdd('\\');
            }
            isOk = isOk == true && mPasteScratch.TryAdd(value);
        }
    }
    isOk = isOk == true && mPasteScratch.TryAdd('"');
    if (isOk == false)
    {
        return failOutOfMemory();
    }
    uint32 offset = 0;
    WorkToken located;
    located.Value = operatorToken;
    if (checkText(mText.TryAppend(std::string_view(mPasteScratch.GetData(), mPasteScratch.GetSize()), offset),
                  located) == false)
    {
        return false;
    }
    outToken = WorkToken{};
    outToken.Value = operatorToken;
    outToken.Value.Kind = TokenKind::String;
    outToken.Value.Symbol = Punctuator::None;
    outToken.Value.IsSynthesized = true;
    outToken.Value.SpellingOffset = offset;
    outToken.Value.SpellingSize = static_cast<uint32>(mPasteScratch.GetSize());
    return true;
}

bool MacroExpander::paste(Array<WorkToken>& inoutOutput,
                          const WorkToken& right,
                          const Token& operatorToken,
                          uint32 expansionIndex) noexcept
{
    if (inoutOutput.IsEmpty() == true)
    {
        return inoutOutput.TryAdd(right) == true ? true : failOutOfMemory();
    }
    WorkToken& left = inoutOutput.GetLast();
    if (left.IsPlacemarker == true)
    {
        left = right;
        return true;
    }
    if (right.IsPlacemarker == true)
    {
        return true;
    }
    const std::string_view leftSpelling = mText.Spell(left.Value);
    const std::string_view rightSpelling = mText.Spell(right.Value);
    mPasteScratch.Clear();
    if (mPasteScratch.TryAddRange(std::span<const char>(leftSpelling.data(), leftSpelling.size())) == false ||
        mPasteScratch.TryAddRange(std::span<const char>(rightSpelling.data(), rightSpelling.size())) == false)
    {
        return failOutOfMemory();
    }
    uint32 offset = 0;
    if (checkText(mText.TryAppend(std::string_view(mPasteScratch.GetData(), mPasteScratch.GetSize()), offset), left) ==
        false)
    {
        return false;
    }
    LexInput input;
    input.Text = mText.GetSynthesized().substr(offset, mPasteScratch.GetSize());
    input.FileIndex = left.Value.Location.FileIndex;
    input.IsSynthesized = true;
    input.SpellingBase = offset;
    Array<Diagnostic> scratchDiagnostics;
    usize scratchDropped = 0;
    DiagnosticSink scratchSink(scratchDiagnostics, scratchDropped);
    mPasteTokens.Clear();
    const LexOutcome lexed = LexText(input, mPasteTokens, scratchSink);
    if (lexed == LexOutcome::OutOfMemory || scratchSink.HasOutOfMemory() == true)
    {
        return failOutOfMemory();
    }
    if (lexed != LexOutcome::Complete || mPasteTokens.GetSize() != 1 ||
        mPasteTokens[0].SpellingSize != mPasteScratch.GetSize())
    {
        // Point at the '##' in the definition, expanded from the outermost macro use.
        Token located = operatorToken;
        located.ExpansionIndex = expansionIndex;
        report(DiagnosticCode::InvalidPaste,
               located,
               ComposeMessage("'##' joins '",
                              std::string_view(mPasteScratch.GetData(), mPasteScratch.GetSize()),
                              "', which is not one token"));
        return inoutOutput.TryAdd(right) == true ? true : failOutOfMemory();
    }
    uint32 shared = HideSetTable::EMPTY_HIDE_SET;
    if (checkHideSet(mHideSets.TryIntersect(left.HideSet, right.HideSet, shared), left) == false)
    {
        return false;
    }
    const Token& joined = mPasteTokens[0];
    left.Value.Kind = joined.Kind;
    left.Value.Symbol = joined.Symbol;
    left.Value.IsSynthesized = true;
    left.Value.SpellingOffset = joined.SpellingOffset;
    left.Value.SpellingSize = joined.SpellingSize;
    left.HideSet = shared;
    return true;
}

bool MacroExpander::expandArgument(const Array<WorkToken>& arguments,
                                   const ArgumentRange& range,
                                   uint32 depth,
                                   Array<WorkToken>& outTokens) noexcept
{
    if (depth > MAX_MACRO_EXPANSION_DEPTH)
    {
        const Token& at = range.Count != 0 ? arguments[range.First].Value : Token{};
        return fail(DiagnosticCode::MacroExpansionLimit,
                    at,
                    ComposeMessage("macro arguments nest deeper than ", foundation::uint64{MAX_MACRO_EXPANSION_DEPTH}));
    }
    Frame frame;
    if (frame.Pending.TryEnsureCapacity(range.Count) == false)
    {
        return failOutOfMemory();
    }
    for (uint32 k = range.Count; k > 0; --k)
    {
        frame.Pending.Add(arguments[range.First + k - 1]);
    }
    return runFrame(frame, &outTokens, depth);
}

bool MacroExpander::checkHideSet(HideSetOutcome outcome, const WorkToken& token) noexcept
{
    if (outcome == HideSetOutcome::Ok)
    {
        return true;
    }
    if (outcome == HideSetOutcome::OutOfMemory)
    {
        return failOutOfMemory();
    }
    return fail(DiagnosticCode::MacroExpansionLimit, token.Value, ComposeMessage("macro expansion is too complex"));
}

bool MacroExpander::checkText(TextOutcome outcome, const WorkToken& token) noexcept
{
    if (outcome == TextOutcome::Ok)
    {
        return true;
    }
    if (outcome == TextOutcome::OutOfMemory)
    {
        return failOutOfMemory();
    }
    return fail(DiagnosticCode::MacroExpansionLimit,
                token.Value,
                ComposeMessage("'#' and '##' create more than ", foundation::uint64{MAX_SYNTHESIZED_BYTES}, " bytes"));
}

SourceLocation MacroExpander::getExpansionLocation(const Token& token) const noexcept
{
    if (token.ExpansionIndex == NO_EXPANSION || token.ExpansionIndex >= mOutputs.Expansions.GetSize())
    {
        return SourceLocation{};
    }
    return mOutputs.Expansions[token.ExpansionIndex];
}

void MacroExpander::report(DiagnosticCode code, const Token& token, const DiagnosticMessage& message) noexcept
{
    mSink.ReportError(code, token.Location, getExpansionLocation(token), message);
}

bool MacroExpander::fail(DiagnosticCode code, const Token& token, const DiagnosticMessage& message) noexcept
{
    report(code, token, message);
    mIsFatal = true;
    return false;
}

bool MacroExpander::failOutOfMemory() noexcept
{
    mSink.ReportOutOfMemory();
    mIsFatal = true;
    return false;
}
} // namespace ludus::tooling::shader_language::internal
