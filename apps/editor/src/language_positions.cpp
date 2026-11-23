#include "internal/language_positions.h"

#include <QList>

// Thanks to Microsoft, Language Server Protocol 3.17, "Position" and
// "PositionEncodingKind": a character offset counts code units of the negotiated
// encoding, UTF-16 unless the client and server agree on UTF-8.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 UTF8_ONE_BYTE_LIMIT = 0x80;
constexpr int32 UTF8_TWO_BYTE_LIMIT = 0x800;
constexpr int32 UTF8_THREE_BYTES = 3;
constexpr int32 UTF8_FOUR_BYTES = 4;

// The UTF-8 length of the code point that starts at text[index], and the UTF-16
// units it occupies. Unpaired surrogates count as U+FFFD (three bytes), as
// QString::toUtf8 encodes them.
void MeasureCodePoint(const QString& text, int32 index, int32& outUtf8Bytes, int32& outUtf16Units)
{
    const QChar character = text.at(index);
    if (character.isHighSurrogate() == true && index + 1 < text.size() && text.at(index + 1).isLowSurrogate() == true)
    {
        outUtf8Bytes = UTF8_FOUR_BYTES;
        outUtf16Units = 2;
        return;
    }
    const int32 unit = character.unicode();
    outUtf16Units = 1;
    outUtf8Bytes = unit < UTF8_ONE_BYTE_LIMIT ? 1 : (unit < UTF8_TWO_BYTE_LIMIT ? 2 : UTF8_THREE_BYTES);
}

bool ResolveOffset(const QString& text,
                   const QList<int32>& lineStarts,
                   const QJsonObject& position,
                   LanguagePositionEncoding encoding,
                   int32& outOffset)
{
    const int32 line = position.value(QStringLiteral("line")).toInt(-1);
    const int32 column = position.value(QStringLiteral("character")).toInt(-1);
    if (line < 0 || line >= lineStarts.size() || column < 0)
    {
        return false;
    }
    const int32 start = lineStarts.at(line);
    const int32 end = line + 1 < lineStarts.size() ? lineStarts.at(line + 1) - 1 : static_cast<int32>(text.size());
    if (encoding == LanguagePositionEncoding::Utf16)
    {
        if (column > end - start)
        {
            return false;
        }
        const int32 offset = start + column;
        if (offset > 0 && offset < text.size() && text.at(offset).isLowSurrogate() == true)
        {
            return false;
        }
        outOffset = offset;
        return true;
    }
    int32 bytes = 0;
    int32 offset = start;
    while (bytes < column && offset < end)
    {
        int32 utf8Bytes = 0;
        int32 utf16Units = 0;
        MeasureCodePoint(text, offset, utf8Bytes, utf16Units);
        bytes += utf8Bytes;
        offset += utf16Units;
    }
    if (bytes != column)
    {
        return false;
    }
    outOffset = offset;
    return true;
}

QList<int32> IndexLines(const QString& text)
{
    QList<int32> lines{0};
    qsizetype position = 0;
    while ((position = text.indexOf(QLatin1Char('\n'), position)) >= 0)
    {
        lines.append(static_cast<int32>(++position));
    }
    return lines;
}
} // namespace

QString GetLanguagePositionEncodingName(LanguagePositionEncoding encoding)
{
    return encoding == LanguagePositionEncoding::Utf8 ? QStringLiteral("utf-8") : QStringLiteral("utf-16");
}

bool ReadLanguagePositionEncoding(const QString& name, LanguagePositionEncoding& outEncoding)
{
    if (name.isEmpty() == true || name == QStringLiteral("utf-16"))
    {
        outEncoding = LanguagePositionEncoding::Utf16;
        return true;
    }
    if (name == QStringLiteral("utf-8"))
    {
        outEncoding = LanguagePositionEncoding::Utf8;
        return true;
    }
    return false;
}

QJsonObject MakeLanguagePosition(const QString& text, int32 offset, LanguagePositionEncoding encoding)
{
    const int32 line = static_cast<int32>(text.left(offset).count(QLatin1Char('\n')));
    const int32 start = offset == 0 ? 0 : static_cast<int32>(text.lastIndexOf(QLatin1Char('\n'), offset - 1)) + 1;
    int32 character = offset - start;
    if (encoding == LanguagePositionEncoding::Utf8)
    {
        character = 0;
        int32 index = start;
        while (index < offset)
        {
            int32 utf8Bytes = 0;
            int32 utf16Units = 0;
            MeasureCodePoint(text, index, utf8Bytes, utf16Units);
            character += utf8Bytes;
            index += utf16Units;
        }
    }
    return {{QStringLiteral("line"), line}, {QStringLiteral("character"), character}};
}

bool GetLanguageOffset(const QString& text,
                       const QJsonObject& position,
                       LanguagePositionEncoding encoding,
                       int32& outOffset)
{
    return ResolveOffset(text, IndexLines(text), position, encoding, outOffset);
}

QList<int32> IndexLanguageLines(const QString& text)
{
    return IndexLines(text);
}

bool GetLanguageOffset(const QString& text,
                       const QList<int32>& lineStarts,
                       const QJsonObject& position,
                       LanguagePositionEncoding encoding,
                       int32& outOffset)
{
    return ResolveOffset(text, lineStarts, position, encoding, outOffset);
}
} // namespace ludus::editor
