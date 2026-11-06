#include <ludus/tooling/shader_language/diagnostic.hpp>

#include <ludus/foundation/containers/static_array.hpp>

#include "internal/diagnostic_sink.hpp"
#include "internal/utf8_bytes.hpp"

namespace ludus::tooling::shader_language
{
std::string_view GetDiagnosticCodeName(DiagnosticCode code) noexcept
{
    switch (code)
    {
        case DiagnosticCode::LogicalNot:
            return "logical-not";
        case DiagnosticCode::InvalidRequest:
            return "invalid-request";
        case DiagnosticCode::InvalidUtf8:
            return "invalid-utf8";
        case DiagnosticCode::InvalidCharacter:
            return "invalid-character";
        case DiagnosticCode::UnterminatedComment:
            return "unterminated-comment";
        case DiagnosticCode::UnterminatedString:
            return "unterminated-string";
        case DiagnosticCode::InvalidEscape:
            return "invalid-escape";
        case DiagnosticCode::CharacterLiteral:
            return "character-literal";
        case DiagnosticCode::FileTokenLimit:
            return "file-token-limit";
        case DiagnosticCode::UnknownDirective:
            return "unknown-directive";
        case DiagnosticCode::MalformedDirective:
            return "malformed-directive";
        case DiagnosticCode::IncludeNotFound:
            return "include-not-found";
        case DiagnosticCode::IncludeNotHeader:
            return "include-not-header";
        case DiagnosticCode::InvalidIncludePath:
            return "invalid-include-path";
        case DiagnosticCode::RecursiveInclude:
            return "recursive-include";
        case DiagnosticCode::IncludeDepthLimit:
            return "include-depth-limit";
        case DiagnosticCode::MacroRedefined:
            return "macro-redefined";
        case DiagnosticCode::ConfigurationModified:
            return "configuration-modified";
        case DiagnosticCode::ReservedMacroName:
            return "reserved-macro-name";
        case DiagnosticCode::VariadicMacro:
            return "variadic-macro";
        case DiagnosticCode::DuplicateMacroParameter:
            return "duplicate-macro-parameter";
        case DiagnosticCode::StringifyNotParameter:
            return "stringify-not-parameter";
        case DiagnosticCode::PasteAtEdge:
            return "paste-at-edge";
        case DiagnosticCode::InvalidPaste:
            return "invalid-paste";
        case DiagnosticCode::MacroArgumentCount:
            return "macro-argument-count";
        case DiagnosticCode::UnterminatedMacroInvocation:
            return "unterminated-macro-invocation";
        case DiagnosticCode::DirectiveInMacroArguments:
            return "directive-in-macro-arguments";
        case DiagnosticCode::UndefinedIdentifierInCondition:
            return "undefined-identifier-in-condition";
        case DiagnosticCode::UnknownMacroName:
            return "unknown-macro-name";
        case DiagnosticCode::InvalidCondition:
            return "invalid-condition";
        case DiagnosticCode::ConditionDivisionByZero:
            return "condition-division-by-zero";
        case DiagnosticCode::ConditionOverflow:
            return "condition-overflow";
        case DiagnosticCode::UnterminatedConditional:
            return "unterminated-conditional";
        case DiagnosticCode::UnmatchedConditional:
            return "unmatched-conditional";
        case DiagnosticCode::ErrorDirective:
            return "error-directive";
        case DiagnosticCode::UnknownPragma:
            return "unknown-pragma";
        case DiagnosticCode::LineDirective:
            return "line-directive";
        case DiagnosticCode::ConditionalDepthLimit:
            return "conditional-depth-limit";
        case DiagnosticCode::MacroExpansionLimit:
            return "macro-expansion-limit";
        case DiagnosticCode::OutputTokenLimit:
            return "output-token-limit";
        case DiagnosticCode::MacroLimit:
            return "macro-limit";
        case DiagnosticCode::DefinedFromExpansion:
            return "defined-from-expansion";
        case DiagnosticCode::UnexpectedToken:
            return "unexpected-token";
        case DiagnosticCode::AssignmentInExpression:
            return "assignment-in-expression";
        case DiagnosticCode::MultipleDeclarators:
            return "multiple-declarators";
        case DiagnosticCode::LaterPhaseConstruct:
            return "later-phase-construct";
        case DiagnosticCode::KeywordAsName:
            return "keyword-as-name";
        case DiagnosticCode::CStyleCast:
            return "c-style-cast";
        case DiagnosticCode::ExpressionWithoutEffect:
            return "expression-without-effect";
        case DiagnosticCode::PositionalAfterNamed:
            return "positional-after-named";
        case DiagnosticCode::UnscopedEnum:
            return "unscoped-enum";
        case DiagnosticCode::SyntaxNodeLimit:
            return "syntax-node-limit";
        case DiagnosticCode::SyntaxDepthLimit:
            return "syntax-depth-limit";
        case DiagnosticCode::FormattedSourceLimit:
            return "formatted-source-limit";
        case DiagnosticCode::UnknownName:
            return "unknown-name";
        case DiagnosticCode::UnknownType:
            return "unknown-type";
        case DiagnosticCode::Redeclaration:
            return "redeclaration";
        case DiagnosticCode::TypeMismatch:
            return "type-mismatch";
        case DiagnosticCode::ConstantOutOfRange:
            return "constant-out-of-range";
        case DiagnosticCode::InvalidLiteral:
            return "invalid-literal";
        case DiagnosticCode::InvalidOperands:
            return "invalid-operands";
        case DiagnosticCode::NonBoolCondition:
            return "non-bool-condition";
        case DiagnosticCode::NotAssignable:
            return "not-assignable";
        case DiagnosticCode::NoMatchingOverload:
            return "no-matching-overload";
        case DiagnosticCode::AmbiguousCall:
            return "ambiguous-call";
        case DiagnosticCode::UnknownNamedArgument:
            return "unknown-named-argument";
        case DiagnosticCode::DuplicateArgument:
            return "duplicate-argument";
        case DiagnosticCode::MissingArgument:
            return "missing-argument";
        case DiagnosticCode::DirectionMismatch:
            return "direction-mismatch";
        case DiagnosticCode::AliasedArguments:
            return "aliased-arguments";
        case DiagnosticCode::ArraySizeLiteral:
            return "array-size-literal";
        case DiagnosticCode::NotConstant:
            return "not-constant";
        case DiagnosticCode::InvalidArraySize:
            return "invalid-array-size";
        case DiagnosticCode::UnknownMember:
            return "unknown-member";
        case DiagnosticCode::InvalidInitializer:
            return "invalid-initializer";
        case DiagnosticCode::InvalidZeroInitializer:
            return "invalid-zero-initializer";
        case DiagnosticCode::InvalidReturn:
            return "invalid-return";
        case DiagnosticCode::InvalidTopLevelVariable:
            return "invalid-top-level-variable";
        case DiagnosticCode::StaticAssertFailed:
            return "static-assert-failed";
        case DiagnosticCode::InvalidAttribute:
            return "invalid-attribute";
        case DiagnosticCode::InvalidSemantic:
            return "invalid-semantic";
        case DiagnosticCode::InvalidSwitch:
            return "invalid-switch";
        case DiagnosticCode::ConstantOverflow:
            return "constant-overflow";
        case DiagnosticCode::NotCallable:
            return "not-callable";
        case DiagnosticCode::UnsupportedConstruct:
            return "unsupported-construct";
        case DiagnosticCode::VoidValue:
            return "void-value";
        case DiagnosticCode::InvalidConstruction:
            return "invalid-construction";
        case DiagnosticCode::UndefinedFunction:
            return "undefined-function";
        case DiagnosticCode::CheckerLimit:
            return "checker-limit";
    }
    return {};
}
} // namespace ludus::tooling::shader_language

