#pragma once

#include <ludus/foundation/base/types.h>

#include <string_view>

// Thanks to the Unicode Consortium, The Unicode Standard 17.0, section 3.9 "Unicode Encoding
// Forms": table 3-7 (well-formed UTF-8 byte sequences) defines the accepted ranges below, and
// "U+FFFD Substitution of Maximal Subparts" with table 3-8 defines the subpart length reported for
// an ill-formed sequence (https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/).
namespace ludus::foundation::detail
{
/// Bytes below this value are ASCII, one-byte sequences.
inline constexpr uint8 UTF8_ASCII_LIMIT = 0x80;
/// Inclusive range of an unrestricted continuation byte, 10xxxxxx.
inline constexpr uint8 UTF8_CONTINUATION_MINIMUM = 0x80;
inline constexpr uint8 UTF8_CONTINUATION_MAXIMUM = 0xBF;
/// Lead-byte ranges; C0, C1 and F5..FF never occur in well-formed UTF-8.
inline constexpr uint8 UTF8_TWO_BYTE_LEAD_MINIMUM = 0xC2;
inline constexpr uint8 UTF8_TWO_BYTE_LEAD_MAXIMUM = 0xDF;
inline constexpr uint8 UTF8_THREE_BYTE_LEAD_MINIMUM = 0xE0;
inline constexpr uint8 UTF8_THREE_BYTE_LEAD_MAXIMUM = 0xEF;
inline constexpr uint8 UTF8_FOUR_BYTE_LEAD_MINIMUM = 0xF0;
inline constexpr uint8 UTF8_FOUR_BYTE_LEAD_MAXIMUM = 0xF4;
/// E0 needs a second byte of at least A0, rejecting overlong three-byte forms.
inline constexpr uint8 UTF8_THREE_BYTE_OVERLONG_LEAD = 0xE0;
inline constexpr uint8 UTF8_THREE_BYTE_OVERLONG_SECOND_MINIMUM = 0xA0;
/// ED needs a second byte of at most 9F, rejecting the surrogates U+D800..U+DFFF.
inline constexpr uint8 UTF8_SURROGATE_LEAD = 0xED;
inline constexpr uint8 UTF8_SURROGATE_SECOND_MAXIMUM = 0x9F;
/// F0 needs a second byte of at least 90, rejecting overlong four-byte forms.
inline constexpr uint8 UTF8_FOUR_BYTE_OVERLONG_LEAD = 0xF0;
inline constexpr uint8 UTF8_FOUR_BYTE_OVERLONG_SECOND_MINIMUM = 0x90;
/// F4 needs a second byte of at most 8F, rejecting values above U+10FFFF.
inline constexpr uint8 UTF8_MAXIMUM_SCALAR_LEAD = 0xF4;
inline constexpr uint8 UTF8_MAXIMUM_SCALAR_SECOND_MAXIMUM = 0x8F;
/// Sequence lengths, in bytes.
inline constexpr usize UTF8_THREE_BYTE_LENGTH = 3;
inline constexpr usize UTF8_FOUR_BYTE_LENGTH = 4;
/// Payload bits carried by one continuation byte, and their mask.
inline constexpr uint32 UTF8_CONTINUATION_PAYLOAD_BITS = 6;
inline constexpr uint32 UTF8_CONTINUATION_PAYLOAD_MASK = 0x3F;
/// Payload masks of two-, three- and four-byte lead bytes.
inline constexpr uint32 UTF8_TWO_BYTE_LEAD_PAYLOAD_MASK = 0x1F;
inline constexpr uint32 UTF8_THREE_BYTE_LEAD_PAYLOAD_MASK = 0x0F;
inline constexpr uint32 UTF8_FOUR_BYTE_LEAD_PAYLOAD_MASK = 0x07;
/// Lead-byte tags of two-, three- and four-byte sequences.
inline constexpr uint32 UTF8_TWO_BYTE_LEAD_TAG = 0xC0;
inline constexpr uint32 UTF8_THREE_BYTE_LEAD_TAG = 0xE0;
inline constexpr uint32 UTF8_FOUR_BYTE_LEAD_TAG = 0xF0;
/// Smallest scalars that need two, three and four bytes.
inline constexpr uint32 UTF8_TWO_BYTE_SCALAR_MINIMUM = 0x80;
inline constexpr uint32 UTF8_THREE_BYTE_SCALAR_MINIMUM = 0x800;
inline constexpr uint32 UTF8_FOUR_BYTE_SCALAR_MINIMUM = 0x10000;

/// One sequence found at a byte offset by ScanUtf8Sequence.
struct Utf8Sequence final
{
    /// Bytes of the well-formed sequence, or of the maximal ill-formed subpart; at least one.
    usize Length = 1;
    /// True when the bytes form one well-formed scalar.
    bool IsWellFormed = false;
};

/// True for a continuation byte, which never starts a sequence.
[[nodiscard]] constexpr bool IsUtf8Continuation(char byte) noexcept
{
    const uint8 value = static_cast<uint8>(byte);
    return value >= UTF8_CONTINUATION_MINIMUM && value <= UTF8_CONTINUATION_MAXIMUM;
}

/// Classifies the sequence starting at `bytes[offset]`; `offset` must be less than the size.
[[nodiscard]] constexpr Utf8Sequence ScanUtf8Sequence(std::string_view bytes, usize offset) noexcept
{
    const uint8 lead = static_cast<uint8>(bytes[offset]);
    if (lead < UTF8_ASCII_LIMIT)
    {
        return { .Length = 1, .IsWellFormed = true };
    }
    usize length = 0;
    uint8 minimum = UTF8_CONTINUATION_MINIMUM;
    uint8 maximum = UTF8_CONTINUATION_MAXIMUM;
    if (lead >= UTF8_TWO_BYTE_LEAD_MINIMUM && lead <= UTF8_TWO_BYTE_LEAD_MAXIMUM)
    {
        length = 2;
    }
    else if (lead >= UTF8_THREE_BYTE_LEAD_MINIMUM && lead <= UTF8_THREE_BYTE_LEAD_MAXIMUM)
    {
        length = UTF8_THREE_BYTE_LENGTH;
        minimum = lead == UTF8_THREE_BYTE_OVERLONG_LEAD ? UTF8_THREE_BYTE_OVERLONG_SECOND_MINIMUM : minimum;
        maximum = lead == UTF8_SURROGATE_LEAD ? UTF8_SURROGATE_SECOND_MAXIMUM : maximum;
    }
    else if (lead >= UTF8_FOUR_BYTE_LEAD_MINIMUM && lead <= UTF8_FOUR_BYTE_LEAD_MAXIMUM)
    {
        length = UTF8_FOUR_BYTE_LENGTH;
        minimum = lead == UTF8_FOUR_BYTE_OVERLONG_LEAD ? UTF8_FOUR_BYTE_OVERLONG_SECOND_MINIMUM : minimum;
        maximum = lead == UTF8_MAXIMUM_SCALAR_LEAD ? UTF8_MAXIMUM_SCALAR_SECOND_MAXIMUM : maximum;
    }
    const usize available = bytes.size() - offset;
    if (length == 0 || available < 2)
    {
        return { .Length = 1, .IsWellFormed = false };
    }
    const uint8 second = static_cast<uint8>(bytes[offset + 1]);
    if (second < minimum || second > maximum)
    {
        return { .Length = 1, .IsWellFormed = false };
    }
    for (usize i = 2; i < length; ++i)
    {
        if (i >= available || IsUtf8Continuation(bytes[offset + i]) == false)
        {
            // The lead and its valid continuations so far form one maximal subpart.
            return { .Length = i, .IsWellFormed = false };
        }
    }
    return { .Length = length, .IsWellFormed = true };
}

/// Decodes the well-formed sequence of `length` bytes at `bytes[offset]`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (offset, length) as returned by ScanUtf8Sequence.
[[nodiscard]] constexpr uint32 DecodeUtf8Sequence(std::string_view bytes, usize offset, usize length) noexcept
{
    const uint32 lead = static_cast<uint8>(bytes[offset]);
    uint32 scalar = lead;
    if (length == 2)
    {
        scalar = lead & UTF8_TWO_BYTE_LEAD_PAYLOAD_MASK;
    }
    else if (length == UTF8_THREE_BYTE_LENGTH)
    {
        scalar = lead & UTF8_THREE_BYTE_LEAD_PAYLOAD_MASK;
    }
    else if (length == UTF8_FOUR_BYTE_LENGTH)
    {
        scalar = lead & UTF8_FOUR_BYTE_LEAD_PAYLOAD_MASK;
    }
    for (usize i = 1; i < length; ++i)
    {
        scalar = (scalar << UTF8_CONTINUATION_PAYLOAD_BITS) |
                 (static_cast<uint8>(bytes[offset + i]) & UTF8_CONTINUATION_PAYLOAD_MASK);
    }
    return scalar;
}

/// Bytes needed to encode one Unicode scalar.
[[nodiscard]] constexpr usize GetUtf8EncodedLength(uint32 scalar) noexcept
{
    if (scalar < UTF8_TWO_BYTE_SCALAR_MINIMUM)
    {
        return 1;
    }
    if (scalar < UTF8_THREE_BYTE_SCALAR_MINIMUM)
    {
        return 2;
    }
    return scalar < UTF8_FOUR_BYTE_SCALAR_MINIMUM ? UTF8_THREE_BYTE_LENGTH : UTF8_FOUR_BYTE_LENGTH;
}

/// Encodes one Unicode scalar into GetUtf8EncodedLength(scalar) bytes at `outBytes`.
constexpr void EncodeUtf8Scalar(uint32 scalar, char* outBytes) noexcept
{
    const usize length = GetUtf8EncodedLength(scalar);
    if (length == 1)
    {
        outBytes[0] = static_cast<char>(scalar);
        return;
    }
    uint32 tag = UTF8_TWO_BYTE_LEAD_TAG;
    if (length == UTF8_THREE_BYTE_LENGTH)
    {
        tag = UTF8_THREE_BYTE_LEAD_TAG;
    }
    else if (length == UTF8_FOUR_BYTE_LENGTH)
    {
        tag = UTF8_FOUR_BYTE_LEAD_TAG;
    }
    for (usize i = length - 1; i > 0; --i)
    {
        outBytes[i] = static_cast<char>(UTF8_CONTINUATION_MINIMUM | (scalar & UTF8_CONTINUATION_PAYLOAD_MASK));
        scalar >>= UTF8_CONTINUATION_PAYLOAD_BITS;
    }
    outBytes[0] = static_cast<char>(tag | scalar);
}
} // namespace ludus::foundation::detail
