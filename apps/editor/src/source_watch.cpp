#include "internal/source_watch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace ludus::editor
{
SourceWatch::SourceWatch(QObject* parent) : QObject(parent)
{
    Debounce_.setSingleShot(true);
    Debounce_.setInterval(450);
    connect(&Watcher_, &QFileSystemWatcher::directoryChanged, this, &SourceWatch::Changed);
    connect(&Watcher_, &QFileSystemWatcher::fileChanged, this, &SourceWatch::Changed);
    connect(&Debounce_, &QTimer::timeout, this, [this]() {
        if (Active() && Pending_ && Busy_ == false)
        {
            Pending_ = false;
            Q_EMIT BuildRequested();
        }
    });
}
bool SourceWatch::Start(const QString& projectDirectory)
{
    Stop();
    Project_ = QFileInfo(projectDirectory).canonicalFilePath();
    if (Project_.isEmpty() == false && Rearm())
    {
        return true;
    }
    Stop();
    return false;
}
void SourceWatch::Stop()
{
    Debounce_.stop();
    if (Watcher_.files().isEmpty() == false)
    {
        Watcher_.removePaths(Watcher_.files());
    }
    if (Watcher_.directories().isEmpty() == false)
    {
        Watcher_.removePaths(Watcher_.directories());
    }
    Project_.clear();
    Pending_ = false;
}
void SourceWatch::SetBusy(bool busy)
{
    Busy_ = busy;
    if (Busy_ == false && Pending_ && Active() && Debounce_.isActive() == false)
    {
        Debounce_.start();
    }
}
bool SourceWatch::Rearm()
{
    QFile sidecar(QDir(Project_).filePath(QStringLiteral("ludus.play.json")));
    if (sidecar.open(QIODevice::ReadOnly) == false || sidecar.size() > 262144)
    {
        return false;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(sidecar.readAll(), &error);
    if (error.error != QJsonParseError::NoError || document.isObject() == false)
    {
        return false;
    }
    auto roots = document.object().value(QStringLiteral("watch_roots")).toArray();
    if (roots.isEmpty())
    {
        roots.append(QStringLiteral("."));
    }
    if (roots.size() > 16)
    {
        return false;
    }
    QStringList paths{Project_};
    const QDir project(Project_);
    for (const auto& root : roots)
    {
        const auto relative = root.toString();
        const QFileInfo info(project.filePath(relative));
        const auto path = info.canonicalFilePath();
        if (root.isString() == false || relative.isEmpty() || QDir::isAbsolutePath(relative) || info.isSymLink() ||
            path.isEmpty() || (path != Project_ && path.startsWith(Project_ + QDir::separator()) == false))
        {
            return false;
        }
        paths.append(path);
        if (info.isDir() == false)
        {
            continue;
        }
        // Traverse explicitly so build stores and symlinks never enter watches.
        QStringList folders{path};
        for (int index = 0; index < folders.size(); ++index)
        {
            const auto entries = QDir(folders.at(index)).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
            for (const auto& entry : entries)
            {
                const auto name = entry.fileName();
                if (name == QStringLiteral("out") || name == QStringLiteral(".git") ||
                    name == QStringLiteral(".ludus") || name == QStringLiteral("node_modules"))
                {
                    continue;
                }
                if (entry.isSymLink() || (entry.isDir() == false && entry.isFile() == false))
                {
                    return false;
                }
                paths.append(entry.absoluteFilePath());
                if (entry.isDir())
                {
                    folders.append(entry.absoluteFilePath());
                }
                if (paths.size() > 4096)
                {
                    return false;
                }
            }
        }
    }
    for (const auto& name : {QStringLiteral("ludus.project.json"),
                             QStringLiteral("ludus.play.json"),
                             QStringLiteral("ludus.lock.json"),
                             QStringLiteral("CMakeLists.txt"),
                             QStringLiteral("CMakePresets.json"),
                             QStringLiteral("CMakeUserPresets.json")})
    {
        const auto path = project.filePath(name);
        if (QFileInfo::exists(path))
        {
            paths.append(path);
        }
    }
    paths.removeDuplicates();
    const auto current = Watcher_.files() + Watcher_.directories();
    QStringList removals;
    for (const auto& path : current)
    {
        if (paths.contains(path) == false)
        {
            removals.append(path);
        }
    }
    if (removals.isEmpty() == false)
    {
        Watcher_.removePaths(removals);
    }
    QStringList additions;
    for (const auto& path : paths)
    {
        if (current.contains(path) == false)
        {
            additions.append(path);
        }
    }
    return additions.isEmpty() || Watcher_.addPaths(additions).isEmpty();
}
void SourceWatch::Changed()
{
    if (Active() == false)
    {
        return;
    }
    if (Rearm() == false)
    {
        Stop();
        Q_EMIT Failed(QStringLiteral("Source watch unavailable or over its 4096-path bound; use manual reload"));
        return;
    }
    Pending_ = true;
    Debounce_.start();
}
} // namespace ludus::editor
