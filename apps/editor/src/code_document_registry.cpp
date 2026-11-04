#include "internal/code_document_registry.h"

#include "internal/code_recovery_writer.h"

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

void CodeDocumentRegistry::onRecoveryDebounce()
{
    flushRecovery(false);
}

void CodeDocumentRegistry::FlushRecoveryNow()
{
    mRecoveryTimer.stop();
    flushRecovery(true);
}

void CodeDocumentRegistry::flushRecovery(bool synchronous)
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
            if (synchronous == false)
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
    if (synchronous == true)
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
} // namespace ludus::editor
