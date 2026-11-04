#pragma once

#include "internal/code_document_controller.h"
#include "internal/code_editor.h"

#include <ludus/foundation/base/types.h>

#include <QJsonObject>
#include <QString>
#include <QWidget>

namespace ludus::editor
{
class CodeFileDocument;
class CodeConflictBanner;

/// One open view of a file: a CodeEditor bound to a shared file host, with its
/// own scroll, cursors, wrap and preview state (CE01, CE02). Two tabs over the
/// same host mirror each other's text while keeping their own view state.
/// The tab does not own the host; the registry does. GUI thread only.
class CodeDocumentTab final : public QWidget
{
    Q_OBJECT
public:
    /// @param hostOrNull The shared file host to bind, or null for an empty tab.
    /// @param absolutePath The file's path, used as the tab identity and title.
    CodeDocumentTab(CodeFileDocument* hostOrNull, const QString& absolutePath, QWidget* parentOrNull = nullptr);

    [[nodiscard]] CodeEditor& GetEditor() const noexcept
    {
        return *mEditor;
    }
    [[nodiscard]] CodeDocumentController& GetController() const noexcept
    {
        return *mController;
    }
    [[nodiscard]] CodeFileDocument* GetHostOrNull() const noexcept
    {
        return mHostOrNull;
    }
    [[nodiscard]] const QString& GetPath() const noexcept
    {
        return mPath;
    }

    /// Preview tabs open on a single click and are replaced by the next preview;
    /// editing, saving or a double-click makes the tab permanent (CE01).
    [[nodiscard]] bool IsPreview() const noexcept
    {
        return mIsPreview;
    }
    void SetPreview(bool isPreview);
    /// Promote a preview tab to permanent; a no-op when already permanent.
    void MakePermanent();

    /// Re-synchronize the projection from the host after another view edited it.
    /// The controller's guard prevents the mirrored delta from being re-sent.
    void MirrorFromHost();

    /// Refresh the editor's undo/redo availability from the host.
    void RefreshHistoryAvailability();

    /// Whether the bound host's buffer differs from disk, for the dirty dot.
    [[nodiscard]] bool IsDirty() const;
    /// Whether the bound host is read-only, for the lock badge and the banner.
    [[nodiscard]] bool IsReadOnly() const;

    /// The conflict/explanation banner shown above this view.
    [[nodiscard]] CodeConflictBanner& GetBanner() const noexcept
    {
        return *mBanner;
    }

    /// Serialize this tab's per-project view state: path, preview flag, and the
    /// caret/scroll so a project restore reopens it where the user left off.
    [[nodiscard]] QJsonObject SaveViewState() const;
    /// Restore caret and scroll from SaveViewState output; unknown fields ignored.
    void RestoreViewState(const QJsonObject& state);

Q_SIGNALS:
    /// The user made an edit that must mirror to sibling tabs and update dirty.
    void Edited();
    /// A first edit, save or explicit action promoted this tab to permanent.
    void BecamePermanent();
    /// A bound-edit failure message to surface without losing the draft (CE20).
    void Failed(const QString& message);

private:
    void bindHost();

    CodeEditor* mEditor;
    CodeDocumentController* mController;
    CodeConflictBanner* mBanner;
    CodeFileDocument* mHostOrNull;
    QString mPath;
    bool mIsPreview = false;
};
} // namespace ludus::editor
