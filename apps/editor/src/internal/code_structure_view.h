#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_syntax_service.h"
#include "internal/editor_commands.h"

#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QTextCursor>
#include <QTimer>

class QLabel;
class QRect;
class QToolButton;
class QWidget;

namespace ludus::editor
{
struct CodeEditSet;

struct CodeFoldEditRange final
{
    foundation::usize OpeningByte;
    foundation::usize ClosingByte;
    CodeFoldKind Kind;
    QString Label;
};

/// Per-projection folding and syntax navigation. The document retains its
/// full text and undo history. Collapsed ranges use live Qt text anchors.
class CodeStructureView final : public QObject
{
public:
    /// Server folding ranges kept per document; a longer result keeps its first ranges.
    static constexpr foundation::int32 MAX_SERVER_FOLDS = 4096;
    explicit CodeStructureView(CodeEditor* editor, const CodeSyntaxService& syntax);
    void UpdateHeader();
    [[nodiscard]] foundation::int32 GetHeaderHeight() const;
    void LayoutHeader(const QRect& rectangle, foundation::int32 gutterWidth);
    void ToggleFold(foundation::int32 line);
    void FoldAll();
    void UnfoldAll();
    void Reveal(const QTextCursor& cursor);
    void RevealSecondaryCursors(const QList<CodeSecondaryCursor>& cursors);
    [[nodiscard]] bool HasFold(foundation::int32 line) const;
    [[nodiscard]] foundation::int32 GetHiddenLineCount(foundation::int32 line) const;
    [[nodiscard]] QString DescribeFold(foundation::int32 line) const;
    [[nodiscard]] QString GetPlaceholder(foundation::int32 line) const;
    [[nodiscard]] foundation::int32 GetLastCollapsedLine(foundation::int32 line) const;
    [[nodiscard]] bool MoveAcrossFold(QKeyEvent* event);
    void DuplicateFold(foundation::int32 sourceLine, foundation::int32 destinationLine);
    [[nodiscard]] QJsonArray SaveFolds() const;
    [[nodiscard]] QJsonArray
    CaptureMovedFolds(foundation::int32 first, foundation::int32 split, foundation::int32 last) const;
    void RestoreFolds(const QJsonArray& records);
    [[nodiscard]] QList<CodeFoldEditRange> MapFolds(const QByteArray& before, const CodeEditSet& edits) const;
    void RestoreMappedFolds(const QList<CodeFoldEditRange>& folds, const QByteArray& after);
    /// Replace the language server's folding ranges (code editor M4, CE26). A range
    /// is offered only when its kind is one the language's grammar does not produce
    /// and no syntax range starts on its first line, so server ranges never move or
    /// replace syntax folds. The ranges are anchored in the text and move with edits
    /// until the next replacement.
    void SetServerFolds(const QList<CodeSyntaxFold>& folds);
    /// The server ranges currently offered, in line order; for tests.
    [[nodiscard]] QList<CodeSyntaxFold> GetServerFolds() const;

private:
    struct CollapsedFold final
    {
        QTextCursor First;
        QTextCursor Last;
        foundation::int32 OpeningPosition;
        foundation::int32 OpeningLength;
        CodeFoldKind Kind;
        QString Label;
    };

    struct ServerFold final
    {
        QTextCursor First;
        QTextCursor Last;
        CodeFoldKind Kind;
        QString Label;
    };

    void acceptResult();
    void addServerFolds();
    void refreshVisibility();
    void addCollapsed(const CodeSyntaxFold& fold);
    void foldSelection();
    void showOutline();
    void showBreadcrumbs();
    void goToPosition(foundation::int32 position);
    void expandSelection();
    void shrinkSelection();
    void runFoldCommand(CodeFoldCommand operation, foundation::int32 level);
    void
    foldMatching(CodeFoldCommand operation, foundation::int32 level, foundation::int32 first, foundation::int32 last);
    void unfoldMatching(CodeFoldCommand operation, foundation::int32 first, foundation::int32 last);
    [[nodiscard]] foundation::int32 findFoldAtLine(foundation::int32 line, bool shouldFindParent) const;
    void announce(const QString& message);
    [[nodiscard]] QList<foundation::int32> findScopes(foundation::int32 position) const;

    CodeEditor* mEditor;
    const CodeSyntaxService& mSyntax;
    QWidget* mHeader;
    QToolButton* mOutline;
    QToolButton* mBreadcrumbs;
    QToolButton* mSticky;
    QLabel* mFoldStatus;
    QMap<foundation::int32, CodeSyntaxFold> mAvailable;
    QMap<foundation::int32, foundation::int32> mHiddenThrough;
    QMap<foundation::int32, QString> mPlaceholderLabels;
    QList<CollapsedFold> mCollapsed;
    QList<ServerFold> mServerFolds;
    QList<QTextCursor> mSelectionHistory;
    QTextCursor mLastExpandedSelection;
    QJsonArray mRestore;
    QTimer mRepair;
    foundation::int32 mStickyPosition = -1;
    foundation::int32 mDocumentRevision = 0;
    foundation::int32 mPreviousFirstHiddenLine = 0;
    foundation::int32 mPreviousLastHiddenLine = 0;
    bool mIsApplyingVisibility = false;
    bool mHasNamedRegions = false;
};
} // namespace ludus::editor
