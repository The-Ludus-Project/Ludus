#include "internal/code_syntax_highlighter.h"

#include <ludus/foundation/base/core.h>

#include <ludus/tooling/shader_language/syntax.hpp>

#include "design_tokens.h"
#include "internal/code_file_limits.h"
#include "internal/code_language_registry.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPalette>
#include <QStringView>
#include <QTextDocument>
#include <QTextLayout>

// Thanks to The Qt Company, "QSyntaxHighlighter", Qt GUI documentation,
// highlightBlock/previousBlockState: incremental multiline lexical state.
// This lexer supplies Slang and LUSL coloring only, not semantic shader information;
// LUSL keywords come from the LUSL compiler library itself.
// https://doc.qt.io/qt-6/qsyntaxhighlighter.html
// Function, constant and user-type colors are same-line token heuristics: a
// name before "(" is a function, an all-capitals name is a macro or constant,
// and a name declared by struct-like keywords or followed by another name is a type.
namespace ludus::editor
{
namespace
{
using namespace foundation;
// Qt initializes every block's state to -1. Keeping the normal state
// equal to that value avoids propagating a visible refresh through an entire
// unformatted document (QSyntaxHighlighterPrivate::reformatBlocks).
constexpr int32 NORMAL_STATE = -1;
constexpr int32 BLOCK_COMMENT_STATE = 1;
constexpr int32 LIGHT_PALETTE_THRESHOLD = 128;
using design::ThemeColour;
constexpr foundation::StaticArray<ThemeColour, static_cast<usize>(CodeSyntaxRole::Count)> ROLE_COLOURS{
    ThemeColour::TextPrimary,        ThemeColour::SyntaxKeyword,     ThemeColour::SyntaxControl,
    ThemeColour::SyntaxType,         ThemeColour::SyntaxFunction,    ThemeColour::SyntaxMethod,
    ThemeColour::SyntaxVariable,     ThemeColour::SyntaxParameter,   ThemeColour::SyntaxProperty,
    ThemeColour::SyntaxConstant,     ThemeColour::SyntaxNumber,      ThemeColour::SyntaxString,
    ThemeColour::SyntaxEscape,       ThemeColour::SyntaxComment,     ThemeColour::SyntaxDocumentation,
    ThemeColour::SyntaxPreprocessor, ThemeColour::SyntaxMacro,       ThemeColour::SyntaxNamespace,
    ThemeColour::SyntaxOperator,     ThemeColour::SyntaxPunctuation, ThemeColour::SyntaxInvalid,
};
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

/// LUSL resource and void types; scalar, vector and matrix types are spelled by IsLuslNumericType.
constexpr QStringView LUSL_TYPES =
    u"void Texture2D RWTexture2D SamplerState ConstantBuffer StructuredBuffer RWStructuredBuffer";
/// LUSL keywords whose next name declares a type.
constexpr QStringView LUSL_TYPE_DECLARATION_KEYWORDS = u"struct interface enum extension concept";
/// LUSL's fixed-width scalar spellings (docs/modules/tooling/shader-language.md).
constexpr QStringView LUSL_SCALARS = u"bool int16 uint16 int32 uint32 int64 uint64 float16 float32 float64";

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
bool IsDimension(QChar character)
{
    return character >= QLatin1Char('1') && character <= QLatin1Char('4');
}

/// `float32`, `uint32x2`, `float32x4x4`: a LUSL scalar, vector or matrix.
bool IsLuslNumericType(QStringView word)
{
    constexpr isize VECTOR_SUFFIX_LENGTH = 2;
    constexpr isize MATRIX_SUFFIX_LENGTH = 4;
    isize start = 0;
    while (start < LUSL_SCALARS.size())
    {
        const isize separator = LUSL_SCALARS.indexOf(QLatin1Char(' '), start);
        const isize end = separator < 0 ? LUSL_SCALARS.size() : separator;
        const QStringView scalar = LUSL_SCALARS.sliced(start, end - start);
        start = end + 1;
        if (word.startsWith(scalar) == false)
        {
            continue;
        }
        const QStringView suffix = word.sliced(scalar.size());
        const bool isVector = suffix.size() == VECTOR_SUFFIX_LENGTH && suffix.at(0) == QLatin1Char('x') &&
                              IsDimension(suffix.at(1)) == true;
        const bool isMatrix = suffix.size() == MATRIX_SUFFIX_LENGTH && suffix.at(0) == QLatin1Char('x') &&
                              IsDimension(suffix.at(1)) == true && suffix.at(2) == QLatin1Char('x') &&
                              IsDimension(suffix.at(3)) == true;
        if (suffix.isEmpty() == true || isVector == true || isMatrix == true)
        {
            return true;
        }
    }
    return false;
}

bool IsLuslKeyword(QStringView word)
{
    const foundation::BorrowedView<const std::string_view> keywords = tooling::shader_language::GetKeywords();
    for (usize i = 0; i < keywords.GetSize(); ++i)
    {
        if (word == QLatin1StringView(keywords[i].data(), static_cast<isize>(keywords[i].size())))
        {
            return true;
        }
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

CodeSyntaxHighlighter::CodeSyntaxHighlighter(QTextDocument* document, const CodeSyntaxService& syntax)
    : QSyntaxHighlighter(document), mSyntax(syntax)
{
    LUDUS_ASSERT(document != nullptr);
    // Qt's default contentsChange slot walks every inserted block during open,
    // including blocks the editor has never shown. Use the public per-block
    // highlighter instead, with synchronous edits and visible-first slices.
    QObject::disconnect(document, &QTextDocument::contentsChange, this, nullptr);
    rehighlight(); // Complete Qt's queued initial highlight while the document is empty.
    mDocumentRevision = document->revision();
    connect(document, &QTextDocument::contentsChange, this, [this](int32 position, int32, int32 added) {
        const int32 revision = this->document()->revision();
        if (revision != mDocumentRevision)
        {
            mDocumentRevision = revision;
            rehighlightEdit(position, added);
        }
    });
    mReapply.setSingleShot(true);
    connect(&mReapply, &QTimer::timeout, this, &CodeSyntaxHighlighter::reapplySlice);
}

void CodeSyntaxHighlighter::SetLanguage(CodeLanguage language)
{
    if (mLanguage != language)
    {
        mLanguage = language;
        ScheduleReapply();
    }
}

void CodeSyntaxHighlighter::SetPalette(const QPalette& palette)
{
    const bool isDark = IsLightCodePalette(palette) == false;
    const QColor normalPanel = QColor::fromRgb(design::GetThemeArgb(ThemeColour::SurfacePanel, isDark, false));
    const QColor contrastPanel = QColor::fromRgb(design::GetThemeArgb(ThemeColour::SurfacePanel, isDark, true));
    const QColor contrastText = QColor::fromRgb(design::GetThemeArgb(ThemeColour::TextPrimary, isDark, true));
    const bool isHighContrast = palette.color(QPalette::Base) == contrastPanel &&
                                (contrastPanel != normalPanel || palette.color(QPalette::Text) == contrastText);
    for (usize role = 0; role < ROLE_COLOURS.GetSize(); ++role)
    {
        mRoleFormats[role].setForeground(
            QColor::fromRgb(design::GetThemeArgb(ROLE_COLOURS[role], isDark, isHighContrast)));
    }
    mKeywordFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::Keyword)];
    mTypeFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::Type)];
    mCommentFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::Comment)];
    mStringFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::String)];
    mNumberFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::Number)];
    mFunctionFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::Function)];
    mConstantFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::Constant)];
    mDirectiveFormat = mRoleFormats[static_cast<usize>(CodeSyntaxRole::Preprocessor)];
    ScheduleReapply();
}

