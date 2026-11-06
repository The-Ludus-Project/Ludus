#pragma once

// Opt-in portable signed 128-bit integer for exact intermediate arithmetic on
// every supported compiler, including MSVC, which has no native 128-bit type.
// Arithmetic is checked like checked_integer.hpp: operands are copied, output
// may alias an operand and is unchanged on failure. No logging, assertions,
// allocation, saturation, or exceptions.
// Thanks to Donald E. Knuth, "The Art of Computer Programming, Volume 2:
// Seminumerical Algorithms", 3rd edition, section 4.3.1, Algorithm M, for the
// schoolbook multiplication of 32-bit digits used for the 64x64-bit product.
// Thanks to the LLVM Project, compiler-rt builtins __floattidf and __fixdfti
// (https://github.com/llvm/llvm-project/tree/main/compiler-rt/lib/builtins),
// whose native __int128 conversions are the oracle the tests compare against.
// Review and departures: docs/architecture/primitive-types.md.
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/types.h>

namespace ludus::foundation::core
{
namespace detail
{
inline constexpr uint32 INT128_WORD_BITS = 64;
inline constexpr uint32 INT128_HALF_WORD_BITS = 32;
inline constexpr uint64 INT128_HALF_WORD_MASK = 0xFFFF'FFFFULL;
/// Bit 63 of a word: the sign bit of the high word.
inline constexpr uint64 INT128_SIGN_BIT = uint64{1} << (INT128_WORD_BITS - 1);
/// 2^64 and 2^127 as float64; both are exact.
inline constexpr float64 INT128_TWO_POW_64 = 18446744073709551616.0;
inline constexpr float64 INT128_TWO_POW_127 = 170141183460469231731687303715884105728.0;
} // namespace detail

/// @brief Two's-complement signed 128-bit integer with value semantics.
///
/// A value type for exact intermediates that may exceed 64 bits, such as
/// products of two int64 values. It has no wrapping arithmetic operators: use
/// the checked TryAdd, TrySubtract and TryMultiply overloads, which fail
/// instead of overflowing. Every operation is constexpr and portable; the
/// representation is two 64-bit words, independent of host byte order.
class Int128 final
{
public:
    /// Zero.
    constexpr Int128() noexcept = default;

    /// Sign-extend a 64-bit value.
    constexpr explicit Int128(int64 value) noexcept
        : mLow(static_cast<uint64>(value)), mHigh(value < 0 ? ALL_BITS : uint64{0})
    {
    }

    /// @brief Value from its two's-complement words.
    /// @param high Bits 64 to 127; bit 63 of this word is the sign.
    /// @param low Bits 0 to 63.
    [[nodiscard]] static constexpr Int128 FromWords(uint64 high, uint64 low) noexcept
    {
        Int128 value;
        value.mHigh = high;
        value.mLow = low;
        return value;
    }

    /// Bits 64 to 127 of the two's-complement representation.
    [[nodiscard]] constexpr uint64 GetHighWord() const noexcept
    {
        return mHigh;
    }

    /// Bits 0 to 63 of the two's-complement representation.
    [[nodiscard]] constexpr uint64 GetLowWord() const noexcept
    {
        return mLow;
    }

    /// True for values below zero.
    [[nodiscard]] constexpr bool IsNegative() const noexcept
    {
        return (mHigh & SIGN_BIT) != 0;
    }

    /// Exact equality.
    friend constexpr bool operator==(Int128 left, Int128 right) noexcept = default;

    /// Exact signed ordering.
    friend constexpr bool operator<(Int128 left, Int128 right) noexcept
    {
        if (left.mHigh != right.mHigh)
        {
            // Flipping the sign bit maps signed order onto unsigned order.
            return (left.mHigh ^ SIGN_BIT) < (right.mHigh ^ SIGN_BIT);
        }
        return left.mLow < right.mLow;
    }

    /// Exact signed ordering.
    friend constexpr bool operator>(Int128 left, Int128 right) noexcept
    {
        return right < left;
    }

    /// Exact signed ordering.
    friend constexpr bool operator<=(Int128 left, Int128 right) noexcept
    {
        return (right < left) == false;
    }

    /// Exact signed ordering.
    friend constexpr bool operator>=(Int128 left, Int128 right) noexcept
    {
        return (left < right) == false;
    }

private:
    static constexpr uint64 ALL_BITS = ~uint64{0};
    static constexpr uint64 SIGN_BIT = detail::INT128_SIGN_BIT;

