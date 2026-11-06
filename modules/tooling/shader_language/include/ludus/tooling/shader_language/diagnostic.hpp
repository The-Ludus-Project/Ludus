#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
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
/// language rules shared by every stage, 100-199 are lexical, 200-299 are preprocessor, 300-399
/// are syntax, 400-499 are formatter, 500-599 are semantic, 600-699 are style and 700-799 are
/// lowering diagnostics. docs/modules/tooling/shader-language.md lists every code.
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

    /// A token that the grammar does not allow here; the message names what was expected.
    UnexpectedToken = 301,
    /// `=` or a compound assignment inside an expression. Assignment is a statement in LUSL.
    AssignmentInExpression = 302,
    /// A declaration with more than one declarator, such as `float32 a, b;`.
    MultipleDeclarators = 303,
    /// A construct of a later LUSL phase, such as `interface`, `concept` or `template` (L2).
    LaterPhaseConstruct = 304,
    /// A keyword used where a name is required.
    KeywordAsName = 305,
    /// A C cast such as `(float32)value`; LUSL converts with `float32(value)`.
    CStyleCast = 306,
    /// An expression statement that is not a call, such as `a + b;`, has no effect.
    ExpressionWithoutEffect = 307,
    /// A positional argument follows a named argument.
    PositionalAfterNamed = 308,
    /// `enum` without `class`; LUSL enums are scoped.
    UnscopedEnum = 309,
    /// The source exceeds MAX_SYNTAX_NODES nodes.
    SyntaxNodeLimit = 310,
    /// Statements or expressions nest deeper than MAX_SYNTAX_DEPTH.
    SyntaxDepthLimit = 311,

    /// The formatted text would exceed `shader_compiler::MAX_SOURCE_BYTES`.
    FormattedSourceLimit = 401,

    /// A name that no visible declaration declares.
    UnknownName = 501,
    /// A type name that is neither built in nor declared.
    UnknownType = 502,
    /// A name declared twice in one scope, or a function signature defined twice.
    Redeclaration = 503,
    /// A value of one type where another is required; LUSL has no implicit conversions.
    TypeMismatch = 504,
    /// An untyped constant that its target type cannot represent exactly.
    ConstantOutOfRange = 505,
    /// A number that is not a decimal, hexadecimal or floating literal, or has a suffix.
    InvalidLiteral = 506,
    /// An operator applied to types it is not defined for.
    InvalidOperands = 507,
    /// An `if`, loop or `?:` condition that is not a `bool` scalar.
    NonBoolCondition = 508,
    /// Assignment, `++`, `--` or an `out`/`inout` argument whose target is not a variable.
    NotAssignable = 509,
    /// No function overload accepts the arguments.
    NoMatchingOverload = 510,
    /// More than one overload accepts the arguments equally well.
    AmbiguousCall = 511,
    /// A named argument that matches no parameter.
    UnknownNamedArgument = 512,
    /// A parameter bound by two arguments.
    DuplicateArgument = 513,
    /// A parameter without a default that no argument binds.
    MissingArgument = 514,
    /// An `out` or `inout` mark that does not match its parameter.
    DirectionMismatch = 515,
    /// Two writable arguments of one call that may refer to overlapping storage.
    AliasedArguments = 516,
    /// An array size written as a literal; name the size with a constant.
    ArraySizeLiteral = 517,
    /// A value that must be a constant expression is not one.
    NotConstant = 518,
    /// An array size that is not a positive integer constant within limits.
    InvalidArraySize = 519,
    /// A field, method or swizzle that the type does not have.
    UnknownMember = 520,
    /// A designated initializer without a struct target, or with an unknown or repeated field.
    InvalidInitializer = 521,
    /// `= 0` on a type that has no zero value, such as an opaque `extern slang` type.
    InvalidZeroInitializer = 522,
    /// A `return` whose value does not match the function, or `break`/`continue` outside a loop.
    InvalidReturn = 523,
    /// A top-level variable that is neither `static const` nor `groupshared`.
    InvalidTopLevelVariable = 524,
    /// A `static_assert` whose condition is false.
    StaticAssertFailed = 525,
    /// An unknown attribute, or one with the wrong arguments or target.
    InvalidAttribute = 526,
    /// An unknown system-value semantic, or one on the wrong parameter type.
    InvalidSemantic = 527,
    /// A switch selector that is not an integer or enum, or a repeated or non-constant label.
    InvalidSwitch = 528,
    /// Constant arithmetic that overflows or divides by zero.
    ConstantOverflow = 529,
    /// A call of something that is not a function or type.
    NotCallable = 530,
    /// A construct that a later LUSL phase supports, such as members of generic resource types.
    UnsupportedConstruct = 531,
    /// The result of a `void` call used as a value.
    VoidValue = 532,
    /// A vector, matrix, scalar or enum construction with the wrong arguments.
    InvalidConstruction = 533,
    /// A LUSL function declared without a body and never defined.
    UndefinedFunction = 534,
    /// More types or symbols than the checker allows.
    CheckerLimit = 535,
    /// A variable is read on a path where it may not have been assigned; initialize it with `= 0`,
    /// or declare it `= void` when a later write is certain.
    UseBeforeAssignment = 541,
    /// An `out` parameter may be unwritten when the function returns.
    OutParameterNotAssigned = 542,
    /// A function with a return type can reach its end without returning a value.
    MissingReturn = 543,
    /// A function calls itself, directly or through other functions; GPU code has no call stack.
    Recursion = 544,
    /// A `groupshared` variable is not declared `= 0` or `= void`.
    GroupsharedInitializer = 545,
    /// A name that the generated Slang cannot use: a Slang keyword, or a name starting with the
    /// `_lusl` prefix reserved for generated code.
    ReservedName = 546,

    /// A name breaks the naming rules: PascalCase types, functions and fields, lowerCamelCase
    /// parameters and locals, `out`/`inout` prefixes, UPPER_CASE constants, question-like booleans.
    NamingStyle = 601,
    /// A `bool` value used directly as a condition or `&&`/`||` operand; compare with `== true`.
    BareBoolOperand = 602,

    /// The Slang file named by `extern slang` is not in the request's snapshot.
    ExternSlangNotFound = 701,
    /// The generated Slang would exceed `shader_compiler::MAX_SOURCE_BYTES`, or lowering needs more
    /// generated helpers than MAX_GENERATED_HELPERS.
    LoweringLimit = 702,
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