void CodeSyntaxHighlighter::SetLargeFileMode(bool isLarge)
{
    if (mIsLargeFile != isLarge)
    {
        mIsLargeFile = isLarge;
        ScheduleReapply();
    }
}

bool CodeSyntaxHighlighter::IsLargeDocument() const
{
    return mIsLargeFile == true || static_cast<usize>(document()->blockCount()) > CODE_FILE_NORMAL_LIMIT_LINES ||
           static_cast<usize>(document()->characterCount()) > CODE_FILE_NORMAL_LIMIT_BYTES;
}

bool CodeSyntaxHighlighter::UsesVisibleFallback() const
{
    constexpr int32 EAGER_LEXER_LINE_LIMIT = 4096;
    return IsLargeDocument() == true || document()->blockCount() > EAGER_LEXER_LINE_LIMIT;
}

void CodeSyntaxHighlighter::rehighlightEdit(int32 position, int32 added)
{
    const QTextBlock first = document()->findBlock(position);
    if (first.isValid() == false || mLanguage == CodeLanguage::PlainText)
    {
        return;
    }
    const QTextBlock last = document()->findBlock(qMin(position + added, document()->characterCount() - 1));
    if (UsesVisibleFallback() == true && last.blockNumber() > first.blockNumber() + 1)
    {
        // Large paste/open takes the same path as scrolling. Its first visible
        // blocks are colored by the view's queued refresh after Qt lays out.
        mReapply.stop();
        return;
    }
    constexpr int32 INLINE_BLOCK_BUDGET = 32;
    const int32 end = qMin(last.blockNumber(), first.blockNumber() + INLINE_BLOCK_BUDGET - 1);
    mIsApplyingExplicitly = true;
    mApplyThroughLine = first.blockNumber() + INLINE_BLOCK_BUDGET - 1;
    for (QTextBlock block = first; block.isValid() == true && block.blockNumber() <= end; block = block.next())
    {
        if (static_cast<usize>(block.length() - 1) <= CODE_LONG_LINE_UTF16_UNITS)
        {
            rehighlightBlock(block);
        }
    }
    mApplyThroughLine = -1;
    mIsApplyingExplicitly = false;
}

