#include "decimal_number.h"

#include <ludus/foundation/base/parse_number.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
[[nodiscard]] bool IsDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}
} // namespace

bool ParseNumber(std::string_view text, float64& outValue) noexcept
{
    // Normalise to the JSON number grammar accepted by ParseFloat32:
    // drop '+', drop redundant leading zeros, add '0' around a bare '.'.
    char normalised[MAX_DECIMAL_TEXT_BYTES + 8];
    usize length = 0;
    usize at = 0;
    bool isNegative = false;
    if (at < text.size() && (text[at] == '+' || text[at] == '-'))
    {
        isNegative = text[at] == '-';
        ++at;
    }
    if (isNegative == true)
    {
        normalised[length++] = '-';
    }
    const usize integerStart = at;
    while (at < text.size() && IsDigit(text[at]) == true)
    {
        ++at;
    }
    usize integerFirst = integerStart;
    while (at - integerFirst > 1 && text[integerFirst] == '0')
    {
        ++integerFirst;
    }
    if (integerFirst == at)
    {
        normalised[length++] = '0';
    }
    for (usize i = integerFirst; i < at; ++i)
    {
        normalised[length++] = text[i];
    }
    bool hasDigits = at > integerStart;
    if (at < text.size() && text[at] == '.')
    {
        ++at;
        const usize fractionStart = at;
        while (at < text.size() && IsDigit(text[at]) == true)
        {
            ++at;
        }
        hasDigits = hasDigits == true || at > fractionStart;
        if (at > fractionStart)
        {
            normalised[length++] = '.';
            for (usize i = fractionStart; i < at; ++i)
            {
                normalised[length++] = text[i];
            }
        }
    }
    if (hasDigits == false || text.size() > MAX_DECIMAL_TEXT_BYTES)
    {
        return false;
    }
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E'))
    {
        normalised[length++] = 'e';
        ++at;
        if (at < text.size() && (text[at] == '+' || text[at] == '-'))
        {
            normalised[length++] = text[at];
            ++at;
        }
        const usize exponentStart = at;
        while (at < text.size() && IsDigit(text[at]) == true)
        {
            normalised[length++] = text[at];
            ++at;
        }
        if (at == exponentStart)
        {
            return false;
        }
    }
    if (at != text.size())
    {
        return false;
    }
    float32 parsed = 0.0F;
    const foundation::NumberParseStatus status = foundation::ParseFloat32(normalised, length, parsed);
    if (status == foundation::NumberParseStatus::Success)
    {
        outValue = static_cast<float64>(parsed);
        return true;
    }
    if (status != foundation::NumberParseStatus::OutOfRange)
    {
        return false;
    }
    // Distinguish underflow (exponent negative) from overflow.
    for (usize i = 0; i < length; ++i)
    {
        if (normalised[i] == 'e' && i + 1 < length && normalised[i + 1] == '-')
        {
            outValue = isNegative == true ? -0.0 : 0.0;
            return true;
        }
    }
    return false;
}
} // namespace ludus::content::scene_cook::importers
