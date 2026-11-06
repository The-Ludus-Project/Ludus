#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/tooling/shader_language/limits.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

#include "type_table.hpp"

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// One call argument as the binder sees it.
struct ArgumentFacts final
{
    /// Spelling of the parameter name of a named argument; empty for a positional one.
    std::string_view Name;
    /// Checked type of the value, or NO_TYPE for a designated initializer awaiting its target.
    foundation::uint32 Type = NO_TYPE;
};

/// ParameterFacts::TypeParameter of a parameter whose type is not a template parameter.
inline constexpr foundation::uint32 NO_TYPE_PARAMETER = 0xffffffffU;

/// One parameter of a candidate function.
struct ParameterFacts final
{
    std::string_view Name;
    foundation::uint32 Type = NO_TYPE;
    bool HasDefault = false;
    /// True when the parameter type is a struct that a designated initializer can build.
    bool CanTakeInitializer = false;
    /// Position of the candidate's template parameter that is this parameter's type, or
    /// NO_TYPE_PARAMETER; such a parameter deduces that template argument.
    foundation::uint32 TypeParameter = NO_TYPE_PARAMETER;
};

/// Why a candidate does not accept the arguments.
enum class BindingFailure : foundation::uint8
{
    None,             ///< The candidate is viable.
    TooManyArguments, ///< More positional arguments than parameters.
    UnknownName,      ///< A named argument matches no parameter; FailingArgument says which.
    Duplicate,        ///< Two arguments bind one parameter; FailingArgument is the second.
    Missing,          ///< FailingParameter has no argument and no default.
    TypeMismatch,     ///< FailingArgument does not convert to its parameter.
};

/// Outcome of binding one call to one candidate.
struct BindingResult final
{
    BindingFailure Failure = BindingFailure::None;
    foundation::uint32 FailingArgument = 0;
    foundation::uint32 FailingParameter = 0;
    /// Untyped constants that had to adapt; fewer is a better match.
    foundation::uint32 ConversionCount = 0;
    /// Parameter index each argument binds to.
    foundation::StaticArray<foundation::uint32, MAX_FUNCTION_PARAMETERS> ParameterOfArgument{};
    /// Deduced template arguments, NO_TYPE where no argument deduced one.
    foundation::StaticArray<foundation::uint32, MAX_TEMPLATE_PARAMETERS> TypeArguments{};
};

/// True when a value of `from` may initialize `to` without an explicit conversion.
[[nodiscard]] bool
IsImplicitlyConvertible(const TypeTable& types, foundation::uint32 from, foundation::uint32 to) noexcept;

/// @brief Bind arguments to a candidate's parameters: positional first, then by name.
/// @pre At most MAX_FUNCTION_PARAMETERS arguments and parameters.
[[nodiscard]] bool BindArguments(foundation::BorrowedView<const ArgumentFacts> arguments,
                                 foundation::BorrowedView<const ParameterFacts> parameters,
                                 const TypeTable& types,
                                 BindingResult& outResult) noexcept;
} // namespace ludus::tooling::shader_language::internal
