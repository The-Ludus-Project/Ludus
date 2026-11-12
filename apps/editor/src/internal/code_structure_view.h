#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/code_syntax_service.h"

#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QTextCursor>
#include <QTimer>

class QRect;
class QToolButton;
class QWidget;

namespace ludus::editor
{
/// Per-projection folding and syntax navigation. The document retains its
/// full text and undo history. Collapsed ranges use live Qt text anchors.
class CodeStructureView final : public QObject
{
public:
    explicit CodeStructureView(CodeEditor* editor, const CodeSyntaxService& syntax);
    void UpdateHeader();
    [[nodiscard]] foundation::int32 GetHeaderHeight() const;
    void LayoutHeader(const QRect& rectangle, foundation::int32 gutterWidth);
    void ToggleFold(foundation::int32 line);
    void FoldAll();
    void UnfoldAll();
    void Reveal(const QTextCursor& cursor);
    [[nodiscard]] bool HasFold(foundation::int32 line) const;
    [[nodiscard]] foundation::int32 GetHiddenLineCount(foundation::int32 line) const;
    [[nodiscard]] QString DescribeFold(foundation::int32 line) const;
    [[nodiscard]] QString GetPlaceholder(foundation::int32 line) const;
    [[nodiscard]] foundation::int32 GetLastCollapsedLine(foundation::int32 line) const;
    [[nodiscard]] bool MoveAcrossFold(QKeyEvent* event);
    void DuplicateFold(foundation::int32 sourceLine, foundation::int32 destinationLine);
    [[nodiscard]] QJsonArray SaveFolds() const;
    void RestoreFolds(const QJsonArray& records);

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

    void acceptResult();
    void refreshVisibility();
    void addCollapsed(const CodeSyntaxFold& fold);
    void foldSelection();
    void showOutline();
    void showBreadcrumbs();
    void goToPosition(foundation::int32 position);
    void expandSelection();
    void shrinkSelection();
    void toggleFoldAtCaret();
    void announce(const QString& message);
    [[nodiscard]] QList<foundation::int32> findScopes(foundation::int32 position) const;

    CodeEditor* mEditor;
    const CodeSyntaxService& mSyntax;
    QWidget* mHeader;
    QToolButton* mOutline;
    QToolButton* mBreadcrumbs;
    QToolButton* mSticky;
    QMap<foundation::int32, CodeSyntaxFold> mAvailable;
    QMap<foundation::int32, foundation::int32> mHiddenThrough;
    QMap<foundation::int32, QString> mPlaceholderLabels;
    QList<CollapsedFold> mCollapsed;
    QList<QTextCursor> mSelectionHistory;
    QTextCursor mLastExpandedSelection;
    QJsonArray mRestore;
    QTimer mRepair;
    foundation::int32 mStickyPosition = -1;
    foundation::int32 mDocumentRevision = 0;
    foundation::int32 mPreviousFirstHiddenLine = 0;
    foundation::int32 mPreviousLastHiddenLine = 0;
    bool mIsApplyingVisibility = false;
};
} // namespace ludus::editor