void CodeSyntaxHighlighter::ApplyBlock(QTextBlock block)
{
    if (block.isValid() == false || static_cast<usize>(block.length() - 1) > CODE_LONG_LINE_UTF16_UNITS)
    {
        return;
    }
    mApplyThroughLine = block.blockNumber();
    rehighlightBlock(block);
    mApplyThroughLine = -1;
}

void CodeSyntaxHighlighter::ApplyVisibleBlocks(QTextBlock first, int32 lastLine)
{
    mFirstVisibleLine = first.blockNumber();
    mLastVisibleLine = lastLine;
    mIsApplyingExplicitly = true;
    mApplyThroughLine = lastLine;
    for (QTextBlock block = first; block.isValid() == true && block.blockNumber() <= lastLine; block = block.next())
    {
        if (block.isVisible() == true && static_cast<usize>(block.length() - 1) <= CODE_LONG_LINE_UTF16_UNITS)
        {
            rehighlightBlock(block);
        }
    }
    mApplyThroughLine = -1;
    mIsApplyingExplicitly = false;
}

void CodeSyntaxHighlighter::ScheduleReapply()
{
    mNextBlockNumber = 0;
    mReapply.start(0);
}

void CodeSyntaxHighlighter::reapplySlice()
{
    if (document() == nullptr || IsLargeDocument() == true)
    {
        return;
    }
    constexpr int32 SLICE_MILLISECONDS = 2;
    QElapsedTimer clock;
    clock.start();
    mIsApplyingExplicitly = true;
    // Find the block again for each slice: the document may have been edited or replaced since the last one.
    QTextBlock block = document()->findBlockByNumber(mNextBlockNumber);
    while (block.isValid() == true && clock.elapsed() < SLICE_MILLISECONDS)
    {
        ApplyBlock(block);
        block = block.next();
    }
    mIsApplyingExplicitly = false;
    if (block.isValid() == true)
    {
        mNextBlockNumber = block.blockNumber();
        mReapply.start(0);
    }
}

void CodeSyntaxHighlighter::applySyntaxTokens()
{
    const CodeSyntaxResult* resultOrNull = mSyntax.GetCurrentResultOrNull();
    if (resultOrNull == nullptr)
    {
        return;
    }
    const QTextBlock block = currentBlock();
    const int32 start = block.position();
    const int32 end = start + block.length() - 1;
    qsizetype first = 0;
    qsizetype last = resultOrNull->Tokens.size();
    while (first < last)
    {
        const qsizetype middle = first + (last - first) / 2;
        if (resultOrNull->Tokens.at(middle).GetEnd() <= start)
        {
            first = middle + 1;
        }
        else
        {
            last = middle;
        }
    }
    for (qsizetype index = first; index < resultOrNull->Tokens.size(); ++index)
    {
        const CodeSyntaxToken& token = resultOrNull->Tokens.at(index);
        if (token.GetStart() >= end)
        {
            break;
        }
        const int32 clippedStart = qMax(start, token.GetStart());
        const int32 clippedEnd = qMin(end, token.GetEnd());
        setFormat(clippedStart - start, clippedEnd - clippedStart, mRoleFormats[static_cast<usize>(token.GetRole())]);
    }
}

