#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/tooling/shader_language/semantic.hpp>

namespace ludus::tooling::shader_language::internal
{
/// Kind of one step from a variable to the storage an lvalue names.
enum class AccessStepKind : foundation::uint8
{
    Field,         ///< Value: the field symbol.
    ConstantIndex, ///< Value: the element index.
    DynamicIndex,  ///< An index known only at run time; may be any element.
    Swizzle,       ///< Value: a bit mask of the vector components named.
};

struct AccessStep final
{
    AccessStepKind Kind = AccessStepKind::Field;
    foundation::uint32 Value = 0;
};

/// Steps recorded per path; deeper paths are compared conservatively.
inline constexpr foundation::usize MAX_ACCESS_STEPS = 16;

/// @brief The storage an lvalue names: a variable or parameter and the steps into it.
struct AccessPath final
{
    foundation::uint32 Root = NO_SYMBOL;
    foundation::StaticArray<AccessStep, MAX_ACCESS_STEPS> Steps{};
    foundation::usize StepCount = 0;
};

/// @brief True when two paths may name overlapping storage.
///
/// Distinct fields, distinct constant indices and disjoint swizzles are separate; a run-time index
/// may match any element, and a path that is a prefix of the other overlaps it.
[[nodiscard]] bool MayOverlap(const AccessPath& left, const AccessPath& right) noexcept;
} // namespace ludus::tooling::shader_language::internal
