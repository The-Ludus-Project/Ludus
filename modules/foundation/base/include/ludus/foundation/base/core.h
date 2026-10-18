#pragma once

// -----------------------------------------------------------------------------
// THE Ludus foundational header (Band 1).
//
// Including this header GUARANTEES a translation unit has the universal Ludus
// vocabulary: fixed-width primitive types, build/compiler/platform-family/arch
// configuration macros, the codegen attribute/hint macros, the low-level
// utility primitives Move/Forward, the DerivedFrom concept and nullptr_t, and
// the assertion entry points (LUDUS_ASSERT/REQUIRE/CHECK/FATAL).
//
// This is the ONE header a Ludus file may rely on being universally available.
// Every heavier facility — containers, strings, logging, profiling, math,
// threading, filesystem, smart pointers beyond this file — is included
// EXPLICITLY by the files that use it (see the transitive-include policy in
// docs/architecture/foundational-headers.md §14 and ADR 0007).
//
// COST CONTRACT (enforced by the build-time budget, ADR 0005, and the
// forbidden-include check in tools/check_foundational_includes.py):
// core.h and its Band 0 includes MUST pull only <cstdint>/<cstddef> (via
// types.h) and <type_traits>/<utility>. They MUST NOT include <string>,
// <string_view>, <format>, <chrono>, <filesystem>, any container header, any
// logging/profiling/platform header, or anything that instantiates a
// substantial template or adds ABI surface.
// -----------------------------------------------------------------------------

#include <ludus/foundation/base/assert.hpp> // LUDUS_ASSERT/REQUIRE/CHECK/FATAL
#include <ludus/foundation/base/compiler.h> // LUDUS_INLINE/NOINLINE/COLD/LIKELY/DEBUG_BREAK/...
#include <ludus/foundation/base/config.h>   // LUDUS_PLATFORM_* / LUDUS_ARCH_* / LUDUS_BUILD_*
#include <ludus/foundation/base/types.h>    // uint8..uint64/int*/usize/isize/float32/float64

#include <type_traits>
#include <utility>

namespace ludus::foundation::core
{

/// nullptr_t: the Ludus-scoped spelling of the null pointer literal's type.
using nullptr_t = decltype(nullptr);

/// @brief DerivedFrom: BaseType is a proper (non-identical) base of DerivedType.
///
/// Used by UniquePtr's converting assignment and other polymorphic-ownership seams.
template <typename DerivedType, typename BaseType>
concept DerivedFrom = std::is_base_of_v<BaseType, DerivedType> && std::is_same_v<DerivedType, BaseType> == false;

/// @brief Move: unconditional cast to an rvalue reference.
///
/// The Ludus-named, verb-first spelling of std::move; call sites read as engine vocabulary and never import the std
/// name. Free and constexpr.
template <typename ValueType>
[[nodiscard]] constexpr std::remove_reference_t<ValueType>&& Move(ValueType&& value) noexcept
{
    return static_cast<std::remove_reference_t<ValueType>&&>(value);
}

/// @brief Forward: perfect-forward a value while preserving its value category.
///
/// The two overloads mirror std::forward's contract (the rvalue overload must not be asked to forward as an
/// lvalue).
template <typename ValueType>
[[nodiscard]] constexpr ValueType&& Forward(std::remove_reference_t<ValueType>& value) noexcept
{
    return static_cast<ValueType&&>(value);
}

/// Preserve an rvalue's category when forwarding; ValueType must not be an lvalue reference.
template <typename ValueType>
[[nodiscard]] constexpr ValueType&& Forward(std::remove_reference_t<ValueType>&& value) noexcept
{
    static_assert(std::is_lvalue_reference_v<ValueType> == false, "Forward must not turn an rvalue into an lvalue");
    return static_cast<ValueType&&>(value);
}

} // namespace ludus::foundation::core

// Re-export the short-scoped names into ludus::foundation, matching how types.h
// surfaces uint32/usize/... so most engine code uses the shorter spelling.
namespace ludus::foundation
{
using core::DerivedFrom;
using core::Forward;
using core::Move;
using core::nullptr_t;
} // namespace ludus::foundation
