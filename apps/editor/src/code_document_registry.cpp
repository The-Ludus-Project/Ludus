#include "internal/code_document_registry.h"

#include "internal/code_file_limits.h"
#include "internal/code_recovery_writer.h"
#include "internal/code_text.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <utility>

// One host per canonical path and project epoch (CE02). The registry owns the
// CodeFileDocument lifetime and drops hosts when the epoch changes, so a reload
// invalidates outstanding edits. Loading is CodeFileDocument's own background
// step; the registry only keys, reference-counts and fans out mirroring.

namespace ludus::editor
{
using foundation::int32;
using foundation::uint64;
using foundation::usize;

namespace
{
/// The idle gap after the last edit before a recovery checkpoint is flushed, so
/// a burst of typing coalesces into one background write rather than one per key.
constexpr int32 RECOVERY_DEBOUNCE_MILLISECONDS = 1500;
} // namespace

QString CodeDocumentRegistry::recoveryStateDirectory()
{
    const QString configured = qEnvironmentVariable("LUDUS_EDITOR_RECOVERY_DIRECTORY");
    if (configured.isEmpty() == false)
    {
        return configured;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

CodeDocumentRegistry::CodeDocumentRegistry(QObject* parentOrNull)
    : QObject(parentOrNull), mJournal(recoveryStateDirectory(), QString{})
{
    mRecoveryTimer.setSingleShot(true);
    mRecoveryTimer.setInterval(RECOVERY_DEBOUNCE_MILLISECONDS);
    connect(&mRecoveryTimer, &QTimer::timeout, this, &CodeDocumentRegistry::onRecoveryDebounce);
}

CodeDocumentRegistry::~CodeDocumentRegistry()
{
    // A pending debounced checkpoint is dropped on teardown; the owner flushes a
    // final checkpoint through FlushRecoveryNow before the registry is destroyed.
    mRecoveryTimer.stop();
    if (mWriterOrNull != nullptr)
    {
        mWriterOrNull->wait();
    }
}

QString CodeDocumentRegistry::canonicalKey(const QString& absolutePath)
{
    const QFileInfo info(absolutePath);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() == true ? info.absoluteFilePath() : canonical;
}

void CodeDocumentRegistry::SetProjectEpoch(uint64 epoch)
{
    if (epoch == mEpoch)
    {
        return;
    }
    mEpoch = epoch;
    // Drop every host whose remaining references are zero; a referenced host is
    // left for its open views to close, after which ReleaseHost removes it.
    QHash<QString, HostEntry>::iterator iterator = mHosts.begin();
    while (iterator != mHosts.end())
    {
        if (iterator->ReferenceCount <= 0)
        {
            iterator = mHosts.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

CodeFileDocument*
CodeDocumentRegistry::AcquireHost(const QString& absolutePath, CodeFileLoadStatus& outStatus, QString& outExplanation)
{
    const QString key = canonicalKey(absolutePath);
    QHash<QString, HostEntry>::iterator existing = mHosts.find(key);
    if (existing != mHosts.end())
    {
        ++existing->ReferenceCount;
        outStatus = existing->Host->IsReadOnly() == true ? CodeFileLoadStatus::ReadOnly : CodeFileLoadStatus::Ok;
        outExplanation = existing->Host->GetExplanation();
        return existing->Host.get();
    }
    CodeFileLoadResult result = LoadCodeFile(absolutePath);
    outStatus = result.Status;
    outExplanation = result.Explanation;
    if (result.Status == CodeFileLoadStatus::Failed)
    {
        return nullptr;
    }
    HostEntry entry;
    entry.Host = std::make_shared<CodeFileDocument>(std::move(result), mEpoch);
    entry.ReferenceCount = 1;
    CodeFileDocument* host = entry.Host.get();
    mHosts.insert(key, std::move(entry));
    return host;
}

void CodeDocumentRegistry::ReleaseHost(CodeFileDocument* host)
{
    if (host == nullptr)
    {
        return;
    }
    QHash<QString, HostEntry>::iterator iterator = mHosts.begin();
    while (iterator != mHosts.end())
    {
        if (iterator->Host.get() == host)
        {
            --iterator->ReferenceCount;
            // Keep a dirty host even with no views so its draft survives for the
            // recovery and dirty-close prompts; a clean host is freed at once.
            if (iterator->ReferenceCount <= 0 && iterator->Host->IsDirty() == false)
            {
                mHosts.erase(iterator);
            }
            return;
        }
        ++iterator;
    }
}

int32 CodeDocumentRegistry::GetHostCount() const noexcept
{
    return static_cast<int32>(mHosts.size());
}

int32 CodeDocumentRegistry::GetDirtyHostCount() const
{
    int32 count = 0;
    for (QHash<QString, HostEntry>::const_iterator iterator = mHosts.constBegin(); iterator != mHosts.constEnd();
         ++iterator)
    {
        if (iterator->Host->IsDirty() == true)
        {
            ++count;
        }
    }
    return count;
}

CodeFileDocument* CodeDocumentRegistry::FindHostOrNull(const QString& absolutePath) const
{
    const QString key = canonicalKey(absolutePath);
    QHash<QString, HostEntry>::const_iterator iterator = mHosts.constFind(key);
    return iterator == mHosts.constEnd() ? nullptr : iterator->Host.get();
}

QHash<QString, QByteArray> CodeDocumentRegistry::GetDirtyDrafts() const
{
    QHash<QString, QByteArray> drafts;
    for (QHash<QString, HostEntry>::const_iterator iterator = mHosts.constBegin(); iterator != mHosts.constEnd();
         ++iterator)
    {
        const CodeFileDocument* host = iterator->Host.get();
        if (host->IsDirty() == false)
        {
            continue;
        }
        const CodeDocumentSnapshot snapshot = host->GetSnapshot();
        drafts.insert(host->GetCanonicalPath(),
                      QByteArray(snapshot.Text.GetDataOrNull(), static_cast<qsizetype>(snapshot.Text.GetSize())));
    }
    return drafts;
}

void CodeDocumentRegistry::BindRecoveryProject(const QString& projectRoot)
{
    mRecoveryTimer.stop();
    // A project switch must not leave a flush addressing the old project.
    if (mWriterOrNull != nullptr)
    {
        mWriterOrNull->wait();
    }
    mJournaledPaths.clear();
    mJournal.SetProject(projectRoot);
    mRecoveredBuffers.clear();
    if (projectRoot.isEmpty() == true)
    {
        return;
    }
    const QVector<CodeRecoveryEntry> entries = mJournal.Read();
    mRecoveredBuffers.reserve(entries.size());
    for (const CodeRecoveryEntry& entry : entries)
    {
        // Every entry on disk is journaled: the next checkpoint rewrites the
        // drafts that were restored and removes the ones that were not, so the
        // journal stays intact until restored drafts are durable again.
        mJournaledPaths.insert(entry.CanonicalPath);
        CodeRecoveredBuffer buffer;
        buffer.CanonicalPath = entry.CanonicalPath;
        buffer.SavedHash = entry.SavedHash;
        buffer.Draft = entry.Draft;
        mRecoveredBuffers.append(buffer);
    }
}

QVector<CodeRecoveredBuffer> CodeDocumentRegistry::TakeRecoveredBuffers()
{
    QVector<CodeRecoveredBuffer> buffers;
    buffers.swap(mRecoveredBuffers);
    return buffers;
}

void CodeDocumentRegistry::ScheduleRecoveryCheckpoint()
{
    if (mJournal.GetDirectory().isEmpty() == true)
    {
        return;
    }
    // Restart the idle debounce; a flush follows RECOVERY_DEBOUNCE_MILLISECONDS
    // after the last change, so typing never triggers a per-key write.
    mRecoveryTimer.start();
}

void CodeDocumentRegistry::StartRecoveryCheckpoint()
{
    mRecoveryTimer.stop();
    flushRecovery(false);
}

void CodeDocumentRegistry::DiscardDirtyDrafts()
{
    for (QHash<QString, HostEntry>::iterator iterator = mHosts.begin(); iterator != mHosts.end(); ++iterator)
    {
        CodeFileDocument* host = iterator->Host.get();
        if (host->IsDirty() == true)
        {
            host->DiscardDraft();
        }
    }
}

void CodeDocumentRegistry::onRecoveryDebounce()
{
    flushRecovery(false);
}

void CodeDocumentRegistry::FlushRecoveryNow()
{
    mRecoveryTimer.stop();
    flushRecovery(true);
}

void CodeDocumentRegistry::flushRecovery(bool shouldBlock)
{
    if (mJournal.GetDirectory().isEmpty() == true)
    {
        return;
    }
    // One flush at a time: wait for any in-flight writer so the snapshot it holds
    // is not overtaken by a newer one, keeping each file's last write authoritative.
    if (mWriterOrNull != nullptr)
    {
        if (mWriterOrNull->isFinished() == false)
        {
            if (shouldBlock == false)
            {
                // Retry once the current flush finishes rather than racing it.
                mRecoveryTimer.start();
                return;
            }
            mWriterOrNull->wait();
        }
    }
    // Capture on the UI thread: copy every dirty draft and its baseline hash, and
    // collect the paths that are no longer dirty so their journal file is removed.
    QVector<CodeRecoveryEntry> entries;
    QSet<QString> stillDirty;
    for (QHash<QString, HostEntry>::const_iterator iterator = mHosts.constBegin(); iterator != mHosts.constEnd();
         ++iterator)
    {
        CodeFileDocument* host = iterator->Host.get();
        if (host->IsDirty() == false)
        {
            continue;
        }
        const CodeDocumentSnapshot snapshot = host->GetSnapshot();
        CodeRecoveryEntry entry;
        entry.CanonicalPath = host->GetCanonicalPath();
        entry.SavedHash = host->GetSavedHash();
        entry.Draft = QByteArray(snapshot.Text.GetDataOrNull(), static_cast<qsizetype>(snapshot.Text.GetSize()));
        entries.append(entry);
        stillDirty.insert(entry.CanonicalPath);
    }
    QStringList removals;
    for (const QString& path : mJournaledPaths)
    {
        if (stillDirty.contains(path) == false)
        {
            removals.append(path);
        }
    }
    mJournaledPaths = stillDirty;
    if (entries.isEmpty() == true && removals.isEmpty() == true)
    {
        return;
    }
    if (shouldBlock == true)
    {
        for (const QString& path : removals)
        {
            static_cast<void>(mJournal.Remove(path));
        }
        for (const CodeRecoveryEntry& entry : entries)
        {
            static_cast<void>(mJournal.Write(entry));
        }
        return;
    }
    CodeRecoveryWriter* writer = new CodeRecoveryWriter(mJournal, std::move(entries), std::move(removals), this);
    mWriterOrNull = writer;
    connect(writer, &QThread::finished, this, [this, writer]() {
        if (mWriterOrNull == writer)
        {
            mWriterOrNull = nullptr;
        }
        writer->deleteLater();
    });
    writer->start(QThread::LowPriority);
}

void CodeDocumentRegistry::ClearRecovery()
{
    mRecoveryTimer.stop();
    if (mWriterOrNull != nullptr)
    {
        mWriterOrNull->wait();
    }
    mJournaledPaths.clear();
    mJournal.Clear();
}

namespace
{
/// One file prepared for a workspace edit: the acquired host, whether it was
/// already open before this edit (so it is not released afterwards), the byte
/// edits and the owned inserted bytes those edits borrow.
struct PreparedWorkspaceFile final
{
    CodeFileDocument* HostOrNull = nullptr;
    QList<CodeTextEdit> Edits;
    QList<QByteArray> InsertedBytes;
};

/// Map one file's editor-coordinate replacements to byte edits against its
/// snapshot, in one forward pass. Returns false when a position cannot be mapped
/// to a scalar boundary; on success outPrepared.Edits borrow outPrepared.InsertedBytes.
[[nodiscard]] bool PrepareFileEdits(const CodeWorkspaceFileEdit& fileEdit, PreparedWorkspaceFile& outPrepared)
{
    const CodeDocumentSnapshot snapshot = outPrepared.HostOrNull->GetSnapshot();
    const QByteArray bytes(snapshot.Text.GetDataOrNull(), static_cast<qsizetype>(snapshot.Text.GetSize()));
    const bool hasCrLf = bytes.contains("\r\n");
    outPrepared.Edits.reserve(fileEdit.Replacements.size());
    outPrepared.InsertedBytes.reserve(fileEdit.Replacements.size());
    CodePositionOffset mapped;
    for (const CodeWorkspaceReplacement& replacement : fileEdit.Replacements)
    {
        if (replacement.EndPosition < replacement.StartPosition)
        {
            return false;
        }
        usize startBytes = 0;
        usize endBytes = 0;
        if (AdvanceCodeByteOffset(bytes, replacement.StartPosition, mapped, startBytes) == false ||
            AdvanceCodeByteOffset(bytes, replacement.EndPosition, mapped, endBytes) == false)
        {
            return false;
        }
        QString text = replacement.Replacement;
        if (hasCrLf == true)
        {
            text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        }
        outPrepared.InsertedBytes.append(text.toUtf8());
        const QByteArray& owned = outPrepared.InsertedBytes.last();
        outPrepared.Edits.append(CodeTextEdit
        {
            .StartBytes = startBytes,
            .RemovedBytes = endBytes - startBytes,
            .Inserted = {owned.constData(), static_cast<usize>(owned.size())},
        });
    }
    return true;
}
} // namespace

CodeWorkspaceEditStatus CodeDocumentRegistry::ApplyWorkspaceEdit(const QList<CodeWorkspaceFileEdit>& fileEdits)
{
    if (fileEdits.isEmpty() == true || fileEdits.size() > CODE_MAX_WORKSPACE_EDIT_FILES)
    {
        return CodeWorkspaceEditStatus::Rejected;
    }

    // Acquire and prepare every file, releasing any host we opened if preparation
    // or validation fails, so a rejected edit leaves no file open or changed.
    QList<PreparedWorkspaceFile> prepared;
    prepared.reserve(fileEdits.size());
    // Every successful AcquireHost is balanced by exactly one ReleaseHost. A host
    // that a view already held survives the release (its view keeps a reference);
    // a host opened only for this edit is destroyed when released if it is clean.
    const auto releaseAll = [this, &prepared]() {
        for (const PreparedWorkspaceFile& file : prepared)
        {
            if (file.HostOrNull != nullptr)
            {
                ReleaseHost(file.HostOrNull);
            }
        }
    };

    bool isRejected = false;
    for (const CodeWorkspaceFileEdit& fileEdit : fileEdits)
    {
        PreparedWorkspaceFile file;
        CodeFileLoadStatus status = CodeFileLoadStatus::Failed;
        QString explanation;
        file.HostOrNull = AcquireHost(fileEdit.AbsolutePath, status, explanation);
        if (file.HostOrNull == nullptr)
        {
            isRejected = true;
            break;
        }
        // Record it so releaseAll balances the acquire even on the failure path.
        prepared.append(std::move(file));
        if (prepared.last().HostOrNull->IsReadOnly() == true || PrepareFileEdits(fileEdit, prepared.last()) == false)
        {
            isRejected = true;
            break;
        }
    }

    // Validate every prepared file against its snapshot WITHOUT mutating it, so
    // the whole edit is all-or-nothing across files (design section 7).
    for (int32 index = 0; isRejected == false && index < prepared.size(); ++index)
    {
        const PreparedWorkspaceFile& file = prepared.at(index);
        const CodeDocumentSnapshot snapshot = file.HostOrNull->GetSnapshot();
        const CodeTextSelection beforeSelections[] = {CodeTextSelection{0, 0}};
        const CodeEditSet set =
        {
            .Edits = {file.Edits.constData(), static_cast<usize>(file.Edits.size())},
            .Before = {{beforeSelections, 1}},
            .After = {},
            .Transaction = {},
        };
        QByteArray probe;
        if (ValidateAndApplyEditSet(snapshot.Text, snapshot.LimitBytes, set, probe) != CodeEditStatus::Ok)
        {
            isRejected = true;
        }
    }

    if (isRejected == true)
    {
        releaseAll();
        return CodeWorkspaceEditStatus::Rejected;
    }

    // Every file validated: mint one transaction id and apply each slice, then
    // save. Validation above guarantees each ApplyEdits returns Ok.
    const CodeTransactionId transaction{++mNextTransaction};
    bool didSaveFail = false;
    for (int32 index = 0; index < prepared.size(); ++index)
    {
        PreparedWorkspaceFile& file = prepared[index];
        const CodeDocumentSnapshot snapshot = file.HostOrNull->GetSnapshot();
        const CodeTextSelection beforeSelections[] = {CodeTextSelection{0, 0}};
        const CodeEditSet set =
        {
            .Edits = {file.Edits.constData(), static_cast<usize>(file.Edits.size())},
            .Before = {{beforeSelections, 1}},
            .After = {},
            .Transaction = transaction,
        };
        static_cast<void>(file.HostOrNull->ApplyEdits(snapshot.Identity, set, false));
        QString saveError;
        if (file.HostOrNull->Save(saveError) == false)
        {
            didSaveFail = true;
        }
    }

    // Mirror and record dirty state, then release the one reference each acquire
    // took. A host a view already held keeps that view's reference and stays
    // open; a host opened only for this edit is now clean (saved) and is dropped.
    Q_EMIT DirtyStateChanged();
    ScheduleRecoveryCheckpoint();
    releaseAll();
    return didSaveFail == true ? CodeWorkspaceEditStatus::AppliedButSaveFailed : CodeWorkspaceEditStatus::Ok;
}
} // namespace ludus::editor
