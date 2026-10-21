#include "internal/code_syntax_highlighter.h"

#include <ludus/foundation/base/core.h>

#include <QColor>
#include <QPalette>
#include <QStringView>

// Thanks to The Qt Company, "QSyntaxHighlighter", Qt GUI documentation,
// highlightBlock/previousBlockState: incremental multiline lexical state.
// This lexer supplies Slang coloring only, not semantic shader information.
// https://doc.qt.io/qt-6/qsyntaxhighlighter.html
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 NORMAL_STATE = 0;
constexpr int32 BLOCK_COMMENT_STATE = 1;
constexpr int32 LIGHT_PALETTE_THRESHOLD = 128;
constexpr uint32 LIGHT_KEYWORD = 0x7041A0;
constexpr uint32 DARK_KEYWORD = 0xC5A5E8;
constexpr uint32 LIGHT_TYPE = 0x006B78;
constexpr uint32 DARK_TYPE = 0x78CCD5;
constexpr uint32 LIGHT_COMMENT = 0x526650;
constexpr uint32 DARK_COMMENT = 0xA0B79A;
constexpr uint32 LIGHT_STRING = 0x965019;
constexpr uint32 DARK_STRING = 0xE4B284;
constexpr uint32 LIGHT_NUMBER = 0x265C9B;
constexpr uint32 DARK_NUMBER = 0x94BFEF;
constexpr QStringView KEYWORDS =
    u"break case continue default discard do else for if return switch while const static uniform in out inout "
    u"struct class interface enum namespace typedef typealias import export public private internal let var "
    u"func property extension generic where this true false nullptr sizeof __target_switch";
constexpr QStringView TYPES = u"void bool int uint int8_t uint8_t int16_t uint16_t int32_t uint32_t int64_t uint64_t "
                              u"half float double vector matrix SamplerState SamplerComparisonState Texture1D "
                              u"Texture2D Texture3D TextureCube RWTexture2D StructuredBuffer RWStructuredBuffer "
                              u"ByteAddressBuffer RWByteAddressBuffer ConstantBuffer RaytracingAccelerationStructure";

bool ContainsWord(QStringView words, QStringView word)
{
    isize start = 0;
    while (start < words.size())
    {
        const isize separator = words.indexOf(QLatin1Char(' '), start);
        const isize end = separator < 0 ? words.size() : separator;
        if (words.sliced(start, end - start) == word)
        {
            return true;
        }
        start = end + 1;
    }
    return false;
}

bool IsNumericType(QStringView word)
{
    for (const QStringView prefix : {QStringView(u"float"),
                                     QStringView(u"half"),
                                     QStringView(u"double"),
                                     QStringView(u"int"),
                                     QStringView(u"uint"),
                                     QStringView(u"bool")})
    {
        if (word.startsWith(prefix) == false || word.size() == prefix.size())
        {
            continue;
        }
        const QStringView suffix = word.sliced(prefix.size());
        constexpr isize MATRIX_SUFFIX_LENGTH = 3;
        const auto isDimension = [](QChar character) {
            return character >= QLatin1Char('1') && character <= QLatin1Char('4');
        };
        return (suffix.size() == 1 && isDimension(suffix.at(0)) == true) ||
               (suffix.size() == MATRIX_SUFFIX_LENGTH && isDimension(suffix.at(0)) == true &&
                suffix.at(1) == QLatin1Char('x') && isDimension(suffix.at(2)) == true);
    }
    return false;
}
} // namespace

CodeSyntaxHighlighter::CodeSyntaxHighlighter(QTextDocument* document) : QSyntaxHighlighter(document)
{
    LUDUS_ASSERT(document != nullptr);
}

void CodeSyntaxHighlighter::SetLanguage(CodeLanguage language)
{
    if (mLanguage != language)
    {
        mLanguage = language;
        rehighlight();
    }
}

