#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "internal/code_syntax_service.h"

#include <QFontMetricsF>
#include <QList>
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

/// The label an inlay hint draws, with the padding spaces the server asked for.
[[nodiscard]] QString GetCodeInlayHintText(const CodeInlayHint& hint);

/// The width an inlay hint reserves in `metrics`' font: its text plus a margin on
/// each side. The highlighter and the view use it so the drawn hint fills the gap.
[[nodiscard]] foundation::float64 MeasureCodeInlayHint(const QFontMetricsF& metrics, const CodeInlayHint& hint);

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
    void ApplyBlock(QTextBlock block);
    void ApplyVisibleBlocks(QTextBlock first, foundation::int32 lastLine);
    [[nodiscard]] bool UsesVisibleFallback() const;
    void ScheduleReapply();
    /// Replace the language server's semantic tokens (code editor M4, CE10), drawn
    /// over the syntax tokens. Tokens are ordered and disjoint UTF-16 ranges of
    /// this document. Edits move later tokens and drop the ones they touch until
    /// the next replacement; a language change clears them.
    void SetSemanticTokens(const QList<CodeSyntaxToken>& tokens);
    [[nodiscard]] LUDUS_INLINE const QList<CodeSyntaxToken>& GetSemanticTokens() const noexcept
    {
        return mSemanticTokens;
    }
    /// Replace the inlay hints, ordered by position. While shown, each reserves its
    /// measured width after the character before it, as letter spacing in that
    /// character's format; the document text never changes. Edits move later hints
    /// and drop the ones at the edit; a language change clears them.
    void SetInlayHints(const QList<CodeInlayHint>& hints);
    [[nodiscard]] LUDUS_INLINE const QList<CodeInlayHint>& GetInlayHints() const noexcept
    {
        return mInlayHints;
    }
    /// Reserve space for the hints (true) or lay the text out as if there were none.
    void SetInlayHintsShown(bool isShown);
    [[nodiscard]] LUDUS_INLINE bool AreInlayHintsShown() const noexcept
    {
        return mAreInlayHintsShown;
    }

private:
    void highlightBlock(const QString& text) override;
    void applySyntaxTokens();
    void applyTokens(const QList<CodeSyntaxToken>& tokens);
    void moveSemanticTokens(foundation::int32 position, foundation::int32 removed, foundation::int32 added);
    void moveInlayHints(foundation::int32 position, foundation::int32 removed, foundation::int32 added);
    void applyInlayHints();
    void rehighlightEdit(foundation::int32 position, foundation::int32 added);
    void reapplySlice();
    const CodeSyntaxService& mSyntax;
    QList<CodeSyntaxToken> mSemanticTokens;
    QList<CodeInlayHint> mInlayHints;
    bool mAreInlayHintsShown = true;
    QTimer mReapply;
    // The next block to reapply, by number. A stored QTextBlock would keep a fragment index that an edit or a
    // replaced document frees between slices.
    foundation::int32 mNextBlockNumber = 0;
    foundation::int32 mFirstVisibleLine = 0;
    foundation::int32 mLastVisibleLine = 0;
    bool mIsApplyingExplicitly = false;
    bool mIsLargeFile = false;
    foundation::int32 mDocumentRevision = 0;
    foundation::int32 mApplyThroughLine = -1;
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
