#include "internal/code_text.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/core.h>

#include <QChar>
#include <QStringDecoder>
#include <QtEndian>

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
constexpr usize ASCII_WORD_BYTES = sizeof(uint64);
constexpr int32 ASCII_WORD_UTF16_UNITS = static_cast<int32>(ASCII_WORD_BYTES);
constexpr uint64 BYTE_LOW_BITS = 0x0101010101010101ULL;
constexpr uint64 BYTE_HIGH_BITS = 0x8080808080808080ULL;
constexpr uint64 CARRIAGE_RETURN_BYTES = BYTE_LOW_BITS * static_cast<uint64>('\r');

// ASCII bytes other than CR advance both coordinates by one. XOR turns a CR
// into a zero byte, so a word containing CR or a high bit needs the scalar walk.
// Thanks to Alan Mycroft and Stephen M. Bennet, as collected by Sean Eron Anderson:
// https://graphics.stanford.edu/~seander/bithacks.html#ZeroInWord
LUDUS_INLINE bool IsSingleUnitAsciiWord(const char* bytes) noexcept
{
    const uint64 word = qFromLittleEndian<uint64>(bytes);
    const uint64 carriageReturns = word ^ CARRIAGE_RETURN_BYTES;
    const uint64 zeroByteHighBits = (carriageReturns - BYTE_LOW_BITS) & ~carriageReturns & BYTE_HIGH_BITS;
    return ((word & BYTE_HIGH_BITS) | zeroByteHighBits) == 0;
}

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

bool IsPhysicalLineBreak(QChar character) noexcept
{
    return character == u'\n' || character == u'\r' || character == QChar::ParagraphSeparator;
}

isize SkipLineContinuations(QStringView source, isize position) noexcept
{
    while (position + 1 < source.size() && source.at(position) == u'\\' &&
           IsPhysicalLineBreak(source.at(position + 1)) == true)
    {
        position += 2;
        if (position < source.size() && source.at(position - 1) == u'\r' && source.at(position) == u'\n')
        {
            ++position;
        }
    }
    return position;
}

isize SkipQuotedSlangText(QStringView source, isize position, QChar quote) noexcept
{
    while (position < source.size())
    {
        position = SkipLineContinuations(source, position);
        if (position == source.size() || IsPhysicalLineBreak(source.at(position)) == true)
        {
            return position;
        }
        const QChar character = source.at(position++);
        if (character == quote)
        {
            return position;
        }
        if (character == u'\\' && position < source.size())
        {
            position = SkipLineContinuations(source, position);
            position += position < source.size() ? 1 : 0;
        }
    }
    return position;
}

isize SkipSlangComment(QStringView source, isize position, bool isBlock) noexcept
{
    while (position < source.size())
    {
        position = SkipLineContinuations(source, position);
        if (position == source.size())
        {
            break;
        }
        const QChar character = source.at(position);
        if (isBlock == false && IsPhysicalLineBreak(character) == true)
        {
            return position;
        }
        ++position;
        if (isBlock == true && character == u'*')
        {
            const isize next = SkipLineContinuations(source, position);
            if (next < source.size() && source.at(next) == u'/')
            {
                return next + 1;
            }
        }
    }
    return position;
}

