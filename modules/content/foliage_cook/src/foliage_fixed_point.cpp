#include "internal/foliage_fixed_point.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/content/foliage_cook/foliage_cook.h>
#include <ludus/content/foliage_format/lufol.h>

#include <cstring>

// Original implementation. The float32 decomposition follows IEEE 754-2019
// binary32; rounding of exact values is the only approximation the cook makes.

namespace ludus::content::foliage_cook::internal
{
namespace
{
using foundation::Int128;

constexpr uint32 FLOAT32_FRACTION_BITS = 23;
constexpr uint32 FLOAT32_EXPONENT_MASK = 0xffU;
constexpr uint32 FLOAT32_FRACTION_MASK = (uint32{1} << FLOAT32_FRACTION_BITS) - 1;
constexpr uint32 FLOAT32_SIGN_SHIFT = 31;
/// Bias plus fraction width: a normal value is (fraction | 2^23) * 2^(exponent - 150).
constexpr int32 FLOAT32_NORMAL_EXPONENT_OFFSET = 150;
/// Exponent of the subnormal binary32 values, fraction * 2^-149.
constexpr int32 FLOAT32_SUBNORMAL_EXPONENT = -149;

constexpr uint32 WORD_BITS = 64;
constexpr uint32 INT128_BITS = 128;
constexpr uint32 INT128_LAST_BIT = INT128_BITS - 1;
/// Largest power of two whose Int128 factor fits a signed 64-bit constructor argument.
constexpr uint32 SHIFT_STEP = 62;
constexpr uint32 HALF_WORD_BITS = 32;
constexpr uint64 HALF_WORD_MASK = 0xffffffffULL;
constexpr uint32 HALF_WORDS = 4;
/// 2^32: a per-slot probability of one.
constexpr uint64 CERTAIN_ADMISSION = uint64{1} << HALF_WORD_BITS;
/// log2 of SCALE_UNITS_PER_ONE.
constexpr uint32 SCALE_SHIFT = 12;
static_assert((uint32{1} << SCALE_SHIFT) == foliage_format::SCALE_UNITS_PER_ONE);

Int128 Square(int64 value) noexcept
{
    Int128 product;
    // |value| < 2^63 so the square is below 2^126.
    [[maybe_unused]] const bool isExact = foundation::TryMultiply(Int128{value}, Int128{value}, product);
    LUDUS_ASSERT(isExact == true);
    return product;
}

/// left * right for products the callers bound below 2^127.
Int128 MultiplyBounded(Int128 left, Int128 right) noexcept
{
    Int128 product;
    [[maybe_unused]] const bool isExact = foundation::TryMultiply(left, right, product);
    LUDUS_ASSERT(isExact == true);
    return product;
}

Int128 AddBounded(Int128 left, Int128 right) noexcept
{
    Int128 sum;
    [[maybe_unused]] const bool isExact = foundation::TryAdd(left, right, sum);
    LUDUS_ASSERT(isExact == true);
    return sum;
}

bool GetBit(Int128 value, uint32 bit) noexcept
{
    if (bit < WORD_BITS)
    {
        return ((value.GetLowWord() >> bit) & 1U) != 0;
    }
    return ((value.GetHighWord() >> (bit - WORD_BITS)) & 1U) != 0;
}

/// Quotient of a nonnegative value by a nonzero 32-bit divisor, by schoolbook division of 32-bit digits.
Int128 DivideNonnegative(Int128 value, uint32 divisor) noexcept
{
    const uint64 high = value.GetHighWord();
    const uint64 low = value.GetLowWord();
    const foundation::StaticArray<uint64, HALF_WORDS> digits = {
        high >> HALF_WORD_BITS,
        high & HALF_WORD_MASK,
        low >> HALF_WORD_BITS,
        low & HALF_WORD_MASK,
    };
    foundation::StaticArray<uint64, HALF_WORDS> quotient{};
    uint64 remainder = 0;
    for (uint32 i = 0; i < HALF_WORDS; ++i)
    {
        const uint64 current = (remainder << HALF_WORD_BITS) | digits[i];
        quotient[i] = current / divisor;
        remainder = current % divisor;
    }
    constexpr usize HIGH_DIGIT = 0;
    constexpr usize UPPER_MIDDLE_DIGIT = 1;
    constexpr usize LOWER_MIDDLE_DIGIT = 2;
    constexpr usize LOW_DIGIT = 3;
    return Int128::FromWords((quotient[HIGH_DIGIT] << HALF_WORD_BITS) | quotient[UPPER_MIDDLE_DIGIT],
                             (quotient[LOWER_MIDDLE_DIGIT] << HALF_WORD_BITS) | quotient[LOW_DIGIT]);
}
} // namespace

bool TryDecomposeFloat32(float32 value, DyadicValue& outValue) noexcept
{
    uint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32 exponent = (bits >> FLOAT32_FRACTION_BITS) & FLOAT32_EXPONENT_MASK;
    const uint32 fraction = bits & FLOAT32_FRACTION_MASK;
    if (exponent == FLOAT32_EXPONENT_MASK)
    {
        return false;
    }
    DyadicValue result;
    if (exponent == 0)
    {
        result.Mantissa = static_cast<int64>(fraction);
        result.Exponent = FLOAT32_SUBNORMAL_EXPONENT;
    }
    else
    {
        result.Mantissa = static_cast<int64>(fraction | (uint32{1} << FLOAT32_FRACTION_BITS));
        result.Exponent = static_cast<int32>(exponent) - FLOAT32_NORMAL_EXPONENT_OFFSET;
    }
    if ((bits >> FLOAT32_SIGN_SHIFT) != 0)
    {
        result.Mantissa = -result.Mantissa;
    }
    outValue = result;
    return true;
}

Int128 ShiftRightFloor(Int128 value, uint32 shift) noexcept
{
    LUDUS_ASSERT(shift >= 1 && shift <= INT128_LAST_BIT);
    // Arithmetic shifts of signed values are exact two's-complement floors since C++20.
    const int64 high = static_cast<int64>(value.GetHighWord());
    const uint64 low = value.GetLowWord();
    if (shift < WORD_BITS)
    {
        const uint64 shiftedLow = (low >> shift) | (static_cast<uint64>(high) << (WORD_BITS - shift));
        return Int128::FromWords(static_cast<uint64>(high >> shift), shiftedLow);
    }
    const int64 shiftedLow = high >> (shift - WORD_BITS);
    return Int128::FromWords(static_cast<uint64>(high >> (WORD_BITS - 1)), static_cast<uint64>(shiftedLow));
}

Int128 ShiftRightRoundHalfUp(Int128 value, uint32 shift) noexcept
{
    LUDUS_ASSERT(shift >= 1);
    if (shift >= INT128_BITS)
    {
        // |value| < 2^127 <= 2^(shift - 1), so value / 2^shift lies in [-1/2, 1/2) and rounds to zero.
        return Int128{};
    }
    // floor((v + 2^(s-1)) / 2^s) = floor(v / 2^s) + bit (s - 1) of v, which cannot overflow.
    const Int128 floor = ShiftRightFloor(value, shift);
    if (GetBit(value, shift - 1) == true)
    {
        return AddBounded(floor, Int128{1});
    }
    return floor;
}

bool TryShiftLeft(Int128 value, uint32 shift, Int128& outValue) noexcept
{
    Int128 result = value;
    uint32 remaining = shift;
    while (remaining > 0)
    {
        if (result == Int128{})
        {
            break;
        }
        const uint32 step = remaining < SHIFT_STEP ? remaining : SHIFT_STEP;
        if (foundation::TryMultiply(result, Int128{int64{1} << step}, result) == false)
        {
            return false;
        }
        remaining -= step;
    }
    outValue = result;
    return true;
}

// Origin, quantum, code is the order of the formula origin + quantum * code.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool TryRoundDyadicSum(DyadicValue origin, DyadicValue quantum, int64 code, int32 scaleBits, int64& outValue) noexcept
{
    Int128 quantumTerm;
    if (foundation::TryMultiply(Int128{quantum.Mantissa}, Int128{code}, quantumTerm) == false)
    {
        return false;
    }
    const bool hasOrigin = origin.Mantissa != 0;
    const bool hasQuantum = quantumTerm != Int128{};
    if (hasOrigin == false && hasQuantum == false)
    {
        outValue = 0;
        return true;
    }
    const int64 originExponent = int64{origin.Exponent} + scaleBits;
    const int64 quantumExponent = int64{quantum.Exponent} + scaleBits;
    int64 common = hasOrigin == true ? originExponent : quantumExponent;
    if (hasQuantum == true && quantumExponent < common)
    {
        common = quantumExponent;
    }
    Int128 sum;
    if (hasOrigin == true)
    {
        Int128 aligned;
        if (TryShiftLeft(Int128{origin.Mantissa}, static_cast<uint32>(originExponent - common), aligned) == false)
        {
            return false;
        }
        sum = aligned;
    }
    if (hasQuantum == true)
    {
        Int128 aligned;
        if (TryShiftLeft(quantumTerm, static_cast<uint32>(quantumExponent - common), aligned) == false ||
            foundation::TryAdd(sum, aligned, sum) == false)
        {
            return false;
        }
    }
    Int128 rounded;
    if (common >= 0)
    {
        if (TryShiftLeft(sum, static_cast<uint32>(common), rounded) == false)
        {
            return false;
        }
    }
    else
    {
        rounded = ShiftRightRoundHalfUp(sum, static_cast<uint32>(-common));
    }
    return foundation::TryIntegerCast(rounded, outValue);
}

int64 FloorDivide(int64 value, int64 divisor) noexcept
{
    LUDUS_ASSERT(divisor > 0);
    int64 quotient = value / divisor;
    if (value % divisor != 0 && value < 0)
    {
        --quotient;
    }
    return quotient;
}

int64 FloorModulo(int64 value, int64 divisor) noexcept
{
    const int64 remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

uint64 ScaleRadius(uint64 value, uint32 scale) noexcept
{
    const uint64 roundUp = (uint64{1} << SCALE_SHIFT) - 1;
    return (value * scale + roundUp) >> SCALE_SHIFT;
}

uint32 ComputeSlopeUnits(int64 gradientX, int64 gradientZ, int64 spacing) noexcept
{
    // Largest k with k^2 * spacing^2 <= UNITS^2 * (gx^2 + gz^2). The bounds keep the left side below 2^126
    // and the right side below 2^117.
    const Int128 spacingSquared = Square(spacing);
    const Int128 unitsSquared = Square(int64{FOLIAGE_SLOPE_UNITS_PER_ONE});
    const Int128 target = MultiplyBounded(unitsSquared, AddBounded(Square(gradientX), Square(gradientZ)));
    uint32 lowest = 0;
    uint32 highest = FOLIAGE_MAXIMUM_SLOPE_UNITS;
    while (lowest < highest)
    {
        const uint32 middle = lowest + (highest - lowest + 1) / 2;
        if (MultiplyBounded(Square(int64{middle}), spacingSquared) <= target)
        {
            lowest = middle;
        }
        else
        {
            highest = middle - 1;
        }
    }
    return lowest;
}

int16 ComputeNormalComponent(int64 component, int64 gradientX, int64 gradientZ, int64 spacing) noexcept
{
    if (component == 0)
    {
        return 0;
    }
    // Largest k with k^2 * D <= NORMAL_UNITS^2 * component^2; D < 2^94 and k^2 < 2^30.
    const Int128 lengthSquared = AddBounded(AddBounded(Square(gradientX), Square(gradientZ)), Square(spacing));
    const Int128 target = MultiplyBounded(Square(int64{foliage_format::NORMAL_UNITS}), Square(component));
    uint32 lowest = 0;
    uint32 highest = foliage_format::NORMAL_UNITS;
    while (lowest < highest)
    {
        const uint32 middle = lowest + (highest - lowest + 1) / 2;
        if (MultiplyBounded(Square(int64{middle}), lengthSquared) <= target)
        {
            lowest = middle;
        }
        else
        {
            highest = middle - 1;
        }
    }
    const int32 magnitude = static_cast<int32>(lowest);
    return static_cast<int16>(component > 0 ? -magnitude : magnitude);
}

bool TryComputeAdmissionThreshold(uint32 density,
                                  uint32 cellSize,
                                  DyadicValue spacingMetres,
                                  uint32 slots,
                                  uint64& outThreshold) noexcept
{
    LUDUS_ASSERT(slots > 0 && spacingMetres.Mantissa > 0);
    // density < 2^32, cellSize <= 2^16 and the mantissa < 2^24 keep the product below 2^112.
    Int128 product = MultiplyBounded(Int128{int64{density}}, Int128{int64{cellSize}});
    product = MultiplyBounded(product, Int128{int64{cellSize}});
    product = MultiplyBounded(product, Int128{spacingMetres.Mantissa});
    product = MultiplyBounded(product, Int128{spacingMetres.Mantissa});
    const int64 exponent = int64{spacingMetres.Exponent} * 2;
    Int128 scaled;
    if (exponent >= 0)
    {
        if (TryShiftLeft(product, static_cast<uint32>(exponent), scaled) == false)
        {
            return false;
        }
    }
    else
    {
        scaled = -exponent >= INT128_BITS ? Int128{} : ShiftRightFloor(product, static_cast<uint32>(-exponent));
    }
    // Nested floors of nonnegative values equal one floor of the exact quotient.
    const Int128 threshold = DivideNonnegative(scaled, slots);
    uint64 result = 0;
    if (foundation::TryIntegerCast(threshold, result) == false || result > CERTAIN_ADMISSION)
    {
        return false;
    }
    outThreshold = result;
    return true;
}
} // namespace ludus::content::foliage_cook::internal
