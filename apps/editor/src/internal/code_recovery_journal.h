#pragma once

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ludus::editor
{
/// One unsaved code buffer captured for crash recovery: the file it belongs to,
/// the hash of the on-disk baseline the draft was edited against, and the draft
/// bytes themselves (UTF-8, no byte-order mark). A journal entry is written only
/// while the host is dirty, and removed once the host is clean or closed.
struct CodeRecoveryEntry final
{
    /// The host's canonical absolute path at capture time.
    QString CanonicalPath;
    /// SHA-256 (raw, not hex) of the saved baseline the draft was edited over, so
    /// a restore is skipped when the file changed on disk since the crash.
    QByteArray SavedHash;
    /// The unsaved draft bytes (UTF-8, no BOM).
    QByteArray Draft;
};

/// A bounded, machine-local journal of unsaved code buffers for crash recovery
/// (CE03). It is owned by CodeDocumentRegistry, lives under the project's local
/// state directory, and is written off the UI thread on an idle debounce; this
/// class is the pure storage layer and performs no debouncing or threading of
/// its own. One file per canonical path, named by a hash of that path, holding a
/// checksum-prefixed JSON envelope. Reads validate the checksum, the schema and
/// the project scope and never execute project code. The journal never writes
/// into the project tree: on a restore the caller re-anchors the recovered draft
/// against the file's current on-disk baseline.
///
/// Thread model: Write and Remove are called from the background pool with a
/// borrowed snapshot copy; Read, List, Clear, Scope and the directory accessor
/// are GUI-thread setup/teardown. A single CodeRecoveryJournal instance is not
/// shared across threads concurrently; the registry serializes capture (UI) and
/// flush (one worker) so only one writer runs at a time.
class CodeRecoveryJournal final
{
public:
    /// The largest single journal envelope the store will write or accept, sized
    /// for a large dirty buffer plus base64 expansion and the small header. This
    /// is the journal's own owner constant for its per-entry limit (decision log,
    /// "Code-buffer recovery journal owner").
    static constexpr foundation::isize MAX_RECOVERY_ENTRY_BYTES = static_cast<foundation::isize>(48) * 1024 * 1024;
    /// The most journal entries kept for one project; older files beyond this are
    /// pruned so a long crash history cannot grow without bound.
    static constexpr foundation::int32 MAX_RECOVERY_ENTRIES = 256;

    /// Construct over `stateDirectory` (the machine-local AppLocalData root for
    /// the editor); an empty directory disables the journal. `projectRoot` scopes
    /// entries to one project, so reopening a different project ignores them.
    CodeRecoveryJournal(const QString& stateDirectory, const QString& projectRoot);

    /// Rebind the journal to a different project. Entries already on disk for the
    /// new project are retained for its next restore; the old project's files are
    /// left in place for that project to recover or prune.
    void SetProject(const QString& projectRoot);

    /// The absolute directory this journal reads and writes, empty when disabled.
    [[nodiscard]] const QString& GetDirectory() const noexcept
    {
        return mDirectory;
    }

    /// Atomically write one entry's journal file. Overwrites the file for the
    /// same canonical path. Returns false (and writes nothing) when the store is
    /// disabled, the entry exceeds MAX_RECOVERY_ENTRY_BYTES, or the write fails.
    /// Background-pool safe; touches only this journal's directory.
    [[nodiscard]] bool Write(const CodeRecoveryEntry& entry) const;

    /// Remove the journal file for `canonicalPath`, if any. Returns true when the
    /// file is gone afterwards (including when it was already absent).
    /// Background-pool safe.
    [[nodiscard]] bool Remove(const QString& canonicalPath) const;

    /// Read every valid entry for the current project, newest first, dropping any
    /// file that is damaged, too large, linked or out of scope. Bounded by
    /// MAX_RECOVERY_ENTRIES. GUI thread.
    [[nodiscard]] QVector<CodeRecoveryEntry> Read() const;

    /// Delete every journal file for the current project. GUI thread; used after
    /// a restore pass and when recovery is explicitly discarded.
    void Clear() const;

private:
    [[nodiscard]] QString slotPath(const QString& canonicalPath) const;
    [[nodiscard]] bool isLocalFile(const QString& path) const;
    [[nodiscard]] QStringList journalFiles() const;

    QString mStateDirectory;
    QString mProjectRoot;
    /// The per-project subdirectory under mStateDirectory; empty when disabled.
    QString mDirectory;
};
} // namespace ludus::editor
