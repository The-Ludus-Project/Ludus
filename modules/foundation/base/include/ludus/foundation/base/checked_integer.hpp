#pragma once

// Opt-in checked arithmetic for the native Ludus integer aliases. Operands are
// copied; output may alias an operand and is unchanged on failure. No logging,
// assertions, allocation, saturation, or exceptions, including on overflow.
// Thanks to Eric Lengyel, "Bit Hacks for Games", Game Engine Gems 2, chapter 24,
// pp. 391-401, for the width/signed-minimum edge cases. Our independently written
// helpers use defined LLVM overflow operations instead of the chapter's bit hacks.
// Thanks to the LLVM Project, Clang Language Extensions, "Checked Arithmetic
// Builtins": https://clang.llvm.org/docs/LanguageExtensions.html#checked-arithmetic-builtins
// Thanks to ISO/IEC JTC1/SC22/WG21, C++ Working Draft [utility.intcmp], for the
// signedness-aware in_range contract: https://eel.is/c++draft/utility.intcmp
// Review and departures: docs/architecture/primitive-types.md.
#include <ludus/foundation/base/types.h>

#include <type_traits>
#include <utility>

namespace ludus::foundation::core
{
/// Exact unqualified Ludus unsigned integer alias; excludes bool, characters and enums.
template <typename ValueType>
concept UnsignedInteger =
    std::is_same_v<ValueType, uint8> || std::is_same_v<ValueType, uint16> || std::is_same_v<ValueType, uint32> ||
    std::is_same_v<ValueType, uint64> || std::is_same_v<ValueType, usize>;

/// Exact unqualified Ludus signed or unsigned integer alias; excludes bool, characters and enums.
template <typename ValueType>
concept Integer =
    UnsignedInteger<ValueType> || std::is_same_v<ValueType, int8> || std::is_same_v<ValueType, int16> ||
    std::is_same_v<ValueType, int32> || std::is_same_v<ValueType, int64> || std::is_same_v<ValueType, isize>;

/// @brief Convert an integer only when its value fits the destination range.
///
/// @return True on success; false preserves out. Output may alias the input.
/// Bool, characters, enums, floats and qualified outputs are excluded; aliases
/// cannot distinguish int32 from native int.
template <Integer DestinationType, Integer SourceType>
[[nodiscard]] constexpr bool TryIntegerCast(SourceType value, DestinationType& out) noexcept
{
    if (std::in_range<DestinationType>(value) == false)
    {
        return false;
    }
    // NOLINTNEXTLINE(bugprone-signed-char-misuse): int8 is numeric; preserve negative values.
    out = static_cast<DestinationType>(value);
    return true;
}

/// Add without wrapping or saturation; false preserves out, including aliased operands.
template <Integer ValueType>
[[nodiscard]] constexpr bool TryAdd(ValueType left, ValueType right, ValueType& out) noexcept
{
    ValueType result{};
    if (__builtin_add_overflow(left, right, &result))
    {
        return false;
    }
    out = result;
    return true;
}

/// Subtract without wrapping or saturation; false preserves out, including aliased operands.
template <Integer ValueType>
[[nodiscard]] constexpr bool TrySubtract(ValueType left, ValueType right, ValueType& out) noexcept
{
    ValueType result{};
    if (__builtin_sub_overflow(left, right, &result))
    {
        return false;
    }
    out = result;
    return true;
}

/// Multiply without wrapping or saturation; false preserves out, including aliased operands.
template <Integer ValueType>
[[nodiscard]] constexpr bool TryMultiply(ValueType left, ValueType right, ValueType& out) noexcept
{
    ValueType result{};
    if (__builtin_mul_overflow(left, right, &result))
    {
        return false;
    }
    out = result;
    return true;
}
} // namespace ludus::foundation::core

namespace ludus::foundation
{
using core::Integer;
using core::TryAdd;
using core::TryIntegerCast;
using core::TryMultiply;
using core::TrySubtract;
using core::UnsignedInteger;
} // namespace ludus::foundation
