#include "internal/call_binding.hpp"

namespace ludus::tooling::shader_language::internal
{
using foundation::uint32;
using foundation::usize;

bool IsImplicitlyConvertible(const TypeTable& types, uint32 from, uint32 to) noexcept
{
    if (from == to)
    {
        return true;
    }
    const SemanticType& source = types.Get(from);
    const SemanticType& target = types.Get(to);
    if (source.Kind == TypeKind::Error || target.Kind == TypeKind::Error)
    {
        return true;
    }
    if (target.Kind != TypeKind::Scalar)
    {
        return false;
    }
    if (source.Kind == TypeKind::UntypedInteger)
    {
        return target.Scalar != ScalarKind::Bool;
    }
    if (source.Kind == TypeKind::UntypedFloat)
    {
        return IsFloatScalar(target.Scalar);
    }
    return false;
}

bool BindArguments(foundation::BorrowedView<const ArgumentFacts> arguments,
                   foundation::BorrowedView<const ParameterFacts> parameters,
                   const TypeTable& types,
                   BindingResult& outResult) noexcept
{
    outResult = BindingResult{};
    foundation::StaticArray<bool, MAX_FUNCTION_PARAMETERS> isBound{};
    uint32 positional = 0;
    for (usize i = 0; i < arguments.GetSize(); ++i)
    {
        const ArgumentFacts& argument = arguments[i];
        uint32 parameter = 0;
        if (argument.Name.empty() == true)
        {
            parameter = positional;
            ++positional;
            if (parameter >= parameters.GetSize())
            {
                outResult.Failure = BindingFailure::TooManyArguments;
                outResult.FailingArgument = static_cast<uint32>(i);
                return false;
            }
        }
        else
        {
            bool isFound = false;
            for (usize p = 0; p < parameters.GetSize(); ++p)
            {
                if (parameters[p].Name == argument.Name)
                {
                    parameter = static_cast<uint32>(p);
                    isFound = true;
                    break;
                }
            }
            if (isFound == false)
            {
                outResult.Failure = BindingFailure::UnknownName;
                outResult.FailingArgument = static_cast<uint32>(i);
                return false;
            }
        }
        if (isBound[parameter] == true)
        {
            outResult.Failure = BindingFailure::Duplicate;
            outResult.FailingArgument = static_cast<uint32>(i);
            return false;
        }
        isBound[parameter] = true;
        outResult.ParameterOfArgument[i] = parameter;
        const ParameterFacts& target = parameters[parameter];
        if (argument.Type == NO_TYPE)
        {
            if (target.CanTakeInitializer == false)
            {
                outResult.Failure = BindingFailure::TypeMismatch;
                outResult.FailingArgument = static_cast<uint32>(i);
                return false;
            }
            continue;
        }
        if (IsImplicitlyConvertible(types, argument.Type, target.Type) == false)
        {
            outResult.Failure = BindingFailure::TypeMismatch;
            outResult.FailingArgument = static_cast<uint32>(i);
            return false;
        }
        if (argument.Type != target.Type)
        {
            ++outResult.ConversionCount;
        }
    }
    for (usize p = 0; p < parameters.GetSize(); ++p)
    {
        if (isBound[p] == false && parameters[p].HasDefault == false)
        {
            outResult.Failure = BindingFailure::Missing;
            outResult.FailingParameter = static_cast<uint32>(p);
            return false;
        }
    }
    return true;
}
} // namespace ludus::tooling::shader_language::internal
