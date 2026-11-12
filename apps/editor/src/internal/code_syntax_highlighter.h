#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "internal/code_syntax_service.h"

#include <QPalette>
#include <QStringView>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockUserData>
#include <QTextCharFormat>
#include <QVector>

namespace ludus::editor
{
struct CodeBracket final
{
    foundation::int32 Position = 0;
    QChar Character;
};

/// A comment/string span, including an unfinished span's insertion point.
struct CodeExcludedRange final
{
    foundation::int32 Start = 0;
    foundation::int32 End = 0;
    bool IsOpenEnded = false;
};

/// Lexical information cached per text block for matching and automatic bracket pairs.
struct CodeBlockData final : QTextBlockUserData
{
    QVector<CodeBracket> Brackets;
    QVector<CodeExcludedRange> ExcludedRanges;
};

/// Whether a palette's base is light, selecting the light code color set.
[[nodiscard]] bool IsLightCodePalette(const QPalette& palette);

/// Whether a word is a Slang keyword in the lexical coloring profile.
[[nodiscard]] bool IsSlangKeyword(QStringView word);

/// Bounded single-pass block lexer. Qt propagates changed multiline state to
/// subsequent blocks; no compiler or whole-document regular expression runs.
/// Functions, constants and user type names are classified from the
/// surrounding tokens on the same line, not from compiler symbols.
class CodeSyntaxHighlighter final : public QSyntaxHighlighter
{
public:
    explicit CodeSyntaxHighlighter(QTextDocument* document, const CodeSyntaxService& syntax);
    void SetLanguage(CodeLanguage language);
    void SetPalette(const QPalette& palette);
    void SetLargeFileMode(bool isLarge);
    [[nodiscard]] bool IsLargeDocument() const;
    void ApplyVisibleBlocks(QTextBlock first, foundation::int32 lastLine);
    [[nodiscard]] bool UsesVisibleFallback() const;
    void ScheduleReapply();

private:
    void highlightBlock(const QString& text) override;
    void applySyntaxTokens();
    void reapplySlice();
    const CodeSyntaxService& mSyntax;
    QTimer mReapply;
    QTextBlock mNextBlock;
    foundation::int32 mFirstVisibleLine = 0;
    foundation::int32 mLastVisibleLine = 0;
    bool mIsApplyingExplicitly = false;
    bool mIsLargeFile = false;
    foundation::StaticArray<QTextCharFormat, static_cast<foundation::usize>(CodeSyntaxRole::Count)> mRoleFormats;
    CodeLanguage mLanguage = CodeLanguage::PlainText;
    QTextCharFormat mKeywordFormat;
    QTextCharFormat mTypeFormat;
    QTextCharFormat mCommentFormat;
    QTextCharFormat mStringFormat;
    QTextCharFormat mNumberFormat;
    QTextCharFormat mDirectiveFormat;
    QTextCharFormat mFunctionFormat;
    QTextCharFormat mConstantFormat;
};
} // namespace ludus::editor
