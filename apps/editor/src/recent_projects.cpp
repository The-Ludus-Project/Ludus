#include "internal/recent_projects.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>

namespace ludus::editor
{
namespace
{
constexpr foundation::int64 MAX_HISTORY_BYTES = foundation::int64{256} * 1024;
constexpr foundation::int32 MAX_PATH_BYTES = 4096;
constexpr foundation::int32 MAX_NAME_BYTES = 128;

bool ValidEntry(const QString& path, const QString& name) noexcept
{
    return QDir::isAbsolutePath(path) && path.contains(QChar::Null) == false &&
           path.toUtf8().size() <= MAX_PATH_BYTES && name.isEmpty() == false && name.contains(QChar::Null) == false &&
           name.toUtf8().size() <= MAX_NAME_BYTES;
}
} // namespace

RecentProjectStore::RecentProjectStore(const QString& path) : Path_(path)
{
    if (Path_.isEmpty())
    {
        const QString config = (QDir::isAbsolutePath(qEnvironmentVariable("XDG_CONFIG_HOME"))
                                    ? qEnvironmentVariable("XDG_CONFIG_HOME")
                                    : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
        if (config.isEmpty() == false)
        {
            Path_ = QDir(config).filePath(QStringLiteral("Ludus/Editor/recent-projects.json"));
        }
    }
}

bool RecentProjectStore::Load() noexcept
{
    Entries_.clear();
    if (Path_.isEmpty())
    {
        return false;
    }
    QFile file(Path_);
    if (file.exists() == false)
    {
        return true;
    }
    if (file.open(QIODevice::ReadOnly) == false || file.size() > MAX_HISTORY_BYTES)
    {
        return false;
    }
    const QByteArray bytes = file.read(MAX_HISTORY_BYTES + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > MAX_HISTORY_BYTES)
    {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (document.isArray() == false)
    {
        return false;
    }
    for (const auto& value : document.array())
    {
        const QJsonObject object = value.toObject();
        const QString path = object.value(QStringLiteral("path")).toString();
        const QString name = object.value(QStringLiteral("name")).toString();
        if (ValidEntry(path, name) == false)
        {
            continue;
        }
        const QString cleanPath = QDir::cleanPath(path);
        bool duplicate = false;
        for (const RecentProject& entry : Entries_)
        {
            duplicate = duplicate || entry.DescriptorPath == cleanPath;
        }
        if (duplicate == false)
        {
            Entries_.append({ .DescriptorPath = cleanPath, .Name = name });
        }
        if (Entries_.size() == MaxEntries)
        {
            break;
        }
    }
    return true;
}

bool RecentProjectStore::Remember(RecentProject project) noexcept
{
    // Preserve the opening path (including symlink directories): relative
    // descriptor fields must resolve exactly as they do for an ordinary Open.
    const QString path = QDir::cleanPath(QFileInfo(project.DescriptorPath).absoluteFilePath());
    if (ValidEntry(path, project.Name) == false)
    {
        return false;
    }
    Entries_.removeIf([&path](const RecentProject& entry) { return entry.DescriptorPath == path; });
    project.DescriptorPath = path;
    Entries_.prepend(project);
    while (Entries_.size() > MaxEntries)
    {
        Entries_.removeLast();
    }
    return Save();
}

bool RecentProjectStore::Clear() noexcept
{
    Entries_.clear();
    return Save();
}

bool RecentProjectStore::Save() const noexcept
{
    if (Path_.isEmpty() || QDir().mkpath(QFileInfo(Path_).absolutePath()) == false)
    {
        return false;
    }
    QJsonArray array;
    for (const RecentProject& entry : Entries_)
    {
        QJsonObject object;
        object.insert(QStringLiteral("path"), entry.DescriptorPath);
        object.insert(QStringLiteral("name"), entry.Name);
        array.append(object);
    }
    const QByteArray bytes = QJsonDocument(array).toJson(QJsonDocument::Compact);
    QSaveFile file(Path_);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) == false || file.write(bytes) != bytes.size())
    {
        return false;
    }
    return file.commit();
}

} // namespace ludus::editor