/// Most text edits in one DiagnosticFix.
inline constexpr foundation::usize MAX_FIX_EDITS = 2;
/// Longest text one TextEdit inserts, in bytes.
inline constexpr foundation::usize MAX_FIX_TEXT_BYTES = 160;
/// Text inserted by a TextEdit.
using FixText = foundation::StaticString<MAX_FIX_TEXT_BYTES>;

/// @brief One replacement in a request file: remove `RemovedBytes` bytes at `ByteOffset`, then
/// insert `Inserted` there.
struct TextEdit final
{
    /// Index of the file in the request; edits never target configuration values.
    foundation::uint16 FileIndex = NO_SOURCE;
    /// Byte offset in that file's text.
    foundation::uint32 ByteOffset = 0;
    /// Bytes to remove at ByteOffset; zero for an insertion.
    foundation::uint32 RemovedBytes = 0;
    /// Text to insert at ByteOffset.
    FixText Inserted;
};

/// @brief A suggested repair of a diagnostic, as edits to the request's files.
///
/// Edits of one fix never overlap. Apply them from the highest ByteOffset down, so earlier offsets
/// stay valid. A fix is offered only where the affected tokens are spelled in a file, not produced
/// by a macro expansion or a configuration value.
struct DiagnosticFix final
{
    /// Number of valid entries in Edits; zero when the diagnostic has no fix.
    foundation::usize EditCount = 0;
    /// The edits.
    foundation::StaticArray<TextEdit, MAX_FIX_EDITS> Edits{};
};

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
    /// A suggested repair; Fix.EditCount is zero when none is safe to offer.
    DiagnosticFix Fix;
};
} // namespace ludus::tooling::shader_language
