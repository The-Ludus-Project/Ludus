#include "internal/pack_codec.hpp"

#include <ludus/foundation/base/checked_integer.hpp>

#include <cstring>
#include <span>

// Thanks to L. Peter Deutsch, RFC 1952, sec. 2.3.1 (CRC32),
// https://www.rfc-editor.org/rfc/rfc1952 : the reflected IEEE CRC32 polynomial
// and complemented initial/final state define our corruption checks. Original
// bitwise implementation, not the appendix's table code; no GZIP framing used.
// Thanks to Yann Collet, "LZ4 Block Format Description", revised 2022-07-31,
// sections Compressed block format, End of block conditions and Safe decoding:
// https://github.com/lz4/lz4/blob/v1.10.0/doc/lz4_Block_format.md . This original
// bounded decoder checks lengths, offsets and overlap and requires the stated
// final-literal/last-match conditions; no upstream implementation code copied.

namespace ludus::foundation::filesystem::internal
{
uint32 Crc32(std::span<const uint8> bytes) noexcept
{
    uint32 crc = ~uint32{0};
    for (const auto byte : bytes)
    {
        crc ^= byte;
        for (uint32 bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xedb88320U);
        }
    }
    return ~crc;
}
namespace
{
/// LZ4 block end conditions: the last sequence carries at least five literals,
/// and the last match starts at least twelve bytes before the end of the block.
constexpr usize LZ4_MIN_FINAL_LITERALS = 5;
constexpr usize LZ4_LAST_MATCH_END_DISTANCE = 12;
/// Shortest LZ4 match; a token's match length field counts bytes beyond it.
constexpr usize LZ4_MIN_MATCH_BYTES = 4;
bool Length(usize& cursor, std::span<const uint8> source, usize& length) noexcept
{
    if (length != 15)
    {
        return true;
    }
    uint8 byte = 255;
    while (byte == 255)
    {
        if (cursor == source.size())
        {
            return false;
        }
        byte = source[cursor++];
        if (TryAdd(length, static_cast<usize>(byte), length) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace
bool DecodeLz4(std::span<const uint8> stored, std::span<uint8> decoded) noexcept
{
    usize input = 0, output = 0, lastMatch = 0;
    bool matched = false;
    while (input < stored.size())
    {
        const uint8 token = stored[input++];
        usize literals = token >> 4;
        if (Length(input, stored, literals) == false || literals > stored.size() - input ||
            literals > decoded.size() - output)
        {
            return false;
        }
        if (literals != 0)
        {
            std::memcpy(decoded.data() + output, stored.data() + input, literals);
        }
        input += literals;
        output += literals;
        if (input == stored.size())
        {
            // The final sequence has no offset/match. Require the LZ4 final
            // literals and last-match-before-end conditions.
            return output == decoded.size() &&
                   (matched == false ||
                    (literals >= LZ4_MIN_FINAL_LITERALS && decoded.size() - lastMatch >= LZ4_LAST_MATCH_END_DISTANCE));
        }
        if (stored.size() - input < 2)
        {
            return false;
        }
        const usize offset = static_cast<usize>(stored[input]) | (static_cast<usize>(stored[input + 1]) << 8);
        input += 2;
        if (offset == 0 || offset > output)
        {
            return false;
        }
        usize match = token & 15U;
        if (Length(input, stored, match) == false || TryAdd(match, LZ4_MIN_MATCH_BYTES, match) == false ||
            match > decoded.size() - output)
        {
            return false;
        }
        lastMatch = output;
        matched = true;
        // Byte progression deliberately handles overlapping matches (offset=1
        // repeats a byte); memcpy/memmove cannot produce not-yet-decoded bytes.
        for (usize i = 0; i < match; ++i)
        {
            decoded[output] = decoded[output - offset];
            ++output;
        }
    }
    return false;
}
} // namespace ludus::foundation::filesystem::internal
