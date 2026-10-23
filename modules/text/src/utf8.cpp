#include "internal/utf8.h"

namespace ludus::text::internal
{
namespace
{
/// Offset of the last byte of a four-byte sequence from its lead byte.
constexpr usize FOUR_BYTE_SEQUENCE_LAST_OFFSET = 3;
[[nodiscard]] bool IsContinuation(unsigned char byte) noexcept
{
    return (byte & 0xC0U) == 0x80U;
}
} // namespace

// Thanks to the Unicode Consortium, "The Unicode Standard 17.0", section 3.9,
// tables 3-6/3-7, for scalar decoding and well-formed UTF-8 byte ranges:
// https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/ .
// Reject overlong encodings, surrogates and values above U+10FFFF; no
// normalization, grapheme segmentation or replacement-character recovery.
usize DecodeUtf8(std::string_view text, usize offset, uint32* outCodePoint) noexcept
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const usize size = text.size();
    const unsigned char lead = bytes[offset];

    // 1-byte ASCII (0x00..0x7F).
    if (lead < 0x80U)
    {
        *outCodePoint = lead;
        return 1;
    }

    // 2-byte sequence (0xC2..0xDF); 0xC0/0xC1 would be overlong.
    if (lead >= 0xC2U && lead <= 0xDFU)
    {
        if (offset + 1 >= size || IsContinuation(bytes[offset + 1]) == false)
        {
            return 0;
        }
        const uint32 cp = (static_cast<uint32>(lead & 0x1FU) << 6) | static_cast<uint32>(bytes[offset + 1] & 0x3FU);
        if (cp < 0x80U) // overlong
        {
            return 0;
        }
        *outCodePoint = cp;
        return 2;
    }

    // 3-byte sequence (0xE0..0xEF). Reject overlong and surrogates.
    if (lead >= 0xE0U && lead <= 0xEFU)
    {
        if (offset + 2 >= size || IsContinuation(bytes[offset + 1]) == false ||
            IsContinuation(bytes[offset + 2]) == false)
        {
            return 0;
        }
        const uint32 cp = (static_cast<uint32>(lead & 0x0FU) << 12) |
                          (static_cast<uint32>(bytes[offset + 1] & 0x3FU) << 6) |
                          static_cast<uint32>(bytes[offset + 2] & 0x3FU);
        if (cp < 0x800U) // overlong
        {
            return 0;
        }
        if (cp >= 0xD800U && cp <= 0xDFFFU) // surrogate half
        {
            return 0;
        }
        *outCodePoint = cp;
        return 3;
    }

    // 4-byte sequence (0xF0..0xF4). Reject overlong and > U+10FFFF.
    if (lead >= 0xF0U && lead <= 0xF4U)
    {
        if (offset + FOUR_BYTE_SEQUENCE_LAST_OFFSET >= size || IsContinuation(bytes[offset + 1]) == false ||
            IsContinuation(bytes[offset + 2]) == false ||
            IsContinuation(bytes[offset + FOUR_BYTE_SEQUENCE_LAST_OFFSET]) == false)
        {
            return 0;
        }
        const uint32 cp =
            (static_cast<uint32>(lead & 0x07U) << 18) | (static_cast<uint32>(bytes[offset + 1] & 0x3FU) << 12) |
            (static_cast<uint32>(bytes[offset + 2] & 0x3FU) << 6) | static_cast<uint32>(bytes[offset + 3] & 0x3FU);
        if (cp < 0x10000U) // overlong
        {
            return 0;
        }
        if (cp > 0x10FFFFU) // out of Unicode range
        {
            return 0;
        }
        *outCodePoint = cp;
        return 4;
    }

    // 0x80..0xBF (stray continuation), 0xC0/0xC1, and 0xF5..0xFF are all invalid
    // leading bytes.
    return 0;
}

Utf8Result ValidateUtf8(std::string_view text) noexcept
{
    Utf8Result result;
    usize offset = 0;
    const usize size = text.size();
    while (offset < size)
    {
        uint32 codePoint = 0;
        const usize consumed = DecodeUtf8(text, offset, &codePoint);
        if (consumed == 0)
        {
            result.Valid = false;
            result.FirstInvalidByte = offset;
            return result;
        }
        offset += consumed;
        ++result.CodePointCount;
    }
    result.Valid = true;
    result.FirstInvalidByte = 0;
    return result;
}
} // namespace ludus::text::internal
