#pragma once

// DocumentWell: the editor's central, Visual Studio-style tabbed document area
// (private editor header, not installed).
//
// The well presents registered document views (World, render graph, scripts,
// project settings, ...) as tabs in one or more tab groups laid out by a
// splitter. It owns presentation only: which views are open, their order, the
// active group and the split. It never owns a document's draft, history or
// files. Closing a tab hides that view and keeps the view (and its draft) alive
// so it can be reopened; discarding drafts stays with each document's owner.
// An owner-closed view instead asks its owner to close it, and the owner takes
// the view back with RemoveDocument.
// See docs/development/editor-workspace.md#workspace-layout.

#include <ludus/foundation/base/types.h>

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QSplitter;
class QTabWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Most tab groups the well shows; a split request beyond it is refused.
inline constexpr foundation::int32 MAX_DOCUMENT_GROUPS = 4;

/// What closing a registered view's tab does.
enum class DocumentCloseMode : foundation::uint8
{
    /// The well hides the view and keeps it registered for a later OpenDocument.
    Hide,
    /// The well emits DocumentWell::CloseRequested and leaves the view open. Its owner
    /// decides (for example after asking to discard a dirty draft) and calls
    /// DocumentWell::RemoveDocument to close it.
    OwnerCloses,
};

/// Central tabbed document area with Visual Studio-style tab groups.
///
/// All methods are UI-thread only. Registered views must be distinct, non-null
/// widgets with a non-empty, unique objectName (used as the persisted ID). The
/// well reparents registered views and keeps them alive until it is destroyed.
class DocumentWell final : public QWidget
{
    Q_OBJECT
public:
    explicit DocumentWell(QWidget* parentOrNull = nullptr);
    ~DocumentWell() override;
    DocumentWell(const DocumentWell&) = delete;
    DocumentWell& operator=(const DocumentWell&) = delete;

    /// Register a document view with its tab title. The view starts closed and available.
    /// The well reparents the view and keeps it alive until RemoveDocument or destruction.
    /// @param closeMode Whether a tab close hides the view (the default) or asks its owner
    /// through CloseRequested.
    /// @pre document is not already registered and has a unique, non-empty objectName.
    void AddDocument(QWidget* document, const QString& title, DocumentCloseMode closeMode = DocumentCloseMode::Hide);
    /// Unregister a view and hand it back to the caller, who then owns it. Its tab is
    /// closed, empty groups are removed, and ActiveDocumentChanged and LayoutChanged are
    /// emitted when the arrangement changed. The view is hidden and unparented, not
    /// deleted. Later SaveState output no longer names it, and RestoreState skips its ID.
    /// Safe to call from a CloseRequested handler.
    /// @return False for an unregistered view; nothing changes then.
    bool RemoveDocument(QWidget* document);
    /// Open (when closed) and activate a registered, available view, then give it focus.
    /// @return False for an unregistered or unavailable view; nothing changes then.
    bool OpenDocument(QWidget* document);
    /// Activate the next (isForward) or previous open document in group/tab order,
    /// wrapping at either end and focusing the view. Closed views remain closed.
    /// @return False when no document is open; nothing changes then.
    bool ActivateAdjacentDocument(bool isForward);
    /// Close a view's tab, the user's close command. A Hide view stays registered and
    /// alive for a later OpenDocument. An OwnerCloses view stays open, and CloseRequested
    /// is emitted for it. Nothing happens for a view that is not open.
    void CloseDocument(QWidget* document);
    /// Close every open view's tab, as CloseDocument does for each. Hide views close
    /// first and empty groups are removed; then CloseRequested is emitted for each
    /// still-open OwnerCloses view in group and tab order.
    void CloseAllDocuments();
    /// Allow or forbid opening a view; making it unavailable closes its tab. Used for
    /// views that need a loaded project (or, for the start page, its absence).
    void SetDocumentAvailable(QWidget* document, bool isAvailable);
    [[nodiscard]] bool IsDocumentAvailable(const QWidget* document) const;
    [[nodiscard]] bool IsDocumentOpen(const QWidget* document) const;
    /// True when the view is the current tab of any group, including a background group.
    /// Independent of whether the containing editor window is currently shown.
    [[nodiscard]] bool IsDocumentCurrent(const QWidget* document) const;
    /// True for a registered view whose tab close is decided by its owner.
    [[nodiscard]] bool IsDocumentOwnerClosed(const QWidget* document) const;
    /// Change a view's tab title, for example to add a `*` while it has unsaved changes.
    void SetDocumentTitle(QWidget* document, const QString& title);
    [[nodiscard]] QString GetDocumentTitle(const QWidget* document) const;
    /// Current view of the active tab group, or null when no document is open.
    [[nodiscard]] QWidget* GetActiveDocumentOrNull() const;
    /// Registered views in registration order.
    [[nodiscard]] QList<QWidget*> GetDocuments() const;

