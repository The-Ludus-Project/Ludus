#include "internal/code_recovery_writer.h"

#include <utility>

// The background flush of the code-buffer recovery journal. The registry hands
// this worker a deep copy of the dirty drafts to write and the clean/closed
// paths to remove; the run() body performs only the journal's atomic file I/O,
// off the UI thread, and never reads a live host or widget.

namespace ludus::editor
{
CodeRecoveryWriter::CodeRecoveryWriter(const CodeRecoveryJournal& journal,
                                       QVector<CodeRecoveryEntry> entries,
                                       QStringList removals,
                                       QObject* parentOrNull)
    : QThread(parentOrNull), mJournal(journal), mEntries(std::move(entries)), mRemovals(std::move(removals))
{
}

void CodeRecoveryWriter::run()
{
    for (const QString& path : mRemovals)
    {
        static_cast<void>(mJournal.Remove(path));
    }
    for (const CodeRecoveryEntry& entry : mEntries)
    {
        static_cast<void>(mJournal.Write(entry));
    }
}
} // namespace ludus::editor