void CodeSyntaxHighlighter::highlightBlock(const QString& text)
{
    if (mApplyThroughLine >= 0 && currentBlock().blockNumber() > mApplyThroughLine)
    {
        // Qt asks for one more block when a multiline state changes. Retain
        // its formats and state here; a later visible or timed pass updates it.
        const QTextLayout* layoutOrNull = currentBlock().layout();
        if (layoutOrNull != nullptr)
        {
            for (const QTextLayout::FormatRange& format : layoutOrNull->formats())
            {
                setFormat(format.start, format.length, format.format);
            }
        }
        return;
    }
    if (mLanguage == CodeLanguage::PlainText || static_cast<usize>(text.size()) > CODE_LONG_LINE_UTF16_UNITS)
    {
        return;
    }
    if (mIsApplyingExplicitly == false && UsesVisibleFallback() == true)
    {
        const int32 line = currentBlock().blockNumber();
        if (line < mFirstVisibleLine || line > mLastVisibleLine)
        {
            return;
        }
    }
    CodeBlockData* dataOrNull = static_cast<CodeBlockData*>(currentBlockUserData());
    if (dataOrNull == nullptr)
    {
        dataOrNull = new CodeBlockData;
        setCurrentBlockUserData(dataOrNull);
    }
    dataOrNull->Brackets.clear();
    dataOrNull->ExcludedRanges.clear();
    setCurrentBlockState(NORMAL_STATE);
    const QStringView view(text);
    const BorrowedView<const char> lineComment = CodeLanguageRegistry::Get().Describe(mLanguage).LineComment;
    const QLatin1StringView comment(lineComment.GetDataOrNull(), static_cast<qsizetype>(lineComment.GetSize()));
    const bool hasCComments = comment == QLatin1StringView("//");
    const bool isLusl = mLanguage == CodeLanguage::Lusl;
    const auto isKeyword = [isLusl](QStringView word) { // ludus-style: inferred-type: lambda closure
        return isLusl == true ? IsLuslKeyword(word) : ContainsWord(KEYWORDS, word);
    };
    const auto declaresType = [isLusl](QStringView word) { // ludus-style: inferred-type: lambda closure
        return ContainsWord(isLusl == true ? LUSL_TYPE_DECLARATION_KEYWORDS : TYPE_DECLARATION_KEYWORDS, word);
    };
    const auto isType = [isLusl](QStringView word) { // ludus-style: inferred-type: lambda closure
        // LUSL also accepts the Slang aliases (`float3`), which IsNumericType spells.
        return isLusl == true ? ContainsWord(LUSL_TYPES, word) == true || IsLuslNumericType(word) == true ||
                                    IsNumericType(word) == true
                              : ContainsWord(TYPES, word) == true || IsNumericType(word) == true;
    };
    isize position = 0;
    bool isBlockComment = previousBlockState() == BLOCK_COMMENT_STATE;
    bool isTypeNameExpected = false;
    while (position < text.size())
    {
        const isize start = position;
        const QChar character = text.at(position);
        const QChar next = position + 1 < text.size() ? text.at(position + 1) : QChar();
        if (hasCComments == true && (isBlockComment == true || (character == u'/' && next == u'*')))
        {
            const isize end = text.indexOf(QStringLiteral("*/"), position + (isBlockComment == true ? 0 : 2));
            isBlockComment = end < 0;
            position = isBlockComment == true ? text.size() : end + 2;
            dataOrNull->ExcludedRanges.append(
                {static_cast<int32>(start), static_cast<int32>(position), isBlockComment});
            setFormat(static_cast<int32>(start), static_cast<int32>(position - start), mCommentFormat);
            continue;
        }
        if (comment.isEmpty() == false && comment != QLatin1StringView("#") &&
            view.sliced(position).startsWith(comment) == true)
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
        if (character == QLatin1Char('#') && (hasCComments == true || mLanguage == CodeLanguage::CMake))
        {
            if (mLanguage == CodeLanguage::CMake)
            {
                dataOrNull->ExcludedRanges.append({static_cast<int32>(start), static_cast<int32>(text.size()), true});
                setFormat(static_cast<int32>(start), static_cast<int32>(text.size() - start), mCommentFormat);
                break;
            }
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
            if (isKeyword(word) == true)
            {
                setFormat(wordStart, wordLength, mKeywordFormat);
                isTypeNameExpected = declaresType(word);
                continue;
            }
            if (isDeclaredTypeName == true || isType(word) == true)
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
            if (nextTokenStart > position && nextTokenStart < text.size() &&
                IsIdentifierStart(text.at(nextTokenStart)) == true)
            {
                isize nextEnd = nextTokenStart + 1;
                while (nextEnd < text.size() && IsIdentifierPart(text.at(nextEnd)) == true)
                {
                    ++nextEnd;
                }
                // "Name name" declares a variable, parameter or member of type Name.
                if (isKeyword(view.sliced(nextTokenStart, nextEnd - nextTokenStart)) == false)
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
    applySyntaxTokens();
}
} // namespace ludus::editor
