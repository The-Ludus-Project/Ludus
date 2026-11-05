#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

#include <string_view>

/// @file
/// Structured LUSL diagnostics: stable codes, severities and source locations.
namespace ludus::tooling::shader_language
{
/// @brief Stable LUSL diagnostic code, displayed as `LUSL` followed by four digits (`LUSL0201`).
///
/// Values never change meaning once released; retired codes are not reused. Codes 1-99 are
/// language rules shared by every stage, 100-199 are lexical and 200-299 are preprocessor
/// diagnostics. docs/modules/tooling/shader-language.md lists every code.
// NOLINTNEXTLINE(performance-enum-size): codes have four digits; later stages exceed 255.
enum class DiagnosticCode : foundation::uint16
{
    /// Unary `!` does not exist in LUSL; compare with `== false`, or with `== 0` in `#if`.
    LogicalNot = 1,
    /// The request is malformed: a path, the root file, a configuration value or a snapshot limit.
    InvalidRequest = 2,

    /// A file is not well-formed UTF-8.
    InvalidUtf8 = 101,
    /// A byte that cannot start a token, such as `@`, `$`, a backtick or non-ASCII text outside a
    /// comment or string.
    InvalidCharacter = 102,
    /// A `/*` comment has no closing `*/`.
    UnterminatedComment = 103,
    /// A string literal has no closing quote on its line.
    UnterminatedString = 104,
    /// A string literal contains an escape other than `\\`, `\"`, `\n`, `\r` or `\t`.
    InvalidEscape = 105,
    /// A character literal such as `'a'`; LUSL has no character type.
    CharacterLiteral = 106,
    /// A file or configuration value lexes to more than MAX_FILE_TOKENS tokens.
    FileTokenLimit = 107,

