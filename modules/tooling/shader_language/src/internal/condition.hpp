#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/token.hpp>

#include "diagnostic_sink.hpp"

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// One fully expanded token of an `#if` or `#elif` expression.
struct ConditionToken final
{
    /// Lexical category and punctuator.
    TokenKind Kind = TokenKind::Identifier;
    /// Which punctuator, for TokenKind::Punctuator.
    Punctuator Symbol = Punctuator::None;
    /// Spelling, valid while the expression is evaluated.
    std::string_view Spelling;
    /// Where the token is spelled.
    SourceLocation Location;
    /// Outermost macro use that produced it, if any.
    SourceLocation ExpansionLocation;
};

/// @brief Evaluate a C integer constant expression in int64 arithmetic.
///
/// Accepts decimal and `0x` hexadecimal literals, `true`, `false`, parentheses, unary `+ - ~`,
/// `* / % + - << >> < <= > >= == != & ^ | && || ?:`. Operands that `&&`, `||` or `?:` skip are
/// parsed but not evaluated, so they cannot divide by zero. Reports one diagnostic on failure.
/// @param tokens The expression after `defined` replacement and macro expansion.
/// @param directive The `#if` or `#elif` name token, for diagnostics about the whole expression.
/// @param outIsTrue Receives whether the value is nonzero; unchanged on failure.
/// @return False when the expression is invalid; a diagnostic was reported.
[[nodiscard]] bool EvaluateCondition(const foundation::Array<ConditionToken>& tokens,
                                     const ConditionToken& directive,
                                     DiagnosticSink& inoutSink,
                                     bool& outIsTrue) noexcept;
} // namespace ludus::tooling::shader_language::internal
