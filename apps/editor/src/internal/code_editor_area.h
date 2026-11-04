#pragma once

#include "internal/code_document_registry.h"

#include <ludus/foundation/base/types.h>

#include <QHash>
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
class CodeDocumentTab;

/// The central code-tab area: one tab per open file view, arranged in up to two
/// editor groups, over a shared CodeDocumentRegistry (CE01, CE02). It owns the
/// tab lifetimes, the preview-tab rule, mirroring between groups, and the
/// per-project tab/view-state round-trip.
///
/// Preview tabs: opening a file with a single click opens (or reuses) one
/// preview tab per group; the next preview replaces it. Editing, saving or a
/// double-click makes the tab permanent, and a permanent tab is never replaced.
///
/// Mirroring: when a file is open in both groups, an edit in one group is
/// mirrored into the other group's view through the registry; the controller's
/// guard stops the mirrored delta from being re-sent as an edit.
class CodeEditorArea final : public QWidget
{
    Q_OBJECT
public:
    explicit CodeEditorArea(QWidget* parentOrNull = nullptr);
    ~CodeEditorArea() override;
    CodeEditorArea(const CodeEditorArea&) = delete;
    CodeEditorArea& operator=(const CodeEditorArea&) = delete;

    [[nodiscard]] CodeDocumentRegistry& GetRegistry() noexcept
    {
        return mRegistry;
    }

    /// Set the active project epoch; closes every tab whose host is from an older
    /// epoch (a reload reopens them fresh). Called on project open/reload.
    void SetProjectEpoch(foundation::uint64 epoch);

    /// Set the active project: bump the epoch (closing stale tabs) and bind the
    /// recovery journal to `projectRoot` so a crashed session's unsaved buffers
    /// can be restored. An empty root disables journaling (no project). Call
    /// before RestoreState on project open; follow with RestoreRecoveredBuffers.
    void SetProject(const QString& projectRoot, foundation::uint64 epoch);

    /// Reopen every unsaved buffer the recovery journal held for this project and
    /// re-anchor its draft against the file's current on-disk baseline (CE03). A
    /// buffer whose file is gone or changed on disk is skipped. Safe to call once
    /// after SetProject and RestoreState; the journal is cleared afterwards.
    void RestoreRecoveredBuffers();

    /// Flush a final recovery checkpoint synchronously, for project close and app
    /// quit, so the latest unsaved drafts are journaled before teardown.
    void FlushRecovery();

    /// Open `absolutePath` in the active group. A preview open replaces the
    /// group's current preview tab; a permanent open (double-click, or isPreview
    /// false) opens or focuses a permanent tab. Returns false and reports through
    /// OpenFailed when the file could not be loaded. The view is focused.
    [[nodiscard]] bool OpenFile(const QString& absolutePath, bool isPreview);

    /// Place the second editor group (creating it) and open `absolutePath` in it
    /// so the file is visible side by side with the active group (CE02).
    [[nodiscard]] bool OpenFileInSecondGroup(const QString& absolutePath);

    /// Close the active tab, prompting through DirtyCloseRequested when its host
    /// is dirty.
    void CloseActiveTab();

    /// Run the dirty-close prompt for one tab and act on the answer (CE03): Save
    /// writes the buffer and closes the tab, Don't Save closes it discarding the
    /// draft, and Cancel (the default) leaves it open. The last host reference is
    /// released on close so a discarded draft is not kept for recovery. Returns
    /// true when the tab was closed.
    [[nodiscard]] bool PromptCloseDirtyTab(CodeDocumentTab* tab);

    /// Whether any open buffer has unsaved edits, for the project/app close chain.
    [[nodiscard]] bool HasDirtyBuffers() const;

    /// The dirty-close prompt for closing the project or quitting: list every
    /// unsaved code buffer and ask Save All, Don't Save or Cancel (the default).
    /// Returns true when the caller may proceed (nothing dirty, Save All
    /// succeeded, or Don't Save). A save failure or Cancel returns false. On
    /// Don't Save the drafts are discarded; the recovery journal is cleared so a
    /// clean close does not resurrect them.
    [[nodiscard]] bool ConfirmDiscardAll();

    [[nodiscard]] foundation::int32 GetGroupCount() const;
    [[nodiscard]] foundation::int32 GetTabCount() const;
    /// The active tab, or null when no tab is open.
    [[nodiscard]] CodeDocumentTab* GetActiveTabOrNull() const;
    /// Every open tab across both groups, in group then tab order.
    [[nodiscard]] QList<CodeDocumentTab*> GetTabs() const;

    /// Serialize open code tabs, their group, the active tab and each view's
    /// caret/scroll, for the per-project workspace blob. No file content.
    [[nodiscard]] QJsonObject SaveState() const;
    /// Reopen the tabs named in a SaveState object against the current project.
    /// Missing files are skipped. Malformed input leaves the area unchanged.
    void RestoreState(const QJsonObject& state);

Q_SIGNALS:
    /// A file open failed to load; the owner shows the reason and Open Externally.
    void OpenFailed(const QString& absolutePath, const QString& explanation);
    /// The set of open tabs or their dirty state changed, for Save All and title.
    void TabsChanged();
    /// The active code view changed, so the owner can refresh the status bar.
    void ActiveTabChanged(CodeDocumentTab* tabOrNull);
    /// The user asked to close a dirty tab; the owner runs the Save/Don't Save
    /// prompt and calls ConfirmClose. (The prompt itself is added later in M1.)
    void DirtyCloseRequested(CodeDocumentTab* tab);

private:
    struct Group final
    {
        QTabWidget* Tabs;
        CodeDocumentTab* PreviewOrNull = nullptr;
    };
    [[nodiscard]] QTabWidget* createGroup();
    [[nodiscard]] foundation::int32 activeGroupIndex() const;
    [[nodiscard]] CodeDocumentTab* findTabInGroup(foundation::int32 group, const QString& path) const;
    [[nodiscard]] CodeFileDocument* hostOf(CodeDocumentTab* tab) const;
    void addTabToGroup(foundation::int32 group, CodeDocumentTab* tab, bool isPreview);
    void removeTab(CodeDocumentTab* tab);
    void mirrorEdit(CodeDocumentTab* source);
    void updateTabDecoration(CodeDocumentTab* tab);
    void onCurrentChanged();
    [[nodiscard]] CodeDocumentTab* openIn(foundation::int32 group, const QString& absolutePath, bool isPreview);

    CodeDocumentRegistry mRegistry;
    QSplitter* mSplitter;
    QList<Group> mGroups;
    // Every tab's acquired host, so the matching ReleaseHost runs on close.
    QHash<CodeDocumentTab*, CodeFileDocument*> mTabHosts;
};
} // namespace ludus::editor