void CodeSyntaxHighlighter::SetPalette(const QPalette& palette)
{
    const bool isLight = palette.color(QPalette::Base).lightness() >= LIGHT_PALETTE_THRESHOLD;
    mKeywordFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_KEYWORD : DARK_KEYWORD));
    mTypeFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_TYPE : DARK_TYPE));
    mCommentFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_COMMENT : DARK_COMMENT));
    mStringFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_STRING : DARK_STRING));
    mNumberFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_NUMBER : DARK_NUMBER));
    mDirectiveFormat = mKeywordFormat;
    rehighlight();
}

void CodeSyntaxHighlighter::highlightBlock(const QString& text)
{
    CodeBlockData* dataOrNull = static_cast<CodeBlockData*>(currentBlockUserData());
    if (dataOrNull == nullptr)
    {
        dataOrNull = new CodeBlockData;
        setCurrentBlockUserData(dataOrNull);
    }
    dataOrNull->Brackets.clear();
    setCurrentBlockState(NORMAL_STATE);
    if (mLanguage == CodeLanguage::PlainText)
    {
        return;
    }
    const QStringView view(text);
    isize position = 0;
    bool isBlockComment = previousBlockState() == BLOCK_COMMENT_STATE;
    while (position < text.size())
    {
        const isize start = position;
        const QChar character = text.at(position);
        const QChar next = position + 1 < text.size() ? text.at(position + 1) : QChar();
        if (isBlockComment == true || (character == QLatin1Char('/') && next == QLatin1Char('*')))
        {
            const isize end = text.indexOf(QStringLiteral("*/"), position + (isBlockComment == true ? 0 : 2));
            isBlockComment = end < 0;
            position = isBlockComment == true ? text.size() : end + 2;
            setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mCommentFormat);
            continue;
        }
        if (character == QLatin1Char('/') && next == QLatin1Char('/'))
        {
            setFormat(static_cast<int32>(position), static_cast<int32>(text.size() - position), mCommentFormat);
            break;
        }
        if (character == QLatin1Char('"') || character == QLatin1Char('\''))
        {
            ++position;
            while (position < text.size())
            {
                const QChar current = text.at(position++);
                if (current == QLatin1Char('\\') && position < text.size())
                {
                    ++position;
                }
                else if (current == character)
                {
                    break;
                }
            }
            setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mStringFormat);
            continue;
        }
        if (character == QLatin1Char('#'))
        {
            ++position;
            while (position < text.size() && text.at(position).isLetter() == true)
            {
                ++position;
            }
            setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mDirectiveFormat);
            continue;
        }
        if (character.isLetter() == true || character == QLatin1Char('_'))
        {
            ++position;
            while (position < text.size() &&
                   (text.at(position).isLetterOrNumber() == true || text.at(position) == QLatin1Char('_')))
            {
                ++position;
            }
            const QStringView word = view.sliced(start, position - start);
            if (ContainsWord(KEYWORDS, word) == true)
            {
                setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mKeywordFormat);
            }
            else if (ContainsWord(TYPES, word) == true || IsNumericType(word) == true)
            {
                setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mTypeFormat);
            }
            continue;
        }
        if (character.isDigit() == true)
        {
            ++position;
            while (position < text.size())
            {
                const QChar current = text.at(position);
                const QChar previous = text.at(position - 1);
                const bool isExponentSign = (current == QLatin1Char('+') || current == QLatin1Char('-')) &&
                                            (previous == QLatin1Char('e') || previous == QLatin1Char('E'));
                if (current.isLetterOrNumber() == false && current != QLatin1Char('.') && isExponentSign == false)
                {
                    break;
                }
                ++position;
            }
            setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mNumberFormat);
            continue;
        }
        if (QStringView(u"()[]{}").contains(character) == true)
        {
            dataOrNull->Brackets.append({static_cast<int32>(position), character});
        }
        ++position;
    }
    setCurrentBlockState(isBlockComment == true ? BLOCK_COMMENT_STATE : NORMAL_STATE);
}
} // namespace ludus::editor
