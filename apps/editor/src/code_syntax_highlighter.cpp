#include "internal/code_syntax_highlighter.h"

#include <ludus/foundation/base/core.h>

#include <QColor>
#include <QPalette>
#include <QStringView>

// Thanks to The Qt Company, "QSyntaxHighlighter", Qt GUI documentation,
// highlightBlock/previousBlockState: incremental multiline lexical state.
// This lexer supplies Slang coloring only, not semantic shader information.
// https://doc.qt.io/qt-6/qsyntaxhighlighter.html
// Function, constant and user-type colors are same-line token heuristics: a
// name before "(" is a function, an all-capitals name is a macro or constant,
// and a name declared by struct-like keywords or followed by another name is a type.
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
constexpr uint32 LIGHT_FUNCTION = 0x6B6200;
constexpr uint32 DARK_FUNCTION = 0xDCDCAA;
constexpr uint32 LIGHT_CONSTANT = 0xA3195B;
constexpr uint32 DARK_CONSTANT = 0xF29CB8;
constexpr QStringView KEYWORDS =
    u"break case continue default discard do else for if return switch while const static uniform in out inout "
    u"struct class interface enum namespace typedef typealias import export public private internal let var "
    u"func property extension generic where this true false nullptr sizeof defined __target_switch";
/// Keywords whose next name declares a type.
constexpr QStringView TYPE_DECLARATION_KEYWORDS = u"struct class interface enum typealias extension";
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

bool IsIdentifierStart(QChar character)
{
    return character.isLetter() == true || character == QLatin1Char('_');
}

bool IsIdentifierPart(QChar character)
{
    return character.isLetterOrNumber() == true || character == QLatin1Char('_');
}

isize SkipSpaces(QStringView text, isize position)
{
    while (position < text.size() && (text.at(position) == QLatin1Char(' ') || text.at(position) == QLatin1Char('\t')))
    {
        ++position;
    }
    return position;
}

/// Macro and constant convention: two or more characters, capitals, digits and underscores.
bool IsConstantName(QStringView word)
{
    bool hasCapital = false;
    for (const QChar character : word)
    {
        if (character.isLower() == true)
        {
            return false;
        }
        hasCapital = hasCapital == true || character.isUpper() == true;
    }
    return word.size() > 1 && hasCapital == true;
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

bool IsLightCodePalette(const QPalette& palette)
{
    return palette.color(QPalette::Base).lightness() >= LIGHT_PALETTE_THRESHOLD;
}

bool IsSlangKeyword(QStringView word)
{
    return ContainsWord(KEYWORDS, word);
}

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
    const bool isLight = IsLightCodePalette(palette);
    mKeywordFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_KEYWORD : DARK_KEYWORD));
    mTypeFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_TYPE : DARK_TYPE));
    mCommentFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_COMMENT : DARK_COMMENT));
    mStringFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_STRING : DARK_STRING));
    mNumberFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_NUMBER : DARK_NUMBER));
    mFunctionFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_FUNCTION : DARK_FUNCTION));
    mConstantFormat.setForeground(QColor::fromRgb(isLight == true ? LIGHT_CONSTANT : DARK_CONSTANT));
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
    dataOrNull->ExcludedRanges.clear();
    setCurrentBlockState(NORMAL_STATE);
    if (mLanguage == CodeLanguage::PlainText)
    {
        return;
    }
    const QStringView view(text);
    isize position = 0;
    bool isBlockComment = previousBlockState() == BLOCK_COMMENT_STATE;
    bool isTypeNameExpected = false;
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
            dataOrNull->ExcludedRanges.append(
                {static_cast<int32>(start), static_cast<int32>(position), isBlockComment});
            setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mCommentFormat);
            continue;
        }
        if (character == QLatin1Char('/') && next == QLatin1Char('/'))
        {
            dataOrNull->ExcludedRanges.append({static_cast<int32>(start), static_cast<int32>(text.size()), true});
            setFormat(static_cast<int32>(position), static_cast<int32>(text.size() - position), mCommentFormat);
            break;
        }
        if (character == QLatin1Char('"') || character == QLatin1Char('\''))
        {
            bool isClosed = false;
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
                    isClosed = true;
                    break;
                }
            }
            dataOrNull->ExcludedRanges.append(
                {static_cast<int32>(start), static_cast<int32>(position), isClosed == false});
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
        if (IsIdentifierStart(character) == true)
        {
            ++position;
            while (position < text.size() && IsIdentifierPart(text.at(position)) == true)
            {
                ++position;
            }
            const QStringView word = view.sliced(start, position - start);
            const int32 wordStart = static_cast<int32>(start);
            const int32 wordLength = static_cast<int32>(position - start);
            const bool isDeclaredTypeName = isTypeNameExpected;
            isTypeNameExpected = false;
            if (ContainsWord(KEYWORDS, word) == true)
            {
                setFormat(wordStart, wordLength, mKeywordFormat);
                isTypeNameExpected = ContainsWord(TYPE_DECLARATION_KEYWORDS, word);
                continue;
            }
            if (isDeclaredTypeName == true || ContainsWord(TYPES, word) == true || IsNumericType(word) == true)
            {
                setFormat(wordStart, wordLength, mTypeFormat);
                continue;
            }
            if (IsConstantName(word) == true)
            {
                setFormat(wordStart, wordLength, mConstantFormat);
                continue;
            }
            const isize nextTokenStart = SkipSpaces(view, position);
            if (nextTokenStart < text.size() && text.at(nextTokenStart) == QLatin1Char('('))
            {
                setFormat(wordStart, wordLength, mFunctionFormat);
                continue;
            }
            if (nextTokenStart > position && nextTokenStart < text.size()
                && IsIdentifierStart(text.at(nextTokenStart)) == true)
            {
                isize nextEnd = nextTokenStart + 1;
                while (nextEnd < text.size() && IsIdentifierPart(text.at(nextEnd)) == true)
                {
                    ++nextEnd;
                }
                // "Name name" declares a variable, parameter or member of type Name.
                if (ContainsWord(KEYWORDS, view.sliced(nextTokenStart, nextEnd - nextTokenStart)) == false)
                {
                    setFormat(wordStart, wordLength, mTypeFormat);
                }
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
        if (character != QLatin1Char(' ') && character != QLatin1Char('\t'))
        {
            isTypeNameExpected = false;
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
