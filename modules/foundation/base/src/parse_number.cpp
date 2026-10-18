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
    // Byte copy preserves the exact representation without aliasing or unions.
    const auto* source = reinterpret_cast<const uint8*>(&bits);
    auto* destination = reinterpret_cast<uint8*>(&output);
    for (usize index = 0; index < sizeof(output); ++index)
    {
        destination[index] = source[index];
    }
    return NumberParseStatus::Success;
}
} // namespace ludus::foundation
