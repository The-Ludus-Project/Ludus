#include "internal/condition.hpp"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::int64;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;

/// Binary operator precedences from C; higher binds tighter.
constexpr uint32 PRECEDENCE_LOGICAL_OR = 1;
constexpr uint32 PRECEDENCE_LOGICAL_AND = 2;
constexpr uint32 PRECEDENCE_BITWISE_OR = 3;
constexpr uint32 PRECEDENCE_BITWISE_XOR = 4;
constexpr uint32 PRECEDENCE_BITWISE_AND = 5;
constexpr uint32 PRECEDENCE_EQUALITY = 6;
constexpr uint32 PRECEDENCE_RELATIONAL = 7;
constexpr uint32 PRECEDENCE_SHIFT = 8;
constexpr uint32 PRECEDENCE_ADDITIVE = 9;
constexpr uint32 PRECEDENCE_MULTIPLICATIVE = 10;

/// Precedence of a binary operator; higher binds tighter. Zero is not a binary operator.
[[nodiscard]] uint32 GetPrecedence(Punctuator symbol) noexcept
{
    switch (symbol)
    {
        case Punctuator::PipePipe:
            return PRECEDENCE_LOGICAL_OR;
        case Punctuator::AmpersandAmpersand:
            return PRECEDENCE_LOGICAL_AND;
        case Punctuator::Pipe:
            return PRECEDENCE_BITWISE_OR;
        case Punctuator::Caret:
            return PRECEDENCE_BITWISE_XOR;
        case Punctuator::Ampersand:
            return PRECEDENCE_BITWISE_AND;
        case Punctuator::EqualEqual:
        case Punctuator::ExclamationEqual:
            return PRECEDENCE_EQUALITY;
        case Punctuator::Less:
        case Punctuator::LessEqual:
        case Punctuator::Greater:
        case Punctuator::GreaterEqual:
            return PRECEDENCE_RELATIONAL;
        case Punctuator::LessLess:
        case Punctuator::GreaterGreater:
            return PRECEDENCE_SHIFT;
        case Punctuator::Plus:
        case Punctuator::Minus:
            return PRECEDENCE_ADDITIVE;
        case Punctuator::Star:
        case Punctuator::Slash:
        case Punctuator::Percent:
            return PRECEDENCE_MULTIPLICATIVE;
        default:
            return 0;
    }
}

/// Largest shift count that keeps an int64 shift defined.
constexpr int64 MAX_SHIFT = 62;
/// Base of hexadecimal literals.
constexpr uint64 HEXADECIMAL_BASE = 16;
/// Value of the hexadecimal digit `a`.
constexpr uint64 HEXADECIMAL_LETTER_VALUE = 10;
/// Largest int64.
constexpr int64 INT64_MAXIMUM = 0x7fffffffffffffff;
/// Smallest int64.
constexpr int64 INT64_MINIMUM = -INT64_MAXIMUM - 1;

class ConditionParser final
{
public:
    ConditionParser(const foundation::Array<ConditionToken>& tokens,
                    const ConditionToken& directive,
                    DiagnosticSink& inoutSink) noexcept
        : mTokens(tokens), mDirective(directive), mSink(inoutSink)
    {
    }

    [[nodiscard]] bool Evaluate(bool& outIsTrue) noexcept
    {
        if (mTokens.IsEmpty() == true)
        {
            return reportText(mDirective, DiagnosticCode::InvalidCondition, "the condition is empty");
        }
        int64 value = 0;
        if (parseConditional(true, value) == false)
        {
            return false;
        }
        if (mPosition != mTokens.GetSize())
        {
            return reportAt(mTokens[mPosition],
                            DiagnosticCode::InvalidCondition,
                            ComposeMessage("unexpected '", mTokens[mPosition].Spelling, "' in the condition"));
        }
        outIsTrue = value != 0;
        return true;
    }

private:
    [[nodiscard]] bool
    reportAt(const ConditionToken& token, DiagnosticCode code, const DiagnosticMessage& message) noexcept
    {
        mSink.ReportError(code, token.Location, token.ExpansionLocation, message);
        return false;
    }

