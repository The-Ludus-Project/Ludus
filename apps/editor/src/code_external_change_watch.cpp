#include "internal/code_external_change_watch.h"

#include <QFileInfo>
#include <QStringList>

// Nominates open code files that may have changed on disk (CE04). The file host
// decides whether they did; see CodeFileDocument::HasExternalChange.

namespace ludus::editor
{
using foundation::int32;

namespace
{
[[nodiscard]] QString directoryOf(const QString& canonicalPath)
{
    return QFileInfo(canonicalPath).absolutePath();
}
} // namespace

CodeExternalChangeWatch::CodeExternalChangeWatch(QObject* parentOrNull) : QObject(parentOrNull)
{
#if !defined(Q_OS_WASM)
    connect(&mWatcher, &QFileSystemWatcher::fileChanged, this, &CodeExternalChangeWatch::onFileChanged);
    connect(&mWatcher, &QFileSystemWatcher::directoryChanged, this, &CodeExternalChangeWatch::onDirectoryChanged);
#endif
}

void CodeExternalChangeWatch::Watch(const QString& canonicalPath)
{
    if (canonicalPath.isEmpty() == true)
    {
        return;
    }
    const int32 count = mReferenceCounts.value(canonicalPath, 0) + 1;
    mReferenceCounts.insert(canonicalPath, count);
    if (count == 1)
    {
        rearm(canonicalPath);
    }
}

void CodeExternalChangeWatch::Unwatch(const QString& canonicalPath)
{
    const int32 count = mReferenceCounts.value(canonicalPath, 0) - 1;
    if (count > 0)
    {
        mReferenceCounts.insert(canonicalPath, count);
        return;
    }
    mReferenceCounts.remove(canonicalPath);
#if !defined(Q_OS_WASM)
    static_cast<void>(mWatcher.removePath(canonicalPath));
    // Keep the directory while another watched file still lives in it.
    const QString directory = directoryOf(canonicalPath);
    for (QHash<QString, int32>::const_iterator iterator = mReferenceCounts.constBegin();
         iterator != mReferenceCounts.constEnd();
         ++iterator)
    {
        if (directoryOf(iterator.key()) == directory)
        {
            return;
        }
    }
    static_cast<void>(mWatcher.removePath(directory));
#endif
}

void CodeExternalChangeWatch::rearm(const QString& canonicalPath)
{
#if !defined(Q_OS_WASM)
    // An atomic replace or a delete drops the file from the watcher; add it back
    // once it exists again. Failures are expected for a missing file.
    if (QFileInfo::exists(canonicalPath) == true && mWatcher.files().contains(canonicalPath) == false)
    {
        static_cast<void>(mWatcher.addPath(canonicalPath));
    }
    const QString directory = directoryOf(canonicalPath);
    if (mWatcher.directories().contains(directory) == false)
    {
        static_cast<void>(mWatcher.addPath(directory));
    }
#else
    static_cast<void>(canonicalPath);
#endif
}

void CodeExternalChangeWatch::onFileChanged(const QString& path)
{
    if (mReferenceCounts.contains(path) == false)
    {
        return;
    }
    rearm(path);
    Q_EMIT FileMayHaveChanged(path);
}

void CodeExternalChangeWatch::onDirectoryChanged(const QString& directory)
{
    // A directory event covers deletes and recreations that the file watch
    // misses. Nominate every watched file in it; the host filters non-changes.
    const QStringList paths = mReferenceCounts.keys();
    for (const QString& path : paths)
    {
        if (directoryOf(path) == directory)
        {
            rearm(path);
            Q_EMIT FileMayHaveChanged(path);
        }
    }
}
} // namespace ludus::editor