    uint64 mLow = 0;
    uint64 mHigh = 0;
};

/// Largest Int128 value, 2^127 - 1.
inline constexpr Int128 INT128_MAXIMUM = Int128::FromWords(~detail::INT128_SIGN_BIT, ~uint64{0});
/// Smallest Int128 value, -2^127.
inline constexpr Int128 INT128_MINIMUM = Int128::FromWords(detail::INT128_SIGN_BIT, 0);

namespace detail
{
/// Unsigned 128-bit magnitude as two words.
struct Int128Magnitude final
{
    uint64 High = 0;
    uint64 Low = 0;
};

/// Two's-complement negation of the words; maps -2^127 to the magnitude 2^127.
[[nodiscard]] constexpr Int128Magnitude NegateInt128Words(uint64 high, uint64 low) noexcept
{
    const uint64 negatedLow = ~low + 1;
    const uint64 negatedHigh = ~high + (negatedLow == 0 ? 1 : 0);
    return Int128Magnitude{ .High = negatedHigh, .Low = negatedLow };
}

[[nodiscard]] constexpr Int128Magnitude GetInt128Magnitude(Int128 value) noexcept
{
    if (value.IsNegative() == true)
    {
        return NegateInt128Words(value.GetHighWord(), value.GetLowWord());
    }
    return Int128Magnitude{ .High = value.GetHighWord(), .Low = value.GetLowWord() };
}

/// Full 128-bit product of two 64-bit words (Knuth 4.3.1 Algorithm M, base 2^32).
[[nodiscard]] constexpr Int128Magnitude MultiplyInt128Words(uint64 left, uint64 right) noexcept
{
    const uint64 leftLow = left & INT128_HALF_WORD_MASK;
    const uint64 leftHigh = left >> INT128_HALF_WORD_BITS;
    const uint64 rightLow = right & INT128_HALF_WORD_MASK;
    const uint64 rightHigh = right >> INT128_HALF_WORD_BITS;
    const uint64 lowLow = leftLow * rightLow;
    const uint64 lowHigh = leftLow * rightHigh;
    const uint64 highLow = leftHigh * rightLow;
    const uint64 highHigh = leftHigh * rightHigh;
    // Each addend is below 2^32, so the middle column cannot overflow 64 bits.
    const uint64 middle =
        (lowLow >> INT128_HALF_WORD_BITS) + (lowHigh & INT128_HALF_WORD_MASK) + (highLow & INT128_HALF_WORD_MASK);
    return Int128Magnitude
    {
        .High = highHigh + (lowHigh >> INT128_HALF_WORD_BITS) + (highLow >> INT128_HALF_WORD_BITS) +
                (middle >> INT128_HALF_WORD_BITS),
        .Low = (middle << INT128_HALF_WORD_BITS) | (lowLow & INT128_HALF_WORD_MASK),
    };
}

/// Number of significant bits in a nonzero word.
[[nodiscard]] constexpr uint32 GetInt128WordBitLength(uint64 word) noexcept
{
    uint32 length = 0;
    while (word != 0)
    {
        word >>= 1;
        ++length;
    }
    return length;
}
} // namespace detail

/// Add without wrapping or saturation; false preserves outValue, including aliased operands.
[[nodiscard]] constexpr bool TryAdd(Int128 left, Int128 right, Int128& outValue) noexcept
{
    const uint64 low = left.GetLowWord() + right.GetLowWord();
    const uint64 carry = low < left.GetLowWord() ? 1 : 0;
    const Int128 sum = Int128::FromWords(left.GetHighWord() + right.GetHighWord() + carry, low);
    // Overflow exactly when both operands share a sign that the sum lacks.
    if (left.IsNegative() == right.IsNegative() && sum.IsNegative() != left.IsNegative())
    {
        return false;
    }
    outValue = sum;
    return true;
}

/// Subtract without wrapping or saturation; false preserves outValue, including aliased operands.
[[nodiscard]] constexpr bool TrySubtract(Int128 left, Int128 right, Int128& outValue) noexcept
{
    const uint64 low = left.GetLowWord() - right.GetLowWord();
    const uint64 borrow = left.GetLowWord() < right.GetLowWord() ? 1 : 0;
    const Int128 difference = Int128::FromWords(left.GetHighWord() - right.GetHighWord() - borrow, low);
    // Overflow exactly when the operand signs differ and the result takes the subtrahend's sign.
    if (left.IsNegative() != right.IsNegative() && difference.IsNegative() != left.IsNegative())
    {
        return false;
    }
    outValue = difference;
    return true;
}

/// Multiply without wrapping or saturation; false preserves outValue, including aliased operands.
[[nodiscard]] constexpr bool TryMultiply(Int128 left, Int128 right, Int128& outValue) noexcept
{
    const detail::Int128Magnitude leftMagnitude = detail::GetInt128Magnitude(left);
    const detail::Int128Magnitude rightMagnitude = detail::GetInt128Magnitude(right);
    if (leftMagnitude.High != 0 && rightMagnitude.High != 0)
    {
        return false;
    }
    detail::Int128Magnitude product = detail::MultiplyInt128Words(leftMagnitude.Low, rightMagnitude.Low);
    // At most one operand has a high word; its cross product lands in the high word.
    const uint64 crossFactor = leftMagnitude.High != 0 ? leftMagnitude.High : rightMagnitude.High;
    const uint64 crossOther = leftMagnitude.High != 0 ? rightMagnitude.Low : leftMagnitude.Low;
    if (crossFactor != 0)
    {
        const detail::Int128Magnitude cross = detail::MultiplyInt128Words(crossFactor, crossOther);
        const uint64 high = product.High + cross.Low;
        if (cross.High != 0 || high < product.High)
        {
            return false;
        }
        product.High = high;
    }
    const bool isNegative = left.IsNegative() != right.IsNegative();
    const Int128 limit = isNegative == true ? INT128_MINIMUM : INT128_MAXIMUM;
    const detail::Int128Magnitude limitMagnitude = detail::GetInt128Magnitude(limit);
    if (product.High > limitMagnitude.High || (product.High == limitMagnitude.High && product.Low > limitMagnitude.Low))
    {
        return false;
    }
    if (isNegative == true)
    {
        const detail::Int128Magnitude negated = detail::NegateInt128Words(product.High, product.Low);
        outValue = Int128::FromWords(negated.High, negated.Low);
    }
    else
    {
        outValue = Int128::FromWords(product.High, product.Low);
    }
    return true;
}

/// @brief Convert an Int128 only when its value fits the destination integer range.
/// @return True on success; false preserves outValue.
template <Integer DestinationType>
[[nodiscard]] constexpr bool TryIntegerCast(Int128 value, DestinationType& outValue) noexcept
{
    const uint64 signExtension = value.IsNegative() == true ? ~uint64{0} : uint64{0};
    // The value fits int64 when the high word only repeats the low word's sign bit.
    const bool isInt64 = value.GetHighWord() == signExtension &&
                         (value.GetLowWord() & detail::INT128_SIGN_BIT) == (signExtension & detail::INT128_SIGN_BIT);
    if (isInt64 == true)
    {
        return TryIntegerCast(static_cast<int64>(value.GetLowWord()), outValue);
    }
    if (value.IsNegative() == false && value.GetHighWord() == 0)
    {
        return TryIntegerCast(value.GetLowWord(), outValue);
    }
    return false;
}

/// @brief Nearest float64 to value, ties to even, like a native 128-bit conversion.
///
/// Values beyond 2^53 in magnitude round; every Int128 is within float64 range.
/// @note The result assumes the default round-to-nearest floating-point mode.
[[nodiscard]] constexpr float64 ConvertToFloat64(Int128 value) noexcept
{
    int64 narrowValue = 0;
    if (TryIntegerCast(value, narrowValue) == true)
    {
        return static_cast<float64>(narrowValue);
    }
    const detail::Int128Magnitude magnitude = detail::GetInt128Magnitude(value);
    float64 result = 0.0;
    if (magnitude.High == 0)
    {
        result = static_cast<float64>(magnitude.Low);
    }
    else
    {
        // Keep the top 64 significant bits and fold the discarded bits into a
        // sticky bit below the rounding position, then scale by a power of two.
        const uint32 shift = detail::GetInt128WordBitLength(magnitude.High);
        uint64 top = magnitude.High;
        bool hasDiscardedBits = magnitude.Low != 0;
        if (shift < detail::INT128_WORD_BITS)
        {
            top = (magnitude.High << (detail::INT128_WORD_BITS - shift)) | (magnitude.Low >> shift);
            hasDiscardedBits = (magnitude.Low << (detail::INT128_WORD_BITS - shift)) != 0;
        }
        if (hasDiscardedBits == true)
        {
            top |= 1;
        }
        const float64 scale = static_cast<float64>(uint64{1} << (shift - 1)) * 2.0;
        result = static_cast<float64>(top) * scale;
    }
    return value.IsNegative() == true ? -result : result;
}

/// @brief Truncate a float64 toward zero into an Int128.
/// @return False for NaN, infinities and values whose truncation is outside
/// [-2^127, 2^127 - 1]; false preserves outValue.
[[nodiscard]] constexpr bool TryTruncateToInt128(float64 value, Int128& outValue) noexcept
{
    // NaN fails both comparisons.
    if ((value >= -detail::INT128_TWO_POW_127 && value < detail::INT128_TWO_POW_127) == false)
    {
        return false;
    }
    const float64 magnitude = value < 0.0 ? -value : value;
    uint64 high = 0;
    uint64 low = 0;
    if (magnitude < detail::INT128_TWO_POW_64)
    {
        low = static_cast<uint64>(magnitude);
    }
    else
    {
        // Above 2^64 the value is an integer with at most 53 significant bits, so
        // the quotient, its product and the remainder below are all exact.
        high = static_cast<uint64>(magnitude / detail::INT128_TWO_POW_64);
        low = static_cast<uint64>(magnitude - static_cast<float64>(high) * detail::INT128_TWO_POW_64);
    }
    if (value < 0.0)
    {
        const detail::Int128Magnitude negated = detail::NegateInt128Words(high, low);
        outValue = Int128::FromWords(negated.High, negated.Low);
    }
    else
    {
        outValue = Int128::FromWords(high, low);
    }
    return true;
}
} // namespace ludus::foundation::core

namespace ludus::foundation
{
using core::ConvertToFloat64;
using core::Int128;
using core::INT128_MAXIMUM;
using core::INT128_MINIMUM;
using core::TryAdd;
using core::TryIntegerCast;
using core::TryMultiply;
using core::TrySubtract;
using core::TryTruncateToInt128;
} // namespace ludus::foundation
