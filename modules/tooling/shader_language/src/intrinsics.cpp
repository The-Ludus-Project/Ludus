#include "internal/intrinsics.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::uint32;

/// Components of the vectors `cross` accepts.
constexpr uint32 CROSS_COMPONENTS = 3;
/// Arguments of the three-operand families.
constexpr uint32 TERNARY_ARITY = 3;

[[nodiscard]] bool IsThirtyTwoBit(ScalarKind kind) noexcept
{
    return kind == ScalarKind::Int32 || kind == ScalarKind::Uint32 || kind == ScalarKind::Float32;
}
} // namespace

uint32 GetIntrinsicArity(IntrinsicFamily family) noexcept
{
    switch (family)
    {
        case IntrinsicFamily::NumericBinary:
        case IntrinsicFamily::FloatBinary:
        case IntrinsicFamily::Dot:
        case IntrinsicFamily::Distance:
        case IntrinsicFamily::Cross:
        case IntrinsicFamily::Reflect:
            return 2;
        case IntrinsicFamily::NumericTernary:
        case IntrinsicFamily::FloatTernary:
            return TERNARY_ARITY;
        default:
            return 1;
    }
}

// Two named out-parameters, operand then result.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool ResolveIntrinsic(IntrinsicFamily family,
                      const TypeTable& types,
                      foundation::BorrowedView<const uint32> argumentTypes,
                      uint32& outOperandType,
                      uint32& outResultType) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (argumentTypes.GetSize() != GetIntrinsicArity(family))
    {
        return false;
    }
    // The operand type is the widest argument that is not an untyped constant.
    uint32 operand = NO_TYPE;
    bool hasUntypedFloat = false;
    for (foundation::usize i = 0; i < argumentTypes.GetSize(); ++i)
    {
        const TypeKind kind = types.Get(argumentTypes[i]).Kind;
        if (kind == TypeKind::Error)
        {
            outOperandType = TypeTable::ERROR_TYPE;
            outResultType = TypeTable::ERROR_TYPE;
            return true;
        }
        if (kind == TypeKind::UntypedFloat)
        {
            hasUntypedFloat = true;
        }
        else if (kind != TypeKind::UntypedInteger &&
                 (operand == NO_TYPE || types.Get(argumentTypes[i]).Rows > types.Get(operand).Rows))
        {
            // The widest typed argument: a vector beside scalars sets the shape.
            operand = argumentTypes[i];
        }
    }
    const bool isFloatFamily = family == IntrinsicFamily::FloatUnary || family == IntrinsicFamily::FloatVector ||
                               family == IntrinsicFamily::FloatBinary || family == IntrinsicFamily::FloatTernary ||
                               family == IntrinsicFamily::Length || family == IntrinsicFamily::Distance ||
                               family == IntrinsicFamily::Cross || family == IntrinsicFamily::Reflect;
    if (operand == NO_TYPE)
    {
        // Only constants: they take the family's default element type.
        const bool isFloat = hasUntypedFloat == true || isFloatFamily == true;
        operand = types.GetScalar(isFloat == true ? ScalarKind::Float32 : ScalarKind::Int32);
    }
    const SemanticType& shape = types.Get(operand);
    const bool isScalarOrVector = shape.Kind == TypeKind::Scalar || shape.Kind == TypeKind::Vector;
    const bool isVector = shape.Kind == TypeKind::Vector;
    const bool isNumeric = isScalarOrVector == true && shape.Scalar != ScalarKind::Bool;
    const bool isFloat = isScalarOrVector == true && IsFloatScalar(shape.Scalar) == true;
    outOperandType = operand;
    outResultType = operand;
    switch (family)
    {
        case IntrinsicFamily::FloatUnary:
        case IntrinsicFamily::FloatBinary:
        case IntrinsicFamily::FloatTernary:
            return isFloat;
        case IntrinsicFamily::FloatVector:
        case IntrinsicFamily::Reflect:
            return isFloat == true && isVector == true;
        case IntrinsicFamily::NumericUnary:
        case IntrinsicFamily::NumericBinary:
        case IntrinsicFamily::NumericTernary:
            return isNumeric;
        case IntrinsicFamily::Dot:
            outResultType = types.GetScalar(shape.Scalar);
            return isNumeric == true && isVector == true;
        case IntrinsicFamily::Length:
        case IntrinsicFamily::Distance:
            outResultType = types.GetScalar(shape.Scalar);
            return isFloat == true && isVector == true;
        case IntrinsicFamily::Cross:
            return isFloat == true && isVector == true && shape.Rows == CROSS_COMPONENTS;
        case IntrinsicFamily::BoolReduce:
            outResultType = types.GetScalar(ScalarKind::Bool);
            return isScalarOrVector == true && shape.Scalar == ScalarKind::Bool;
        case IntrinsicFamily::BitcastToUint:
            outResultType = types.WithScalar(operand, ScalarKind::Uint32);
            return isScalarOrVector == true && IsThirtyTwoBit(shape.Scalar) == true;
        case IntrinsicFamily::BitcastToInt:
            outResultType = types.WithScalar(operand, ScalarKind::Int32);
            return isScalarOrVector == true && IsThirtyTwoBit(shape.Scalar) == true;
        case IntrinsicFamily::BitcastToFloat:
            outResultType = types.WithScalar(operand, ScalarKind::Float32);
            return isScalarOrVector == true && IsThirtyTwoBit(shape.Scalar) == true;
        case IntrinsicFamily::BitCount:
            outResultType = types.WithScalar(operand, ScalarKind::Uint32);
            return isScalarOrVector == true &&
                   (shape.Scalar == ScalarKind::Int32 || shape.Scalar == ScalarKind::Uint32);
        case IntrinsicFamily::Transpose:
            if (shape.Kind != TypeKind::Matrix)
            {
                return false;
            }
            outResultType = types.GetMatrix(shape.Scalar, shape.Columns, shape.Rows);
            return true;
    }
    return false;
}
} // namespace ludus::tooling::shader_language::internal