namespace ludus::tooling::shader_language::internal
{
namespace
{
/// Enough digits for any uint64.
constexpr foundation::usize MAX_UINT64_DIGITS = 20;
/// Base of the printed digits.
constexpr foundation::uint64 DECIMAL_BASE = 10;
} // namespace

void AppendClipped(DiagnosticMessage& inoutMessage, std::string_view text) noexcept
{
    const foundation::usize available = DiagnosticMessage::GetCapacity() - inoutMessage.GetSize();
    foundation::usize size = text.size() < available ? text.size() : available;
    // Never split a UTF-8 sequence: back up to the start of the clipped character.
    while (size > 0 && size < text.size() && IsUtf8Continuation(text[size]) == true)
    {
        --size;
    }
    (void)inoutMessage.TryAppend(text.substr(0, size));
}

void AppendUnsigned(DiagnosticMessage& inoutMessage, foundation::uint64 value) noexcept
{
    foundation::StaticArray<char, MAX_UINT64_DIGITS> digits{};
    foundation::usize count = 0;
    do
    {
        digits[MAX_UINT64_DIGITS - 1 - count] = static_cast<char>('0' + (value % DECIMAL_BASE));
        value /= DECIMAL_BASE;
        ++count;
    } while (value != 0);
    AppendClipped(inoutMessage, std::string_view(digits.GetData() + (MAX_UINT64_DIGITS - count), count));
}

DiagnosticSink::DiagnosticSink(foundation::Array<Diagnostic>& inoutDiagnostics,
                               foundation::usize& inoutDroppedCount) noexcept
    : mDiagnostics(inoutDiagnostics), mDroppedCount(inoutDroppedCount)
{
}

void DiagnosticSink::ReportError(DiagnosticCode code,
                                 const SourceLocation& location,
                                 const SourceLocation& expansionLocation,
                                 const DiagnosticMessage& message) noexcept
{
    ++mErrorCount;
    if (mDiagnostics.GetSize() >= MAX_DIAGNOSTICS)
    {
        ++mDroppedCount;
        return;
    }
    Diagnostic diagnostic;
    diagnostic.Code = code;
    diagnostic.Level = Severity::Error;
    diagnostic.Location = location;
    diagnostic.ExpansionLocation = expansionLocation;
    diagnostic.Message = message;
    if (mDiagnostics.TryAdd(diagnostic) == false)
    {
        mHasOutOfMemory = true;
    }
}
} // namespace ludus::tooling::shader_language::internal
