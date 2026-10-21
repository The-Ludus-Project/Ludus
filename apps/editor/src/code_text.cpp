#include "internal/code_text.h"

#include <ludus/foundation/base/core.h>

#include <QStringDecoder>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr uint8 CONTINUATION_MASK = 0xC0;
constexpr uint8 CONTINUATION_PREFIX = 0x80;
constexpr uint8 THREE_BYTE_PREFIX = 0xE0;
constexpr uint8 FOUR_BYTE_PREFIX = 0xF0;
constexpr usize THREE_BYTE_WIDTH = 3;
constexpr usize FOUR_BYTE_WIDTH = 4;

usize GetScalarWidth(uint8 lead) noexcept
{
    if (lead < CONTINUATION_PREFIX)
    {
        return 1;
    }
    if (lead < THREE_BYTE_PREFIX)
    {
        return 2;
    }
    return lead < FOUR_BYTE_PREFIX ? THREE_BYTE_WIDTH : FOUR_BYTE_WIDTH;
}

bool IsContinuation(char character) noexcept
{
    return (static_cast<uint8>(character) & CONTINUATION_MASK) == CONTINUATION_PREFIX;
}
} // namespace

bool DecodeCodeText(const QByteArray& bytes, QString& outText)
{
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    QString text = decoder.decode(bytes);
    if (decoder.hasError() == true)
    {
        return false;
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    outText = Move(text);
    return true;
}

// UTF-8 is validated on bind/admission. This walk counts Qt UTF-16 units and
// collapses CRLF, rather than treating byte, scalar and cursor offsets as equal.
// Thanks to The Unicode Consortium, The Unicode Standard, section 3.9,
// tables 3-5/3-6: scalar lengths in UTF-8 and UTF-16 (no grapheme-width claim).
// https://www.unicode.org/versions/Unicode18.0.0/core-spec/chapter-3/
bool GetCodeByteOffset(const QByteArray& bytes, int32 position, usize& outOffset) noexcept
{
    if (position < 0)
    {
        return false;
    }
    usize offset = 0;
    int32 units = 0;
    const usize length = static_cast<usize>(bytes.size());
    while (offset < length && units < position)
    {
        const usize width = GetScalarWidth(static_cast<uint8>(bytes.at(static_cast<isize>(offset))));
        if (width > length - offset)
        {
            return false;
        }
        const int32 scalarUnits = width == FOUR_BYTE_WIDTH ? 2 : 1;
        if (position - units < scalarUnits)
        {
            return false;
        }
        if (bytes.at(static_cast<isize>(offset)) == '\r' && offset + 1 < length &&
            bytes.at(static_cast<isize>(offset + 1)) == '\n')
        {
            ++offset;
        }
        offset += width;
        units += scalarUnits;
    }
    if (units != position)
    {
        return false;
    }
    outOffset = offset;
    return true;
}

bool GetCodeCursorPosition(const QByteArray& bytes, usize offset, int32& outPosition) noexcept
{
    if (offset > static_cast<usize>(bytes.size()))
    {
        return false;
    }
    usize byte = 0;
    int32 units = 0;
    while (byte < offset)
    {
        const usize width = GetScalarWidth(static_cast<uint8>(bytes.at(static_cast<isize>(byte))));
        usize consumed = width;
        if (bytes.at(static_cast<isize>(byte)) == '\r' && byte + 1 < static_cast<usize>(bytes.size()) &&
            bytes.at(static_cast<isize>(byte + 1)) == '\n')
        {
            ++consumed;
        }
        if (consumed > offset - byte)
        {
            return false;
        }
        byte += consumed;
        units += width == FOUR_BYTE_WIDTH ? 2 : 1;
    }
    outPosition = units;
    return true;
}

bool IsCodeScalarBoundary(const QByteArray& bytes, usize offset) noexcept
{
    const usize size = static_cast<usize>(bytes.size());
    return offset <= size && (offset == size || IsContinuation(bytes.at(static_cast<isize>(offset))) == false);
}

CodeTextEdit MakeCodeTextEdit(const QByteArray& before, const QByteArray& after) noexcept
{
    const usize beforeSize = static_cast<usize>(before.size());
    const usize afterSize = static_cast<usize>(after.size());
    usize prefix = 0;
    while (prefix < beforeSize && prefix < afterSize &&
           before.at(static_cast<isize>(prefix)) == after.at(static_cast<isize>(prefix)))
    {
        ++prefix;
    }
    while (prefix > 0 && prefix < afterSize && IsContinuation(after.at(static_cast<isize>(prefix))) == true)
    {
        --prefix;
    }
    usize suffix = 0;
    while (suffix < beforeSize - prefix && suffix < afterSize - prefix &&
           before.at(static_cast<isize>(beforeSize - suffix - 1)) ==
               after.at(static_cast<isize>(afterSize - suffix - 1)))
    {
        ++suffix;
    }
    while (suffix > 0 && IsContinuation(after.at(static_cast<isize>(afterSize - suffix))) == true)
    {
        --suffix;
    }
    return {prefix, beforeSize - prefix - suffix, {after.constData() + prefix, afterSize - prefix - suffix}};
}
} // namespace ludus::editor
