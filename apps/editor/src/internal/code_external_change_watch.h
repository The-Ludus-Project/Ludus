#pragma once

#include <ludus/foundation/base/types.h>

#include <QtGlobal>
#if !defined(Q_OS_WASM)
#    include <QFileSystemWatcher>
#endif
#include <QHash>
#include <QObject>
#include <QString>

namespace ludus::editor
{
/// Watches the files behind open code tabs and reports that one may have
/// changed on disk (CE04). It only nominates paths: the owner asks the file
/// host, whose size/time/hash comparison decides whether anything changed, so a
/// spurious or late notification (the editor's own save, macOS FSEvents
/// latency) is harmless. Each file's parent directory is watched as well, so a
/// file that a tool deletes and recreates is picked up again.
///
/// Reference-counted per canonical path: Watch and Unwatch pair once per open
/// view. In the browser editor there is no file watcher; the owner's checks on
/// tab activation and application activation still apply. GUI thread only.
class CodeExternalChangeWatch final : public QObject
{
    Q_OBJECT
public:
    explicit CodeExternalChangeWatch(QObject* parentOrNull = nullptr);

    /// Start (or add a reference to) watching `canonicalPath`.
    void Watch(const QString& canonicalPath);
    /// Drop one reference; the path stops being watched when none remain.
    void Unwatch(const QString& canonicalPath);

Q_SIGNALS:
    /// The file at `canonicalPath`, or its directory, changed on disk.
    void FileMayHaveChanged(const QString& canonicalPath);

private:
    void onFileChanged(const QString& path);
    void onDirectoryChanged(const QString& directory);
    void rearm(const QString& canonicalPath);

#if !defined(Q_OS_WASM)
    QFileSystemWatcher mWatcher;
#endif
    // Open views per watched canonical path.
    QHash<QString, foundation::int32> mReferenceCounts;
};
} // namespace ludus::editor
