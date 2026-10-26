#pragma once

// Opt-in checked arithmetic for the native Ludus integer aliases. Operands are
// copied; output may alias an operand and is unchanged on failure. No logging,
// assertions, allocation, saturation, or exceptions, including on overflow.
// Thanks to Eric Lengyel, "Bit Hacks for Games", Game Engine Gems 2, chapter 24,
// pp. 391-401, for the width/signed-minimum edge cases. Our independently written
// helpers check representable bounds before arithmetic instead of using the
// chapter's bit hacks. This also works with MSVC, without LLVM overflow builtins.
// Thanks to the LLVM Project, Clang Language Extensions, "Checked Arithmetic
// Builtins", used by the original implementation:
// https://clang.llvm.org/docs/LanguageExtensions.html#checked-arithmetic-builtins
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

namespace detail
{
// The closed alias set has binary, padding-free representations verified by
// src/types.cpp. Form bounds in the unsigned domain; never shift a signed bit
// into the sign position or negate the signed minimum. Keep <limits> out of
// this public header (ADR 0003).
template <Integer ValueType>
inline constexpr ValueType INTEGER_MAXIMUM = []() constexpr noexcept {
    using UnsignedType = std::make_unsigned_t<ValueType>;
    constexpr UnsignedType ALL_BITS = static_cast<UnsignedType>(~UnsignedType{0});
    if constexpr (std::is_signed_v<ValueType> == true)
    {
        return static_cast<ValueType>(ALL_BITS >> 1);
    }
    else
    {
        return static_cast<ValueType>(ALL_BITS);
    }
}();

template <Integer ValueType>
inline constexpr ValueType INTEGER_MINIMUM = []() constexpr noexcept {
    if constexpr (std::is_signed_v<ValueType> == true)
    {
        return static_cast<ValueType>(-INTEGER_MAXIMUM<ValueType> - 1);
    }
    else
    {
        return ValueType{0};
    }
}();
} // namespace detail

/// @brief Convert an integer only when its value fits the destination range.
///
/// @return True on success; false preserves outValue. Output may alias the input.
/// Bool, characters, enums, floats and qualified outputs are excluded; aliases
/// cannot distinguish int32 from native int.
template <Integer DestinationType, Integer SourceType>
[[nodiscard]] constexpr bool TryIntegerCast(SourceType value, DestinationType& outValue) noexcept
{
    if (std::in_range<DestinationType>(value) == false)
    {
        return false;
    }
    // NOLINTNEXTLINE(bugprone-signed-char-misuse): int8 is numeric; preserve negative values.
    outValue = static_cast<DestinationType>(value);
    return true;
}

/// Add without wrapping or saturation; false preserves outValue, including aliased operands.
template <Integer ValueType>
[[nodiscard]] constexpr bool TryAdd(ValueType left, ValueType right, ValueType& outValue) noexcept
{
    constexpr ValueType MAXIMUM = detail::INTEGER_MAXIMUM<ValueType>;
    if constexpr (std::is_unsigned_v<ValueType> == true)
    {
        if (left > MAXIMUM - right)
        {
            return false;
        }
    }
    else
    {
        constexpr ValueType MINIMUM = detail::INTEGER_MINIMUM<ValueType>;
        if ((right > 0 && left > MAXIMUM - right) || (right < 0 && left < MINIMUM - right))
        {
            return false;
        }
    }
    outValue = static_cast<ValueType>(left + right);
    return true;
}

/// Subtract without wrapping or saturation; false preserves outValue, including aliased operands.
template <Integer ValueType>
[[nodiscard]] constexpr bool TrySubtract(ValueType left, ValueType right, ValueType& outValue) noexcept
{
    if constexpr (std::is_unsigned_v<ValueType> == true)
    {
        if (left < right)
        {
            return false;
        }
    }
    else
    {
        constexpr ValueType MAXIMUM = detail::INTEGER_MAXIMUM<ValueType>;
        constexpr ValueType MINIMUM = detail::INTEGER_MINIMUM<ValueType>;
        if ((right > 0 && left < MINIMUM + right) || (right < 0 && left > MAXIMUM + right))
        {
            return false;
        }
    }
    outValue = static_cast<ValueType>(left - right);
    return true;
}

/// Multiply without wrapping or saturation; false preserves outValue, including aliased operands.
template <Integer ValueType>
[[nodiscard]] constexpr bool TryMultiply(ValueType left, ValueType right, ValueType& outValue) noexcept
{
    constexpr ValueType MAXIMUM = detail::INTEGER_MAXIMUM<ValueType>;
    if constexpr (std::is_unsigned_v<ValueType> == true)
    {
        if (right != 0 && left > MAXIMUM / right)
        {
            return false;
        }
    }
    else
    {
        constexpr ValueType MINIMUM = detail::INTEGER_MINIMUM<ValueType>;
        if (left > 0)
        {
            if ((right > 0 && left > MAXIMUM / right) || (right < 0 && right < MINIMUM / left))
            {
                return false;
            }
        }
        else if (left < 0)
        {
            // All divisors are nonzero and MINIMUM / -1 is never evaluated.
            if ((right > 0 && left < MINIMUM / right) || (right < 0 && left < MAXIMUM / right))
            {
                return false;
            }
        }
    }
    outValue = static_cast<ValueType>(left * right);
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