    /// Number of tab groups (at least one, possibly empty).
    [[nodiscard]] foundation::int32 GetGroupCount() const;
    /// Tab group showing document, or -1 when it is closed.
    [[nodiscard]] foundation::int32 GetGroupIndex(const QWidget* document) const;
    /// Index of the group that receives newly opened documents.
    [[nodiscard]] foundation::int32 GetActiveGroupIndex() const;
    /// Split orientation: Horizontal places groups side by side (VS "vertical tab group").
    [[nodiscard]] Qt::Orientation GetSplitOrientation() const;
    /// True when the active document can move into a new group with this orientation:
    /// the active group holds at least two documents, the limit is not reached, and any
    /// existing split already has the same orientation.
    [[nodiscard]] bool CanSplit(Qt::Orientation orientation) const;
    /// Move the active document into a new group after the active one.
    /// @return False when CanSplit is false; nothing changes then.
    bool SplitActiveDocument(Qt::Orientation orientation);
    /// Move the active document into the neighbouring group (offset +1 or -1).
    /// @return False when there is no such group or document.
    bool MoveActiveDocument(foundation::int32 offset);
    /// Show two distinct registered, available views in horizontal tab groups, focusing firstDocument.
    /// Reuses existing groups and their sizes; a one-group well gets an equal split. Other tabs,
    /// drafts and widget identities survive. Only firstDocument may move to another group.
    /// False for unavailable/unregistered views or the same view, without changing the arrangement.
    bool ShowDocumentsSideBySide(QWidget* firstDocument, QWidget* secondDocument);
    /// Move every open document into the first group and remove the others.
    void MergeGroups();

    /// Serialize open documents, groups, split orientation and sizes. Contains only
    /// registered objectNames and numbers; no paths or document content.
    [[nodiscard]] QJsonObject SaveState() const;
    /// Restore a SaveState object. Unknown (including removed) or unavailable IDs are
    /// skipped. Malformed input returns false and leaves the current arrangement unchanged.
    /// Restoring only rearranges tabs: views absent from the state are hidden, and no
    /// CloseRequested is emitted.
    bool RestoreState(const QJsonObject& state);

Q_SIGNALS:
    /// The active document changed (null when the well became empty).
    void ActiveDocumentChanged(QWidget* documentOrNull);
    /// Documents were opened, closed, moved or regrouped.
    void LayoutChanged();
    /// The user asked to close an open OwnerCloses view (tab close button, tab menu,
    /// CloseDocument or CloseAllDocuments). The view stays open until its owner calls
    /// RemoveDocument; ignoring the request keeps it open.
    void CloseRequested(QWidget* document);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry final
    {
        QWidget* Document;
        QString Title;
        bool IsAvailable;
        DocumentCloseMode CloseMode;
    };
    [[nodiscard]] QTabWidget* createGroup(foundation::int32 index);
    [[nodiscard]] QTabWidget* activeGroup() const;
    [[nodiscard]] const Entry* findEntryOrNull(const QWidget* document) const;
    [[nodiscard]] Entry* findEntryOrNull(const QWidget* document);
    [[nodiscard]] QTabWidget* groupOfOrNull(const QWidget* document) const;
    void insertInto(QTabWidget* group, QWidget* document, foundation::int32 index);
    void detach(QWidget* document);
    void hideDocument(QWidget* document);
    void requestClose(const QList<QWidget*>& documents);
    void removeEmptyGroups();
    void setActiveGroup(QTabWidget* group);
    void publishActiveDocument();
    void showTabMenu(QTabWidget* group, foundation::int32 index, const QPoint& position);

    QSplitter* mSplitter;
    QList<QTabWidget*> mGroups;
    QList<Entry> mEntries;
    QTabWidget* mActiveGroupOrNull = nullptr;
    QWidget* mPublishedDocumentOrNull = nullptr;
    // Closed views are parented here, hidden, so they stay alive and keep their state.
    QWidget* mParking;
};
} // namespace ludus::editor
