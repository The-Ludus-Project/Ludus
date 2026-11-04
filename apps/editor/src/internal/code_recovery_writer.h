#pragma once

#include "internal/code_recovery_journal.h"

#include <QObject>
#include <QStringList>
#include <QThread>
#include <QVector>

namespace ludus::editor
{
/// One background flush of the code-buffer recovery journal (CE03). The worker
/// owns a copied batch of entries to write and paths to remove, and runs the
/// journal's atomic I/O off the UI thread; it touches no widget and no live
/// host. The UI thread copies every byte into the batch before the worker
/// starts, so there is no shared mutable state during the run.
///
/// Lifetime: created by CodeDocumentRegistry, started once, and retired on
/// finish (deleteLater). The registry runs at most one writer at a time and
/// coalesces bursts behind an idle debounce, mirroring the per-job QThread
/// pattern used elsewhere in the editor (ContentWorker, ProfilingExportJob).
class CodeRecoveryWriter final : public QThread
{
    Q_OBJECT
public:
    /// @param journal A copy of the journal's addressing (directory and project
    ///   scope); storage is stateless so copying is safe to use off-thread.
    /// @param entries Draft snapshots to write, already deep-copied.
    /// @param removals Canonical paths whose journal files should be removed.
    CodeRecoveryWriter(const CodeRecoveryJournal& journal,
                       QVector<CodeRecoveryEntry> entries,
                       QStringList removals,
                       QObject* parentOrNull = nullptr);

private:
    void run() override;

    CodeRecoveryJournal mJournal;
    QVector<CodeRecoveryEntry> mEntries;
    QStringList mRemovals;
};
} // namespace ludus::editor
