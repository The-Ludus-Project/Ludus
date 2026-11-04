#pragma once

#include "internal/code_file_document.h"
#include "internal/code_recovery_journal.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include <memory>

namespace ludus::editor
{
class CodeRecoveryWriter;

/// A buffer recovered from the journal on reopen: the file to open and the
/// draft bytes to re-anchor against its current baseline. Returned by
/// CodeDocumentRegistry::TakeRecoveredBuffers so the area can reopen each tab.
struct CodeRecoveredBuffer final
{
    QString CanonicalPath;
    QByteArray SavedHash;
    QByteArray Draft;
};

/// One authoritative file host per canonical path within a project epoch (CE02).
/// The registry creates a host on first open, hands the same host to every view
/// of that path (so split groups and mirrored views share one draft and one
/// history), and destroys it when its last view closes unless the buffer is
/// dirty. A project reload bumps the epoch; hosts from an older epoch are
/// dropped so no view keeps editing stale identity (CE23: opening executes no
/// project code; this is pure file I/O).
///
/// The registry is also the single mirroring owner: when one view commits an
/// edit, the registry asks the other views of the same host to re-synchronize
/// their projection from the new snapshot. Because that re-synchronization runs
/// through CodeDocumentController::Synchronize, which guards re-entrancy, a
/// mirrored delta is never echoed back as a user edit. GUI thread only.
class CodeDocumentRegistry final : public QObject
{
    Q_OBJECT
public:
    explicit CodeDocumentRegistry(QObject* parentOrNull = nullptr);
    ~CodeDocumentRegistry() override;
    CodeDocumentRegistry(const CodeDocumentRegistry&) = delete;
    CodeDocumentRegistry& operator=(const CodeDocumentRegistry&) = delete;

    /// Set the active project epoch. Any host whose epoch differs is dropped; a
    /// dirty host from the old epoch is kept only until its views are closed, so
    /// recovery and dirty-close prompts (added later in M1) still see it.
    void SetProjectEpoch(foundation::uint64 epoch);
    [[nodiscard]] foundation::uint64 GetProjectEpoch() const noexcept
    {
        return mEpoch;
    }

    /// Acquire the host for `absolutePath`, loading it on the calling thread when
    /// it is not already open. Returns null only when the load failed hard (the
    /// caller then shows the failure and offers Open Externally). The returned
    /// host is owned by the registry; the caller must call ReleaseHost once per
    /// successful acquire when its view closes.
    [[nodiscard]] CodeFileDocument*
    AcquireHost(const QString& absolutePath, CodeFileLoadStatus& outStatus, QString& outExplanation);

    /// Release one view's reference to a host. The host is destroyed when its
    /// last reference is released and its buffer is clean; a dirty host is kept
    /// so its draft is not lost, and is reported through GetDirtyHostCount.
    void ReleaseHost(CodeFileDocument* host);

    /// Number of distinct hosts currently open (one per canonical path/epoch).
    [[nodiscard]] foundation::int32 GetHostCount() const noexcept;
    /// Number of open hosts whose buffer differs from disk, for Save All and the
    /// quit/close prompts.
    [[nodiscard]] foundation::int32 GetDirtyHostCount() const;
    /// The host already open for `absolutePath` in the current epoch, or null.
    [[nodiscard]] CodeFileDocument* FindHostOrNull(const QString& absolutePath) const;

    /// Bind the recovery journal to `projectRoot`. Call on project open, before
    /// restoring tabs, so journal files from a crashed session of the same
    /// project are read and later entries are scoped to it. An empty root (no
    /// project) disables journaling. Reading the journal here does not touch any
    /// host; the caller drains TakeRecoveredBuffers to reopen the buffers.
    void BindRecoveryProject(const QString& projectRoot);

    /// The unsaved buffers found in the journal at the last BindRecoveryProject,
    /// cleared as they are returned. Each names a file and the draft to restore;
    /// the area opens the file and calls RestoreRecoveredDraft, which drops the
    /// draft when the on-disk baseline no longer matches.
    [[nodiscard]] QVector<CodeRecoveredBuffer> TakeRecoveredBuffers();

    /// Request a debounced journal checkpoint. Call after any edit or save that
    /// may have changed a host's dirty state. The snapshot is captured on the UI
    /// thread and the file I/O runs on the background pool; the UI thread never
    /// blocks on disk. A clean host's journal file is removed on the next flush.
    void ScheduleRecoveryCheckpoint();

    /// Flush the journal synchronously on the calling thread, used on project
    /// close and app quit so a final checkpoint is durable before teardown. It
    /// waits for any in-flight background writer to finish first.
    void FlushRecoveryNow();

    /// Delete every journal file for the current project. Used after the restore
    /// pass completes and when recovery is explicitly discarded.
    void ClearRecovery();

Q_SIGNALS:
    /// A host's dirty state may have changed, so owners can refresh Save All and
    /// tab decorations. Emitted after an edit is mirrored.
    void DirtyStateChanged();

private:
    // The host is held through a shared pointer so the entry is copyable, which
    // QHash's copy-on-write nodes require. The registry is still the sole owner:
    // the only strong reference lives in mHosts, so erasing the entry destroys
    // the host; raw CodeFileDocument pointers handed to callers are non-owning.
    struct HostEntry final
    {
        std::shared_ptr<CodeFileDocument> Host;
        foundation::int32 ReferenceCount = 0;
    };
    [[nodiscard]] static QString canonicalKey(const QString& absolutePath);
    [[nodiscard]] static QString recoveryStateDirectory();
    void flushRecovery(bool synchronous);
    void onRecoveryDebounce();

    foundation::uint64 mEpoch = 0;
    // Keyed by canonical path within the current epoch.
    QHash<QString, HostEntry> mHosts;

    // The code-buffer recovery journal (CE03): a bounded, machine-local store of
    // unsaved drafts, written off the UI thread on an idle debounce.
    CodeRecoveryJournal mJournal;
    QTimer mRecoveryTimer;
    // The current background flush, or null; retired with deleteLater on finish.
    CodeRecoveryWriter* mWriterOrNull = nullptr;
    // Canonical paths whose draft was journaled and is not yet known clean, so a
    // host that becomes clean or closes can have its journal file removed.
    QSet<QString> mJournaledPaths;
    // Buffers read from the journal at the last BindRecoveryProject, drained by
    // TakeRecoveredBuffers.
    QVector<CodeRecoveredBuffer> mRecoveredBuffers;
};
} // namespace ludus::editor
