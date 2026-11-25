#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/code_navigation.h"

#include <QList>
#include <QPointer>
#include <QString>
#include <QTextCursor>
#include <QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ludus::editor
{
/// The References tool window (code editor M4, CE10): the latest Find References
/// result, grouped by file, kept while the author navigates. A row opens its
/// location through the editor that found it. Once that editor's text changes,
/// rows describe an earlier version: activating one reports it and offers Find
/// Again, which searches from the same symbol, now moved with the edits. The
/// window never edits text. GUI thread only.
class CodeReferencesWindow final : public QWidget
{
    Q_OBJECT
public:
    /// Rows show at most this many UTF-16 units of their line.
    static constexpr foundation::int32 MAX_PREVIEW_UNITS = 160;
    explicit CodeReferencesWindow(QWidget* parentOrNull = nullptr);
    /// Replace the shown result. `inoutOrigin` found it at its editor's caret; the
    /// window keeps a guarded pointer, so `inoutOrigin` may be destroyed first.
    void Show(CodeNavigation& inoutOrigin, const CodeReferenceResults& results);
    /// Open the result at `index` in result order. False, with the reason and its
    /// repair in the window, when the editor has closed or its text changed.
    bool ActivateResult(foundation::int32 index);
    /// Run Find References again from the original symbol. False, with the reason
    /// in the window, when the editor has closed or the command is disabled.
    bool FindAgain();
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetResultCount() const noexcept
    {
        return static_cast<foundation::int32>(mTargets.size());
    }
    /// Whether the finding editor's text changed after the result was shown.
    [[nodiscard]] bool IsOutOfDate() const;
    /// The summary and any failure message, for tests and accessibility.
    [[nodiscard]] QString GetSummary() const;
    [[nodiscard]] QString GetStatus() const;

Q_SIGNALS:
    /// A new result replaced the previous one; the host raises the window.
    void ResultsShown();

private:
    void report(const QString& message);
    void activateItem(const QTreeWidgetItem* item);
    QLabel* mSummary;
    QLabel* mStatus;
    QPushButton* mFindAgain;
    QTreeWidget* mResults;
    QPointer<CodeNavigation> mOrigin;
    /// The request caret, anchored in the finding editor so edits move it.
    QTextCursor mOriginCursor;
    foundation::int32 mOriginRevision = 0;
    QList<CodeNavigationTarget> mTargets;
};
} // namespace ludus::editor
