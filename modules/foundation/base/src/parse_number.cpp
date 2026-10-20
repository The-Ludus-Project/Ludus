#include <ludus/foundation/base/parse_number.hpp>

namespace ludus::foundation
{
namespace
{
// Exact rational conversion: compare decimal integers with binary32 values,
// then compare the adjacent-value midpoint. No approximate floating arithmetic.
// 1024 decimal digits plus binary32 scaling fit in 6144 bits.
/// Words of an Integer: 6144 bits.
constexpr usize INTEGER_WORDS = 192;
/// Bits per Integer word.
constexpr uint32 WORD_BITS = 32;
/// Little-endian base-2^32 magnitude. Length counts the words up to the highest
/// nonzero one, so arithmetic touches only significant words; values that would
/// exceed INTEGER_WORDS are truncated exactly as a fixed-width loop would.
struct Integer final
{
    uint32 Words[INTEGER_WORDS] = {};
    usize Length = 0;
    void Multiply(uint32 factor) noexcept
    {
        uint64 carry = 0;
        for (usize index = 0; index < Length; ++index)
        {
            const uint64 product = static_cast<uint64>(Words[index]) * factor + carry;
            Words[index] = static_cast<uint32>(product);
            carry = product >> WORD_BITS;
        }
        if (carry != 0 && Length < INTEGER_WORDS)
        {
            Words[Length++] = static_cast<uint32>(carry);
        }
        trim();
    }
    void Add(uint32 digit) noexcept
    {
        uint64 carry = digit;
        for (usize index = 0; index < INTEGER_WORDS && carry != 0; ++index)
        {
            carry += Words[index];
            Words[index] = static_cast<uint32>(carry);
            carry >>= WORD_BITS;
            if (index >= Length)
            {
                Length = index + 1;
            }
        }
        trim();
    }
    /// Multiply by 2^count, dropping bits beyond INTEGER_WORDS.
    void Shift(uint32 count) noexcept
    {
        if (Length == 0 || count == 0)
        {
            return;
        }
        const usize wordShift = count / WORD_BITS;
        const uint32 bitShift = count % WORD_BITS;
        if (wordShift >= INTEGER_WORDS)
        {
            *this = {};
            return;
        }
        const usize sourceTop = Length + 1 <= INTEGER_WORDS - wordShift ? Length + 1 : INTEGER_WORDS - wordShift;
        for (usize index = sourceTop + wordShift; index-- > wordShift;)
        {
            const usize source = index - wordShift;
            const uint32 high = source < Length ? Words[source] : 0;
            const uint32 low = source > 0 && source - 1 < Length ? Words[source - 1] : 0;
            Words[index] = bitShift == 0 ? high : (high << bitShift) | (low >> (WORD_BITS - bitShift));
        }
        for (usize index = 0; index < wordShift; ++index)
        {
            Words[index] = 0;
        }
        Length = sourceTop + wordShift;
        trim();
    }

private:
    void trim() noexcept
    {
        while (Length != 0 && Words[Length - 1] == 0)
        {
            --Length;
        }
    }
};
bool Digit(char value) noexcept
{
    return value >= '0' && value <= '9';
}
int32 Compare(const Integer& left, const Integer& right) noexcept
{
    if (left.Length != right.Length)
    {
        return left.Length < right.Length ? -1 : 1;
    }
    for (usize index = left.Length; index != 0; --index)
    {
        if (left.Words[index - 1] != right.Words[index - 1])
        {
            return left.Words[index - 1] < right.Words[index - 1] ? -1 : 1;
        }
    }
    return 0;
}
uint32 Significand(uint32 bits) noexcept
{
    return (bits & 0x7fffffU) | (bits >= 0x800000U ? 0x800000U : 0U);
}
int32 Exponent(uint32 bits) noexcept
{
    return bits < 0x800000U ? -149 : static_cast<int32>(bits >> 23) - 150;
}
struct Decimal final
{
    const Integer& Numerator;
    const Integer& Denominator;
};
struct Binary final
{
    uint32 Significand;
    int32 Exponent;
};
int32 CompareBinary(Decimal decimal, Binary binary) noexcept
{
    Integer left = decimal.Numerator;
    Integer right = decimal.Denominator;
    right.Multiply(binary.Significand);
    if (binary.Exponent < 0)
    {
        left.Shift(static_cast<uint32>(-binary.Exponent));
    }
    else
    {
        right.Shift(static_cast<uint32>(binary.Exponent));
    }
    return Compare(left, right);
}

// Byte copy preserves the exact representation without aliasing or unions.
void WriteBinary32(uint32 bits, float32& outValue) noexcept
{
    const uint8* source = reinterpret_cast<const uint8*>(&bits);
    uint8* destination = reinterpret_cast<uint8*>(&outValue);
    for (usize index = 0; index < sizeof(outValue); ++index)
    {
        destination[index] = source[index];
    }
}

// Exact fast path. Most inputs have at most 19 significant digits and a small
// decimal exponent, so the value is w * 10^e with w < 2^64. The quotient or
// product is formed exactly in 128-bit integers; the rounding bit and a sticky
// remainder then select the nearest binary32, ties to even. Results outside the
// normal binary32 range return false and use the general conversion above.
/// Bits in each half of a Wide value.
constexpr uint32 HALF_BITS = 64;
/// Binary32 precision, including the implicit leading bit.
constexpr uint32 SIGNIFICAND_BITS = 24;
/// Stored binary32 fraction bits and their mask.
constexpr uint32 FRACTION_BITS = SIGNIFICAND_BITS - 1;
constexpr uint32 FRACTION_MASK = (1U << FRACTION_BITS) - 1;
/// Exponent bias for a value written as an integer significand times 2^exponent.
constexpr int32 INTEGER_SIGNIFICAND_BIAS = 127 + static_cast<int32>(FRACTION_BITS);
/// Largest biased exponent of a finite binary32 value.
constexpr int32 MAX_FINITE_BIASED_EXPONENT = 254;
/// Binary32 sign bit.
constexpr uint32 SIGN_BIT = 0x80000000U;
/// Decimal digit base.
constexpr uint64 DECIMAL_BASE = 10;
/// The scaled quotient lies below 2^QUOTIENT_BITS and at or above 2^(QUOTIENT_BITS - 2).
constexpr uint32 QUOTIENT_BITS = 27;
constexpr int32 MAX_FAST_POSITIVE_EXPONENT = 19;
constexpr int32 MAX_FAST_NEGATIVE_EXPONENT = 27;
constexpr uint64 POWERS_OF_TEN[] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
    10000000000000000ULL,
    100000000000000000ULL,
    1000000000000000000ULL,
    10000000000000000000ULL,
};
struct Wide final
{
    uint64 High = 0;
    uint64 Low = 0;
};
uint32 GetBitLength(uint64 value) noexcept
{
    uint32 length = 0;
    for (uint32 step = HALF_BITS / 2; step != 0; step /= 2)
    {
        if ((value >> step) != 0)
        {
            value >>= step;
            length += step;
        }
    }
    return length + static_cast<uint32>(value);
}
uint32 GetBitLength(Wide value) noexcept
{
    return value.High != 0 ? HALF_BITS + GetBitLength(value.High) : GetBitLength(value.Low);
}
Wide MultiplyWide(uint64 left, uint64 right) noexcept
{
    constexpr uint64 LOW_HALF = (1ULL << WORD_BITS) - 1;
    const uint64 leftLow = left & LOW_HALF;
    const uint64 leftHigh = left >> WORD_BITS;
    const uint64 rightLow = right & LOW_HALF;
    const uint64 rightHigh = right >> WORD_BITS;
    const uint64 lowLow = (left & LOW_HALF) * (right & LOW_HALF);
    const uint64 lowHigh = leftLow * rightHigh;
    const uint64 highLow = leftHigh * rightLow;
    const uint64 highHigh = leftHigh * rightHigh;
    const uint64 middle = (lowLow >> WORD_BITS) + (lowHigh & LOW_HALF) + (highLow & LOW_HALF);
    return
    {
        .High = highHigh + (lowHigh >> WORD_BITS) + (highLow >> WORD_BITS) + (middle >> WORD_BITS),
        .Low = (lowLow & LOW_HALF) | (middle << WORD_BITS),
    };
}
/// @pre count < 128 and the shifted value fits in 128 bits.
Wide ShiftLeftWide(Wide value, uint32 count) noexcept
{
    if (count == 0)
    {
        return value;
    }
    if (count >= HALF_BITS)
    {
        return { .High = value.Low << (count - HALF_BITS), .Low = 0 };
    }
    return { .High = (value.High << count) | (value.Low >> (HALF_BITS - count)), .Low = value.Low << count };
}
/// @pre count < 128.
Wide ShiftRightWide(Wide value, uint32 count) noexcept
{
    if (count == 0)
    {
        return value;
    }
    if (count >= HALF_BITS)
    {
        return { .High = 0, .Low = value.High >> (count - HALF_BITS) };
    }
    return { .High = value.High >> count, .Low = (value.Low >> count) | (value.High << (HALF_BITS - count)) };
}
bool IsLessWide(Wide left, Wide right) noexcept
{
    return left.High != right.High ? left.High < right.High : left.Low < right.Low;
}
Wide SubtractWide(Wide left, Wide right) noexcept
{
    const uint64 borrow = left.Low < right.Low ? 1U : 0U;
    return { .High = left.High - right.High - borrow, .Low = left.Low - right.Low };
}
bool HasBitsBelow(Wide value, uint32 count) noexcept
{
    if (count == 0)
    {
        return false;
    }
    if (count >= HALF_BITS)
    {
        const uint32 highCount = count - HALF_BITS;
        const uint64 highMask = highCount == 0 ? 0 : ~0ULL >> (HALF_BITS - highCount);
        return value.Low != 0 || (value.High & highMask) != 0;
    }
    return (value.Low & (~0ULL >> (HALF_BITS - count))) != 0;
}
/// Round (value + fraction) * 2^-scale, where hasFraction marks a nonzero
/// fraction below value's least significant bit, to binary32 bits.
/// @return false when the result is not a normal binary32.
bool TryRoundToBinary32(Wide value, bool hasFraction, int32 scale, uint32& outBits) noexcept
{
    const uint32 length = GetBitLength(value);
    uint64 significand = 0;
    int32 exponent = -scale;
    if (length <= SIGNIFICAND_BITS)
    {
        // Exact unless a fraction remains; callers with a fraction supply >= 26 bits.
        significand = value.Low << (SIGNIFICAND_BITS - length);
        exponent -= static_cast<int32>(SIGNIFICAND_BITS - length);
    }
    else
    {
        const uint32 extra = length - SIGNIFICAND_BITS;
        significand = ShiftRightWide(value, extra).Low;
        const bool isRoundBitSet = (ShiftRightWide(value, extra - 1).Low & 1U) != 0;
        const bool isAboveHalf = hasFraction == true || HasBitsBelow(value, extra - 1) == true;
        if (isRoundBitSet == true && (isAboveHalf == true || (significand & 1U) != 0))
        {
            ++significand;
            if (significand == (1ULL << SIGNIFICAND_BITS))
            {
                significand >>= 1;
                ++exponent;
            }
        }
        exponent += static_cast<int32>(extra);
    }
    const int32 biasedExponent = exponent + INTEGER_SIGNIFICAND_BIAS;
    if (biasedExponent < 1 || biasedExponent > MAX_FINITE_BIASED_EXPONENT)
    {
        return false;
    }
    outBits =
        (static_cast<uint32>(biasedExponent) << FRACTION_BITS) | (static_cast<uint32>(significand) & FRACTION_MASK);
    return true;
}
/// Significand bounds and decimal scaling captured by the validated syntax scan.
struct FastDecimalDescription final
{
    usize IntegerStart = 0;
    usize FractionEnd = 0;
    int32 Exponent = 0;
};
/// @return false when the input needs the general conversion; outBits is then unchanged.
bool TryParseFast(const char* text, FastDecimalDescription description, uint32& outBits) noexcept
{
    constexpr uint64 MAX_BEFORE_DIGIT = (~0ULL - (DECIMAL_BASE - 1)) / DECIMAL_BASE;
    uint64 digits = 0;
    for (usize index = description.IntegerStart; index < description.FractionEnd; ++index)
    {
        if (Digit(text[index]) == false)
        {
            continue;
        }
        if (digits > MAX_BEFORE_DIGIT)
        {
            return false;
        }
        digits = digits * DECIMAL_BASE + static_cast<uint64>(text[index] - '0');
    }
    if (digits == 0)
    {
        outBits = 0;
        return true;
    }
    if (description.Exponent >= 0)
    {
        if (description.Exponent > MAX_FAST_POSITIVE_EXPONENT)
        {
            return false;
        }
        const Wide product = MultiplyWide(digits, POWERS_OF_TEN[static_cast<usize>(description.Exponent)]);
        return TryRoundToBinary32(product, false, 0, outBits);
    }
    const int32 negativeExponent = -description.Exponent;
    if (negativeExponent > MAX_FAST_NEGATIVE_EXPONENT)
    {
        return false;
    }
    // 10^k for k > 19 exceeds 64 bits; form it as an exact 10^19 * 10^(k - 19) product.
    Wide divisor = negativeExponent > MAX_FAST_POSITIVE_EXPONENT
                       ? MultiplyWide(POWERS_OF_TEN[static_cast<usize>(MAX_FAST_POSITIVE_EXPONENT)],
                                      POWERS_OF_TEN[static_cast<usize>(negativeExponent - MAX_FAST_POSITIVE_EXPONENT)])
                       : Wide{ .High = 0, .Low = POWERS_OF_TEN[static_cast<usize>(negativeExponent)] };
    // Scale so that the quotient lies in [2^25, 2^27): 24 result bits, a
    // rounding bit and at least one more bit, with the remainder as sticky.
    const int32 scale = static_cast<int32>(QUOTIENT_BITS - 1) + static_cast<int32>(GetBitLength(divisor)) -
                        static_cast<int32>(GetBitLength(digits));
    Wide remainder = { .High = 0, .Low = digits };
    if (scale >= 0)
    {
        remainder = ShiftLeftWide(remainder, static_cast<uint32>(scale));
    }
    else
    {
        divisor = ShiftLeftWide(divisor, static_cast<uint32>(-scale));
    }
    uint64 quotient = 0;
    for (uint32 bit = QUOTIENT_BITS; bit != 0; --bit)
    {
        const Wide shifted = ShiftLeftWide(divisor, bit - 1);
        if (IsLessWide(remainder, shifted) == false)
        {
            remainder = SubtractWide(remainder, shifted);
            quotient |= 1ULL << (bit - 1);
        }
    }
    const bool hasFraction = remainder.High != 0 || remainder.Low != 0;
    return TryRoundToBinary32({ .High = 0, .Low = quotient }, hasFraction, scale, outBits);
}
} // namespace
NumberParseStatus ParseUint64(const char* text, usize size, uint64& output) noexcept
{
    if (size > 1024)
    {
        return NumberParseStatus::TooLong;
    }
    if (text == nullptr || size == 0)
    {
        return NumberParseStatus::InvalidSyntax;
    }
    uint64 value = 0;
    bool overflow = false;
    for (usize index = 0; index < size; ++index)
    {
        if (Digit(text[index]) == false)
        {
            return NumberParseStatus::InvalidSyntax;
        }
        const uint64 digit = static_cast<uint64>(text[index] - '0');
        if (value > (0xffffffffffffffffULL - digit) / 10)
        {
            overflow = true;
        }
        if (overflow == false)
        {
            value = value * 10 + digit;
        }
    }
    if (overflow)
    {
        return NumberParseStatus::OutOfRange;
    }
    output = value;
    return NumberParseStatus::Success;
}
NumberParseStatus ParseFloat32(const char* text, usize size, float32& output) noexcept
{
    if (size > 1024)
    {
        return NumberParseStatus::TooLong;
    }
    if (text == nullptr || size == 0)
    {
        return NumberParseStatus::InvalidSyntax;
    }
    usize position = 0;
    const bool negative = text[0] == '-';
    if (negative)
    {
        ++position;
    }
    const usize integerStart = position;
    while (position < size && Digit(text[position]))
    {
        ++position;
    }
    if (position == integerStart || (position - integerStart > 1 && text[integerStart] == '0'))
    {
        return NumberParseStatus::InvalidSyntax;
    }
    usize fractionStart = position;
    if (position < size && text[position] == '.')
    {
        fractionStart = ++position;
        while (position < size && Digit(text[position]))
        {
            ++position;
        }
        if (position == fractionStart)
        {
            return NumberParseStatus::InvalidSyntax;
        }
    }
    const usize fractionEnd = position;
    int32 decimalExponent = 0;
    if (position < size && (text[position] == 'e' || text[position] == 'E'))
    {
        ++position;
        bool exponentNegative = false;
        if (position < size && (text[position] == '+' || text[position] == '-'))
        {
            exponentNegative = text[position++] == '-';
        }
        const usize start = position;
        while (position < size && Digit(text[position]))
        {
            if (decimalExponent < 100000)
            {
                decimalExponent = decimalExponent * 10 + (text[position] - '0');
            }
            ++position;
        }
        if (position == start)
        {
            return NumberParseStatus::InvalidSyntax;
        }
        if (exponentNegative)
        {
            decimalExponent = -decimalExponent;
        }
    }
    if (position != size)
    {
        return NumberParseStatus::InvalidSyntax;
    }
    const int32 fastExponent = decimalExponent - static_cast<int32>(fractionEnd - fractionStart);
    uint32 fastBits = 0;
    if (TryParseFast(text,
                     { .IntegerStart = integerStart, .FractionEnd = fractionEnd, .Exponent = fastExponent },
                     fastBits) == true)
    {
        WriteBinary32(negative == true ? fastBits | SIGN_BIT : fastBits, output);
        return NumberParseStatus::Success;
    }
    Integer numerator;
    int32 significantDigits = 0;
    for (usize index = integerStart; index < fractionEnd; ++index)
    {
        if (Digit(text[index]) == false)
        {
            continue;
        }
        const uint32 digit = static_cast<uint32>(text[index] - '0');
        numerator.Multiply(10);
        numerator.Add(digit);
        if (digit != 0 || significantDigits != 0)
        {
            ++significantDigits;
        }
    }
    decimalExponent -= static_cast<int32>(fractionEnd - fractionStart);
    uint32 bits = 0;
    if (significantDigits != 0)
    {
        const int32 magnitude = significantDigits + decimalExponent - 1;
        if (magnitude < -46 || magnitude > 38)
        {
            return NumberParseStatus::OutOfRange;
        }
        Integer denominator;
        denominator.Add(1);
        while (decimalExponent > 0)
        {
            numerator.Multiply(10);
            --decimalExponent;
        }
        while (decimalExponent < 0)
        {
            denominator.Multiply(10);
            ++decimalExponent;
        }
        uint32 low = 0;
        uint32 high = 0x7f800000U;
        while (high - low > 1)
        {
            const uint32 middle = low + (high - low) / 2;
            if (CompareBinary({numerator, denominator}, {Significand(middle), Exponent(middle)}) >= 0)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }
        const int32 lowerExponent = Exponent(low);
        const uint32 midpoint =
            Significand(low) + (Significand(high) << static_cast<uint32>(Exponent(high) - lowerExponent));
        const int32 comparison = CompareBinary({numerator, denominator}, {midpoint, lowerExponent - 1});
        bits = comparison > 0 || (comparison == 0 && (low & 1U) != 0) ? high : low;
        if (bits == 0 || bits == 0x7f800000U)
        {
            return NumberParseStatus::OutOfRange;
        }
    }
    if (negative)
    {
        bits |= 0x80000000U;
    }
    WriteBinary32(bits, output);
    return NumberParseStatus::Success;
}
} // namespace ludus::foundation
