#include "internal/code_document_registry.h"

#include <QFileInfo>

#include <utility>

// One host per canonical path and project epoch (CE02). The registry owns the
// CodeFileDocument lifetime and drops hosts when the epoch changes, so a reload
// invalidates outstanding edits. Loading is CodeFileDocument's own background
// step; the registry only keys, reference-counts and fans out mirroring.

namespace ludus::editor
{
using foundation::int32;
using foundation::uint64;

CodeDocumentRegistry::CodeDocumentRegistry(QObject* parentOrNull) : QObject(parentOrNull) {}

CodeDocumentRegistry::~CodeDocumentRegistry() = default;

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
} // namespace ludus::editor
