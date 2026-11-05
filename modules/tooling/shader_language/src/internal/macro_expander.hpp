#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/token.hpp>

#include "diagnostic_sink.hpp"
#include "hide_set.hpp"
#include "macro_table.hpp"
#include "token_text.hpp"

namespace ludus::tooling::shader_language::internal
{
/// A token during expansion: its hide set and whether it is a `##` placemarker.
struct WorkToken final
{
    /// The token itself.
    Token Value;
    /// Macros that may not expand this token again (HideSetTable identifier).
    foundation::uint32 HideSet = HideSetTable::EMPTY_HIDE_SET;
    /// True for the placeholder an empty argument leaves next to `##`; never output.
    bool IsPlacemarker = false;
};

/// Shared output and bookkeeping that expansion writes to.
struct ExpansionOutputs final
{
    /// Top-level preprocessed tokens.
    foundation::Array<Token>& Tokens;
    /// Outermost macro use locations, indexed by Token::ExpansionIndex.
    foundation::Array<SourceLocation>& Expansions;
    /// One byte per configuration value, set when the value is expanded.
    foundation::Array<foundation::uint8>& ConfigurationReads;
};

/// @brief Macro replacement with C rescanning semantics.
///
/// Follows Dave Prosser's hide-set algorithm for ANSI C macro expansion: every token carries the
/// set of macros whose expansion produced it, and a macro name is not expanded again while it is in
/// its own token's hide set. Placemarkers implement empty `##` operands as C99 specifies.
class MacroExpander final
{
public:
    /// Borrow the collaborators; they must outlive the expander.
    MacroExpander(const MacroTable& macros,
                  HideSetTable& inoutHideSets,
                  TokenText& inoutText,
                  DiagnosticSink& inoutSink,
                  const ExpansionOutputs& outputs) noexcept;

    /// @brief Expand file tokens from `inoutPosition` up to the next directive line or the end.
    /// @return False after a fatal limit or allocation failure; processing must stop.
    [[nodiscard]] bool ExpandFileText(const foundation::Array<Token>& tokens,
                                      foundation::usize& inoutPosition) noexcept;

    /// @brief Fully expand `input`, which cannot read past its end, into `outTokens`.
    /// @return False after a fatal limit or allocation failure.
    [[nodiscard]] bool ExpandIsolated(const foundation::Array<WorkToken>& input,
                                      foundation::Array<WorkToken>& outTokens) noexcept;

private:
    struct Frame;
    struct ArgumentRange final
    {
        foundation::uint32 First = 0;
        foundation::uint32 Count = 0;
    };
    /// One macro invocation being substituted.
    struct Invocation final
    {
        foundation::uint32 MacroIndex = 0;     ///< The invoked macro.
        foundation::uint32 HideSet = 0;        ///< Hide set added to every produced token.
        foundation::uint32 ExpansionIndex = 0; ///< Outermost macro use of the produced tokens.
        foundation::uint32 Depth = 0;          ///< Argument-expansion nesting of the invocation.
    };
    enum class ReadResult : foundation::uint8
    {
        Token,
        End,
        Directive,
    };

    [[nodiscard]] bool
    runFrame(Frame& inoutFrame, foundation::Array<WorkToken>* nestedOutputOrNull, foundation::uint32 depth) noexcept;
    [[nodiscard]] ReadResult readToken(Frame& inoutFrame, bool isConsuming, WorkToken& outToken) const noexcept;
    [[nodiscard]] bool processToken(Frame& inoutFrame,
                                    const WorkToken& token,
                                    foundation::Array<WorkToken>* nestedOutputOrNull,
                                    foundation::uint32 depth) noexcept;
    [[nodiscard]] bool emitToken(const WorkToken& token, foundation::Array<WorkToken>* nestedOutputOrNull) noexcept;
    [[nodiscard]] bool collectArguments(Frame& inoutFrame,
                                        const WorkToken& name,
                                        const MacroDefinition& definition,
                                        foundation::Array<WorkToken>& outArguments,
                                        foundation::Array<ArgumentRange>& outRanges,
                                        WorkToken& outClose,
                                        bool& outIsValid) noexcept;
    [[nodiscard]] bool substitute(const Invocation& invocation,
                                  const WorkToken& name,
                                  const foundation::Array<WorkToken>& arguments,
                                  const foundation::Array<ArgumentRange>& ranges,
                                  foundation::Array<WorkToken>& outReplacement) noexcept;
    [[nodiscard]] bool stringize(const foundation::Array<WorkToken>& arguments,
                                 const ArgumentRange& range,
                                 const Token& operatorToken,
                                 WorkToken& outToken) noexcept;
    [[nodiscard]] bool paste(foundation::Array<WorkToken>& inoutOutput,
                             const WorkToken& right,
                             const Token& operatorToken,
                             foundation::uint32 expansionIndex) noexcept;
    [[nodiscard]] bool expandArgument(const foundation::Array<WorkToken>& arguments,
                                      const ArgumentRange& range,
                                      foundation::uint32 depth,
                                      foundation::Array<WorkToken>& outTokens) noexcept;
    [[nodiscard]] bool checkHideSet(HideSetOutcome outcome, const WorkToken& token) noexcept;
    [[nodiscard]] bool checkText(TextOutcome outcome, const WorkToken& token) noexcept;
    [[nodiscard]] SourceLocation getExpansionLocation(const Token& token) const noexcept;
    void report(DiagnosticCode code, const Token& token, const DiagnosticMessage& message) noexcept;
    [[nodiscard]] bool fail(DiagnosticCode code, const Token& token, const DiagnosticMessage& message) noexcept;
    [[nodiscard]] bool failOutOfMemory() noexcept;

    const MacroTable& mMacros;
    HideSetTable& mHideSets;
    TokenText& mText;
    DiagnosticSink& mSink;
    ExpansionOutputs mOutputs;
    foundation::usize mExpansionCount = 0;
    foundation::Array<char> mPasteScratch;
    foundation::Array<Token> mPasteTokens;
    bool mIsFatal = false;
};
} // namespace ludus::tooling::shader_language::internal