    /// A directive name that LUSL does not define.
    UnknownDirective = 201,
    /// A directive is missing a required part or has extra tokens.
    MalformedDirective = 202,
    /// No snapshot file matches an `#include` path.
    IncludeNotFound = 203,
    /// An `#include` names a file that is not a `.luslh` header.
    IncludeNotHeader = 204,
    /// An `#include` path is absolute, contains a backslash or empty component, or leaves the
    /// snapshot root.
    InvalidIncludePath = 205,
    /// A file includes itself, directly or indirectly, without `#pragma once`.
    RecursiveInclude = 206,
    /// `#include` nesting exceeds MAX_INCLUDE_DEPTH.
    IncludeDepthLimit = 207,
    /// A macro is redefined with a different kind, parameter list or body.
    MacroRedefined = 208,
    /// A configuration value is redefined or undefined by `#define` or `#undef`.
    ConfigurationModified = 209,
    /// `defined` cannot be a macro name.
    ReservedMacroName = 210,
    /// A function-like macro declares `...`; LUSL macros are not variadic.
    VariadicMacro = 211,
    /// A function-like macro repeats a parameter name.
    DuplicateMacroParameter = 212,
    /// `#` in a function-like macro body is not followed by a parameter.
    StringifyNotParameter = 213,
    /// `##` begins or ends a macro body.
    PasteAtEdge = 214,
    /// `##` joins two tokens whose combined spelling is not exactly one token.
    InvalidPaste = 215,
    /// A function-like macro invocation has the wrong number of arguments.
    MacroArgumentCount = 216,
    /// A function-like macro invocation reaches the end of its file without a closing `)`.
    UnterminatedMacroInvocation = 217,
    /// A directive line appears inside the arguments of a function-like macro invocation.
    DirectiveInMacroArguments = 218,
    /// An identifier remains in an `#if` or `#elif` expression after macro expansion. C treats it
    /// as zero; LUSL rejects it so misspelled names are caught.
    UndefinedIdentifierInCondition = 219,
    /// `defined`, `#ifdef` or `#ifndef` tests a name that is neither a configuration value nor a
    /// macro defined or undefined earlier in the translation unit.
    UnknownMacroName = 220,
    /// An `#if` or `#elif` expression is not a valid integer constant expression.
    InvalidCondition = 221,
    /// An evaluated `#if` or `#elif` operand divides by zero.
    ConditionDivisionByZero = 222,
    /// An evaluated `#if` or `#elif` operation overflows int64 or shifts out of range.
    ConditionOverflow = 223,
    /// An `#if` group has no `#endif` before the end of the file that opened it.
    UnterminatedConditional = 224,
    /// `#elif`, `#else` or `#endif` has no matching `#if`, or follows `#else`.
    UnmatchedConditional = 225,
    /// An `#error` directive was reached; its text is the message.
    ErrorDirective = 226,
    /// A `#pragma` other than `#pragma once`.
    UnknownPragma = 227,
    /// `#line` is reserved for generated code.
    LineDirective = 228,
    /// `#if` nesting exceeds MAX_CONDITIONAL_DEPTH, or expression nesting exceeds
    /// MAX_CONDITION_NESTING.
    ConditionalDepthLimit = 229,
    /// Macro expansion exceeds MAX_MACRO_EXPANSIONS, MAX_MACRO_EXPANSION_DEPTH or
    /// MAX_SYNTHESIZED_BYTES.
    MacroExpansionLimit = 230,
    /// The preprocessed translation unit exceeds MAX_OUTPUT_TOKENS.
    OutputTokenLimit = 231,
    /// More than MAX_MACROS distinct macro names, or more than MAX_MACRO_PARAMETERS parameters.
    MacroLimit = 232,
    /// `defined` appears in an `#if` expression as a result of macro expansion.
    DefinedFromExpansion = 233,
};

/// @brief Return the stable kebab-case name of a code, such as `unknown-directive`.
/// @return An empty view for a value outside the enumerators.
[[nodiscard]] std::string_view GetDiagnosticCodeName(DiagnosticCode code) noexcept;

/// How a diagnostic affects the result.
enum class Severity : foundation::uint8
{
    /// The request fails; no output is published.
    Error,
    /// Reported only; the request may still succeed.
    Warning,
};

/// SourceLocation::FileIndex of text supplied as a configuration value instead of a file.
inline constexpr foundation::uint16 CONFIGURATION_SOURCE = 0xfffe;
/// SourceLocation::FileIndex of a location that does not exist.
inline constexpr foundation::uint16 NO_SOURCE = 0xffff;

/// @brief A position in a snapshot file or configuration value.
///
/// For a file, `FileIndex` indexes the request's files and `ByteOffset` counts bytes from the start
/// of that file. For CONFIGURATION_SOURCE, `Line` is the one-based index of the configuration value
/// and `ByteOffset` counts bytes inside its value text.
struct SourceLocation final
{
    /// Index of the file in the request, CONFIGURATION_SOURCE or NO_SOURCE.
    foundation::uint16 FileIndex = NO_SOURCE;
    /// Zero-based byte offset of the position.
    foundation::uint32 ByteOffset = 0;
    /// One-based line number.
    foundation::uint32 Line = 0;
    /// One-based column, counted in bytes.
    foundation::uint32 Column = 0;

    /// True when the location names a file or configuration value.
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return FileIndex != NO_SOURCE;
    }
};

/// Bounded diagnostic text; MAX_DIAGNOSTIC_MESSAGE_BYTES bytes at most.
using DiagnosticMessage = foundation::StaticString<MAX_DIAGNOSTIC_MESSAGE_BYTES>;

/// @brief One reported problem.
///
/// A diagnostic about a token produced by macro expansion carries two locations: `Location` is
/// where the token is spelled (inside the macro definition) and `ExpansionLocation` is the
/// outermost macro use that produced it.
struct Diagnostic final
{
    /// Stable code; see DiagnosticCode.
    DiagnosticCode Code = DiagnosticCode::InvalidRequest;
    /// Whether the diagnostic fails the request.
    Severity Level = Severity::Error;
    /// Where the problem is spelled; invalid for request-level problems.
    SourceLocation Location;
    /// Outermost macro use that produced the spelled tokens; invalid when not from an expansion.
    SourceLocation ExpansionLocation;
    /// Human-readable explanation, clipped to its capacity.
    DiagnosticMessage Message;
};
} // namespace ludus::tooling::shader_language