    [[nodiscard]] bool reportText(const ConditionToken& token, DiagnosticCode code, std::string_view message) noexcept
    {
        return reportAt(token, code, ComposeMessage(message));
    }

    [[nodiscard]] bool isAt(Punctuator symbol) const noexcept
    {
        return mPosition < mTokens.GetSize() && mTokens[mPosition].Kind == TokenKind::Punctuator &&
               mTokens[mPosition].Symbol == symbol;
    }

    [[nodiscard]] const ConditionToken& getCurrentOrDirective() const noexcept
    {
        return mPosition < mTokens.GetSize() ? mTokens[mPosition] : mDirective;
    }

    [[nodiscard]] bool enter() noexcept
    {
        ++mDepth;
        if (mDepth > MAX_CONDITION_NESTING)
        {
            mSink.ReportError(DiagnosticCode::ConditionalDepthLimit,
                              getCurrentOrDirective().Location,
                              getCurrentOrDirective().ExpansionLocation,
                              ComposeMessage("the condition nests deeper than ", uint64{MAX_CONDITION_NESTING}));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool parseConditional(bool isEvaluated, int64& outValue) noexcept
    {
        if (enter() == false)
        {
            return false;
        }
        int64 condition = 0;
        if (parseBinary(PRECEDENCE_LOGICAL_OR, isEvaluated, condition) == false)
        {
            return false;
        }
        if (isAt(Punctuator::Question) == false)
        {
            outValue = condition;
            --mDepth;
            return true;
        }
        ++mPosition;
        int64 whenTrue = 0;
        if (parseConditional(isEvaluated == true && condition != 0, whenTrue) == false)
        {
            return false;
        }
        if (isAt(Punctuator::Colon) == false)
        {
            return reportText(getCurrentOrDirective(),
                              DiagnosticCode::InvalidCondition,
                              "expected ':' after '?' operand");
        }
        ++mPosition;
        int64 whenFalse = 0;
        if (parseConditional(isEvaluated == true && condition == 0, whenFalse) == false)
        {
            return false;
        }
        outValue = condition != 0 ? whenTrue : whenFalse;
        --mDepth;
        return true;
    }

    // Precedence climbing: operands bind to operators of at least `minimum` precedence.
    [[nodiscard]] bool parseBinary(uint32 minimum, bool isEvaluated, int64& outValue) noexcept
    {
        int64 left = 0;
        if (parseUnary(isEvaluated, left) == false)
        {
            return false;
        }
        while (mPosition < mTokens.GetSize() && mTokens[mPosition].Kind == TokenKind::Punctuator)
        {
            const ConditionToken& operation = mTokens[mPosition];
            const uint32 precedence = GetPrecedence(operation.Symbol);
            if (precedence == 0 || precedence < minimum)
            {
                break;
            }
            ++mPosition;
            bool isRightEvaluated = isEvaluated;
            if (operation.Symbol == Punctuator::AmpersandAmpersand)
            {
                isRightEvaluated = isEvaluated == true && left != 0;
            }
            else if (operation.Symbol == Punctuator::PipePipe)
            {
                isRightEvaluated = isEvaluated == true && left == 0;
            }
            int64 right = 0;
            if (enter() == false || parseBinary(precedence + 1, isRightEvaluated, right) == false)
            {
                return false;
            }
            --mDepth;
            if (combine(operation, isEvaluated, left, right, left) == false)
            {
                return false;
            }
        }
        outValue = left;
        return true;
    }

    [[nodiscard]] bool
    combine(const ConditionToken& operation, bool isEvaluated, int64 left, int64 right, int64& outValue) noexcept
    {
        if (isEvaluated == false)
        {
            outValue = 0;
            return true;
        }
        int64 result = 0;
        bool isRepresentable = true;
        switch (operation.Symbol)
        {
            case Punctuator::PipePipe:
                result = (left != 0 || right != 0) ? 1 : 0;
                break;
            case Punctuator::AmpersandAmpersand:
                result = (left != 0 && right != 0) ? 1 : 0;
                break;
            case Punctuator::Pipe:
                result = left | right;
                break;
            case Punctuator::Caret:
                result = left ^ right;
                break;
            case Punctuator::Ampersand:
                result = left & right;
                break;
            case Punctuator::EqualEqual:
                result = left == right ? 1 : 0;
                break;
            case Punctuator::ExclamationEqual:
                result = left != right ? 1 : 0;
                break;
            case Punctuator::Less:
                result = left < right ? 1 : 0;
                break;
            case Punctuator::LessEqual:
                result = left <= right ? 1 : 0;
                break;
            case Punctuator::Greater:
                result = left > right ? 1 : 0;
                break;
            case Punctuator::GreaterEqual:
                result = left >= right ? 1 : 0;
                break;
            case Punctuator::LessLess:
                if (right < 0 || right > MAX_SHIFT || left < 0 || left > (INT64_MAXIMUM >> right))
                {
                    return reportText(operation,
                                      DiagnosticCode::ConditionOverflow,
                                      "'<<' shifts out of the int64 range");
                }
                result = left << right;
                break;
            case Punctuator::GreaterGreater:
                if (right < 0 || right > MAX_SHIFT)
                {
                    return reportText(operation, DiagnosticCode::ConditionOverflow, "'>>' shift count is out of range");
                }
                result = left >> right;
                break;
            case Punctuator::Plus:
                isRepresentable = foundation::core::TryAdd(left, right, result);
                break;
            case Punctuator::Minus:
                isRepresentable = foundation::core::TrySubtract(left, right, result);
                break;
            case Punctuator::Star:
                isRepresentable = foundation::core::TryMultiply(left, right, result);
                break;
            case Punctuator::Slash:
            case Punctuator::Percent:
                if (right == 0)
                {
                    return reportText(operation,
                                      DiagnosticCode::ConditionDivisionByZero,
                                      "the condition divides by zero");
                }
                if (left == INT64_MINIMUM && right == -1)
                {
                    isRepresentable = false;
                    break;
                }
                result = operation.Symbol == Punctuator::Slash ? left / right : left % right;
                break;
            default:
                return reportText(operation, DiagnosticCode::InvalidCondition, "unexpected operator");
        }
        if (isRepresentable == false)
        {
            return reportAt(operation,
                            DiagnosticCode::ConditionOverflow,
                            ComposeMessage("'", operation.Spelling, "' overflows int64"));
        }
        outValue = result;
        return true;
    }

    [[nodiscard]] bool parseUnary(bool isEvaluated, int64& outValue) noexcept
    {
        if (mPosition >= mTokens.GetSize())
        {
            return reportText(mDirective, DiagnosticCode::InvalidCondition, "the condition ends before an operand");
        }
        const ConditionToken& token = mTokens[mPosition];
        if (token.Kind == TokenKind::Punctuator && token.Symbol == Punctuator::Exclamation)
        {
            return reportText(token,
                              DiagnosticCode::LogicalNot,
                              "LUSL has no '!'; compare with '== 0', or use '#ifndef'");
        }
        if (token.Kind == TokenKind::Punctuator &&
            (token.Symbol == Punctuator::Plus || token.Symbol == Punctuator::Minus ||
             token.Symbol == Punctuator::Tilde))
        {
            ++mPosition;
            int64 operand = 0;
            if (enter() == false || parseUnary(isEvaluated, operand) == false)
            {
                return false;
            }
            --mDepth;
            if (token.Symbol == Punctuator::Minus)
            {
                if (isEvaluated == true && operand == INT64_MINIMUM)
                {
                    return reportText(token, DiagnosticCode::ConditionOverflow, "'-' overflows int64");
                }
                outValue = isEvaluated == true ? -operand : 0;
            }
            else
            {
                outValue = token.Symbol == Punctuator::Tilde ? ~operand : operand;
            }
            return true;
        }
        return parsePrimary(token, isEvaluated, outValue);
    }

    [[nodiscard]] bool parsePrimary(const ConditionToken& token, bool isEvaluated, int64& outValue) noexcept
    {
        if (token.Kind == TokenKind::Punctuator && token.Symbol == Punctuator::LeftParenthesis)
        {
            ++mPosition;
            if (parseConditional(isEvaluated, outValue) == false)
            {
                return false;
            }
            if (isAt(Punctuator::RightParenthesis) == false)
            {
                return reportText(getCurrentOrDirective(), DiagnosticCode::InvalidCondition, "expected ')'");
            }
            ++mPosition;
            return true;
        }
        if (token.Kind == TokenKind::Identifier)
        {
            if (token.Spelling == "true" || token.Spelling == "false")
            {
                outValue = token.Spelling == "true" ? 1 : 0;
                ++mPosition;
                return true;
            }
            return reportAt(
                token,
                DiagnosticCode::UndefinedIdentifierInCondition,
                ComposeMessage("'", token.Spelling, "' is not a macro; LUSL does not treat unknown names as 0"));
        }
        if (token.Kind == TokenKind::Number)
        {
            ++mPosition;
            return parseLiteral(token, outValue);
        }
        return reportAt(token,
                        DiagnosticCode::InvalidCondition,
                        ComposeMessage("'", token.Spelling, "' is not valid in a condition"));
    }

    [[nodiscard]] bool parseLiteral(const ConditionToken& token, int64& outValue) noexcept
    {
        const std::string_view spelling = token.Spelling;
        uint64 value = 0;
        if (spelling.size() > 2 && spelling[0] == '0' && (spelling[1] == 'x' || spelling[1] == 'X'))
        {
            for (usize i = 2; i < spelling.size(); ++i)
            {
                const char digit = spelling[i];
                uint64 digitValue = HEXADECIMAL_BASE;
                if (digit >= '0' && digit <= '9')
                {
                    digitValue = static_cast<uint64>(digit - '0');
                }
                else if (digit >= 'a' && digit <= 'f')
                {
                    digitValue = static_cast<uint64>(digit - 'a') + HEXADECIMAL_LETTER_VALUE;
                }
                else if (digit >= 'A' && digit <= 'F')
                {
                    digitValue = static_cast<uint64>(digit - 'A') + HEXADECIMAL_LETTER_VALUE;
                }
                if (digitValue == HEXADECIMAL_BASE)
                {
                    return reportAt(token,
                                    DiagnosticCode::InvalidCondition,
                                    ComposeMessage("'", spelling, "' is not an integer literal"));
                }
                if (foundation::core::TryMultiply(value, HEXADECIMAL_BASE, value) == false ||
                    foundation::core::TryAdd(value, digitValue, value) == false)
                {
                    return reportAt(token,
                                    DiagnosticCode::ConditionOverflow,
                                    ComposeMessage("'", spelling, "' does not fit in int64"));
                }
            }
        }
        else
        {
            if (spelling.size() > 1 && spelling[0] == '0')
            {
                return reportAt(token,
                                DiagnosticCode::InvalidCondition,
                                ComposeMessage("'", spelling, "' looks octal; LUSL has no octal literals"));
            }
            const foundation::NumberParseStatus status =
                foundation::ParseUint64(spelling.data(), spelling.size(), value);
            if (status == foundation::NumberParseStatus::OutOfRange)
            {
                return reportAt(token,
                                DiagnosticCode::ConditionOverflow,
                                ComposeMessage("'", spelling, "' does not fit in int64"));
            }
            if (status != foundation::NumberParseStatus::Success)
            {
                return reportAt(token,
                                DiagnosticCode::InvalidCondition,
                                ComposeMessage("'", spelling, "' is not an integer literal"));
            }
        }
        if (value > static_cast<uint64>(INT64_MAXIMUM))
        {
            return reportAt(token,
                            DiagnosticCode::ConditionOverflow,
                            ComposeMessage("'", spelling, "' does not fit in int64"));
        }
        outValue = static_cast<int64>(value);
        return true;
    }

    const foundation::Array<ConditionToken>& mTokens;
    const ConditionToken& mDirective;
    DiagnosticSink& mSink;
    usize mPosition = 0;
    usize mDepth = 0;
};
} // namespace

bool EvaluateCondition(const foundation::Array<ConditionToken>& tokens,
                       const ConditionToken& directive,
                       DiagnosticSink& inoutSink,
                       bool& outIsTrue) noexcept
{
    ConditionParser parser(tokens, directive, inoutSink);
    return parser.Evaluate(outIsTrue);
}
} // namespace ludus::tooling::shader_language::internal