// Thanks to the Slang authors, "Literal Expressions", String Literal Expressions:
// https://docs.shader-slang.org/en/latest/external/slang/docs/language-reference/expressions-literal.html
// A raw string's closing delimiter is paired with the opening delimiter. Inner quotes
// and newlines are data, so the regular quoted-string scan cannot classify them.
isize SkipRawSlangString(QStringView source, isize delimiterStart)
{
    for (isize position = delimiterStart; position < source.size(); ++position)
    {
        const QChar character = source.at(position);
        if (character == u'(')
        {
            QString terminator(u')');
            terminator.append(source.sliced(delimiterStart, position - delimiterStart));
            terminator.append(u'"');
            const isize close = source.indexOf(terminator, position + 1);
            return close < 0 ? source.size() : close + terminator.size();
        }
        if (character == u'"' || character == u')' || IsPhysicalLineBreak(character) == true)
        {
            break;
        }
    }
    return SkipQuotedSlangText(source, delimiterStart, u'"');
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

QString RepairSlangLineSeparators(QStringView source, QStringView newline)
{
    const isize lastSeparator = source.lastIndexOf(QChar::LineSeparator);
    QString repaired;
    isize retainedStart = 0;
    for (isize position = 0; position <= lastSeparator;)
    {
        position = SkipLineContinuations(source, position);
        if (position > lastSeparator)
        {
            break;
        }
        const QChar character = source.at(position);
        const isize nextPosition = SkipLineContinuations(source, position + 1);
        const QChar next = nextPosition < source.size() ? source.at(nextPosition) : QChar();
        if (character == u'/' && (next == u'/' || next == u'*'))
        {
            position = SkipSlangComment(source, nextPosition + 1, next == u'*');
        }
        else if (character == u'R' && next == u'"')
        {
            position = SkipRawSlangString(source, nextPosition + 1);
        }
        else if (character == u'"' || character == u'\'')
        {
            position = SkipQuotedSlangText(source, position + 1, character);
        }
        else
        {
            if (character == QChar::LineSeparator)
            {
                repaired.append(source.sliced(retainedStart, position - retainedStart));
                repaired.append(newline);
                retainedStart = position + 1;
            }
            ++position;
        }
    }
    if (retainedStart != 0)
    {
        repaired.append(source.sliced(retainedStart));
    }
    return repaired;
}

QByteArray RepairSlangLineSeparators(const QByteArray& source)
{
    constexpr char LINE_SEPARATOR_UTF8[] = "\xe2\x80\xa8";
    if (source.contains(LINE_SEPARATOR_UTF8) == false)
    {
        return {};
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    const QString text = decoder.decode(source);
    if (decoder.hasError() == true)
    {
        return {};
    }
    // A replacement LF immediately after a lone CR would merge two Qt lines.
    const bool hasCrLf = source.contains("\r\n") == true || text.contains(u"\r\u2028") == true;
    const QStringView newline = hasCrLf == true ? QStringView(u"\r\n") : QStringView(u"\n");
    return RepairSlangLineSeparators(text, newline).toUtf8();
}

// UTF-8 is validated on bind/admission. This walk counts Qt UTF-16 units and
// collapses CRLF, rather than treating byte, scalar and cursor offsets as equal.
// Thanks to The Unicode Consortium, The Unicode Standard, section 3.9,
// tables 3-5/3-6: scalar lengths in UTF-8 and UTF-16 (no grapheme-width claim).
// https://www.unicode.org/versions/Unicode18.0.0/core-spec/chapter-3/
bool GetCodeByteOffset(const QByteArray& bytes, int32 position, usize& outOffset) noexcept
{
    CodePositionOffset start;
    return AdvanceCodeByteOffset(bytes, position, start, outOffset);
}

bool AdvanceCodeByteOffset(const QByteArray& bytes,
                           int32 position,
                           CodePositionOffset& inoutFrom,
                           usize& outOffset) noexcept
{
    const usize length = static_cast<usize>(bytes.size());
    if (position < inoutFrom.Position || inoutFrom.OffsetBytes > length)
    {
        return false;
    }
    usize offset = inoutFrom.OffsetBytes;
    int32 units = inoutFrom.Position;
    while (offset < length && units < position)
    {
        if (position - units >= ASCII_WORD_UTF16_UNITS && length - offset >= ASCII_WORD_BYTES &&
            IsSingleUnitAsciiWord(bytes.constData() + offset) == true)
        {
            offset += ASCII_WORD_BYTES;
            units += ASCII_WORD_UTF16_UNITS;
            continue;
        }
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
    inoutFrom = { .Position = position, .OffsetBytes = offset };
    outOffset = offset;
    return true;
}

bool GetCodeCursorPosition(const QByteArray& bytes, usize offset, int32& outPosition) noexcept
{
    CodePositionOffset start;
    return AdvanceCodeCursorPosition(bytes, offset, start, outPosition);
}

bool AdvanceCodeCursorPosition(const QByteArray& bytes,
                               usize offset,
                               CodePositionOffset& inoutFrom,
                               int32& outPosition) noexcept
{
    if (offset > static_cast<usize>(bytes.size()) || offset < inoutFrom.OffsetBytes)
    {
        return false;
    }
    usize byte = inoutFrom.OffsetBytes;
    int32 units = inoutFrom.Position;
    while (byte < offset)
    {
        if (offset - byte >= ASCII_WORD_BYTES && IsSingleUnitAsciiWord(bytes.constData() + byte) == true)
        {
            byte += ASCII_WORD_BYTES;
            units += ASCII_WORD_UTF16_UNITS;
            continue;
        }
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
    inoutFrom = { .Position = units, .OffsetBytes = offset };
    outPosition = units;
    return true;
}

bool IsCodeScalarBoundary(const QByteArray& bytes, usize offset) noexcept
{
    return IsCodeScalarBoundary(BorrowedView<const char>{bytes.constData(), static_cast<usize>(bytes.size())}, offset);
}

bool IsCodeScalarBoundary(BorrowedView<const char> bytes, usize offset) noexcept
{
    if (bytes.IsValid() == false || offset > bytes.GetSize())
    {
        return false;
    }
    return offset == bytes.GetSize() || IsContinuation(bytes[offset]) == false;
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
