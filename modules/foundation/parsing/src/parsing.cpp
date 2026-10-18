#include <ludus/foundation/parsing/parsing.hpp>

#include <ludus/foundation/base/byte_order.hpp>

#include <span>
#include <string_view>

namespace ludus::foundation::parsing
{
bool ByteCursor::Take(usize count, std::span<const uint8>& output) noexcept
{
    if (count > Remaining())
    {
        return false;
    }
    output = mInput.subspan(mPosition, count);
    mPosition += count;
    return true;
}

bool ByteCursor::Skip(usize count) noexcept
{
    if (count > Remaining())
    {
        return false;
    }
    mPosition += count;
    return true;
}

bool ByteCursor::ReadUint8(uint8& output) noexcept
{
    if (Remaining() == 0)
    {
        return false;
    }
    output = mInput[mPosition++];
    return true;
}

bool ByteCursor::ReadUint32LittleEndian(uint32& output) noexcept
{
    if (TryReadLittleEndian(mInput.subspan(mPosition), output) == false)
    {
        return false;
    }
    mPosition += sizeof(output);
    return true;
}

bool ByteCursor::ReadUint64LittleEndian(uint64& output) noexcept
{
    if (TryReadLittleEndian(mInput.subspan(mPosition), output) == false)
    {
        return false;
    }
    mPosition += sizeof(output);
    return true;
}

// Thanks to the Unicode Consortium, "The Unicode Standard 17.0", section 3.9,
// tables 3-6/3-7, for scalar decoding and well-formed UTF-8 byte ranges:
// https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/ .
// Reject overlong encodings, surrogates and values above U+10FFFF; no
// normalization, grapheme segmentation or replacement-character recovery.
ParseStatus ValidateUtf8(std::string_view input, ParseError& error) noexcept
{
    error = {};
    for (usize offset = 0; offset < input.size();)
    {
        const auto first = static_cast<uint8>(input[offset]);
        if (first < 0x80)
        {
            ++offset;
            continue;
        }
        usize length = 0;
        uint8 secondMin = 0x80;
        uint8 secondMax = 0xbf;
        if (first >= 0xc2 && first <= 0xdf)
        {
            length = 2;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            length = 3;
            if (first == 0xe0)
            {
                secondMin = 0xa0;
            }
            else if (first == 0xed)
            {
                secondMax = 0x9f;
            }
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            length = 4;
            if (first == 0xf0)
            {
                secondMin = 0x90;
            }
            else if (first == 0xf4)
            {
                secondMax = 0x8f;
            }
        }
        bool valid = length != 0 && length <= input.size() - offset;
        for (usize i = 1; valid && i < length; ++i)
        {
            const auto byte = static_cast<uint8>(input[offset + i]);
            valid = i == 1 ? byte >= secondMin && byte <= secondMax : byte >= 0x80 && byte <= 0xbf;
        }
        if (valid == false)
        {
            error.Status = ParseStatus::InvalidEncoding;
            error.Offset = offset;
            return error.Status;
        }
        offset += length;
    }
    return ParseStatus::Ok;
}

bool LocateByte(std::string_view input, usize offset, SourcePosition& output) noexcept
{
    if (offset > input.size())
    {
        return false;
    }
    SourcePosition position;
    for (usize i = 0; i < offset; ++i)
    {
        if (input[i] == '\r')
        {
            ++position.Line;
            position.ByteColumn = 1;
        }
        else if (input[i] == '\n')
        {
            if (i == 0 || input[i - 1] != '\r')
            {
                ++position.Line;
            }
            position.ByteColumn = 1;
        }
        else
        {
            ++position.ByteColumn;
        }
    }
    output = position;
    return true;
}
} // namespace ludus::foundation::parsing
