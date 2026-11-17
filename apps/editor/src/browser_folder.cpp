#include "internal/browser_folder.h"

#include <QDir>
#include <QSet>

namespace ludus::editor
{
namespace
{
/// Longest relative path the browser editor admits, in UTF-8 bytes (PATH_MAX on Linux).
constexpr qsizetype MAX_RELATIVE_PATH_BYTES = 4096;
/// Bytes per binary unit step in FormatBrowserByteSize.
constexpr foundation::float64 BYTES_PER_UNIT_STEP = 1024.0;

const QString& GetNextStepForLargeProjects()
{
    static const QString TEXT = QStringLiteral("or open the project in the desktop editor");
    return TEXT;
}

/// A file Ludus::SceneCook can import as a best-effort scene entry; the importer
/// itself is chosen from the file's content.
bool IsSceneEntryName(const QString& name)
{
    for (const QString& extension : {QStringLiteral(".pbrt"),
                                     QStringLiteral(".gltf"),
                                     QStringLiteral(".glb"),
                                     QStringLiteral(".obj"),
                                     QStringLiteral(".luscene.json")})
    {
        if (name.endsWith(extension, Qt::CaseInsensitive) == true)
        {
            return true;
        }
    }
    return false;
}

/// Why a relative path cannot be opened, or an empty string when it can.
QString DescribeInvalidRelativePath(const QString& path)
{
    if (path.toUtf8().size() > MAX_RELATIVE_PATH_BYTES)
    {
        return QStringLiteral("its path is longer than %1 bytes").arg(MAX_RELATIVE_PATH_BYTES);
    }
    if (path.contains(QLatin1Char('\\')) == true || path.contains(QChar(0)) == true)
    {
        return QStringLiteral("its name contains a backslash or a null character");
    }
    for (const QString& part : path.split(QLatin1Char('/')))
    {
        if (part.isEmpty() == true || part == QStringLiteral(".") || part == QStringLiteral(".."))
        {
            return QStringLiteral("its path has an empty, \".\" or \"..\" component");
        }
    }
    return {};
}
} // namespace

QStringList GetGeneratedProjectFolderNames()
{
    return {QStringLiteral("out"),
            QStringLiteral("build"),
            QStringLiteral(".git"),
            QStringLiteral(".ludus"),
            QStringLiteral("node_modules")};
}

bool IsGeneratedProjectPath(const QString& relative)
{
    for (const QString& name : GetGeneratedProjectFolderNames())
    {
        if (relative == name || relative.startsWith(name + QLatin1Char('/')) == true)
        {
            return true;
        }
    }
    return false;
}

QString FormatBrowserByteSize(foundation::uint64 bytes)
{
    const QStringList units{QStringLiteral("bytes"),
                            QStringLiteral("KiB"),
                            QStringLiteral("MiB"),
                            QStringLiteral("GiB"),
                            QStringLiteral("TiB")};
    if (bytes < static_cast<foundation::uint64>(BYTES_PER_UNIT_STEP))
    {
        return QStringLiteral("%1 bytes").arg(bytes);
    }
    foundation::float64 value = static_cast<foundation::float64>(bytes);
    qsizetype unit = 0;
    while (value >= BYTES_PER_UNIT_STEP && unit + 1 < units.size())
    {
        value /= BYTES_PER_UNIT_STEP;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', 1), units[unit]);
}

bool AdmitBrowserFolder(const QString& name,
                        const QList<BrowserFolderEntry>& entries,
                        BrowserFolderIntent intent,
                        const BrowserFolderLimits& limits,
                        QString& outEntry,
                        QString& outProblem)
{
    const QString generated = GetGeneratedProjectFolderNames().join(QStringLiteral(", "));
    const QString folder = name.isEmpty() == true ? QStringLiteral("The chosen folder") : name;
    const QString invalidName = DescribeInvalidRelativePath(name);
    if (name.isEmpty() == true || name.contains(QLatin1Char('/')) == true || invalidName.isEmpty() == false)
    {
        outProblem = QStringLiteral("The browser reported the folder name \"%1\", which cannot name a project folder. "
                                    "Rename the folder, then choose it again.")
                         .arg(name);
        return false;
    }
    if (entries.isEmpty() == true)
    {
        outProblem = QStringLiteral("%1 contains no files outside its generated folders (%2). Choose the folder "
                                    "that contains %3.")
                         .arg(folder,
                              generated,
                              intent == BrowserFolderIntent::Project
                                  ? QStringLiteral("ludus.project.json")
                                  : QStringLiteral("the scene's .pbrt, .gltf, .glb, .obj or .luscene.json file"));
        return false;
    }
    if (entries.size() > limits.MaximumFiles)
    {
        outProblem = QStringLiteral("%1 has %2 files outside its generated folders (%3); the browser editor opens at "
                                    "most %4. Move files the project does not need out of the folder, %5.")
                         .arg(folder,
                              QString::number(entries.size()),
                              generated,
                              QString::number(limits.MaximumFiles),
                              GetNextStepForLargeProjects());
        return false;
    }
    QStringList paths;
    paths.reserve(entries.size());
    for (const BrowserFolderEntry& entry : entries)
    {
        const QString reason = DescribeInvalidRelativePath(entry.Path);
        if (reason.isEmpty() == false)
        {
            outProblem = QStringLiteral("%1/%2 cannot be opened in the browser because %3. Rename it, %4.")
                             .arg(folder, entry.Path, reason, GetNextStepForLargeProjects());
            return false;
        }
        if (entry.Size > limits.MaximumFileBytes)
        {
            outProblem = QStringLiteral("%1/%2 is %3; the browser editor reads files of up to %4 (a quarter of its %5 "
                                        "memory). Move or split the file, %6.")
                             .arg(folder,
                                  entry.Path,
                                  FormatBrowserByteSize(entry.Size),
                                  FormatBrowserByteSize(limits.MaximumFileBytes),
                                  FormatBrowserByteSize(BROWSER_EDITOR_MAXIMUM_MEMORY_BYTES),
                                  GetNextStepForLargeProjects());
            return false;
        }
        paths.append(name + QLatin1Char('/') + entry.Path);
    }
    if (QSet<QString>(paths.begin(), paths.end()).size() != paths.size())
    {
        outProblem = QStringLiteral("The browser listed a file of %1 twice. Choose the folder again.").arg(folder);
        return false;
    }
    QString entry;
    if (intent == BrowserFolderIntent::Project)
    {
        if (paths.contains(name + QStringLiteral("/ludus.project.json")) == false)
        {
            outProblem = QStringLiteral("%1 has no ludus.project.json at its top level. Choose the project's root "
                                        "folder, the one that contains ludus.project.json.")
                             .arg(folder);
            return false;
        }
        entry = name + QStringLiteral("/ludus.project.json");
    }
    else if (ValidateSceneImportPaths(paths, true, entry) == false)
    {
        outProblem = QStringLiteral("%1 contains no .pbrt, .gltf, .glb, .obj or .luscene.json scene file. Choose "
                                    "the folder that holds the scene with its textures and models, or import one "
                                    "scene file or .zip archive.")
                         .arg(folder);
        return false;
    }
    outEntry = entry;
    return true;
}

bool ValidateSceneImportPaths(const QStringList& paths, bool isFolder, QString& outEntry) noexcept
{
    if (paths.isEmpty() == true || paths.size() > MAX_BROWSER_FOLDER_FILES)
    {
        return false;
    }
    QSet<QString> unique;
    QString root;
    QString entry;
    // A .pbrt entry wins; glTF/OBJ files are entries only for scenes without one,
    // because pbrt scenes may ship .obj companions.
    QString otherEntry;
    const auto consider = [&entry, &otherEntry](const QString& name, const QString& path) {
        if (name.endsWith(QStringLiteral(".pbrt"), Qt::CaseInsensitive) == true)
        {
            entry = entry.isEmpty() == true ? path : entry;
        }
        else if (IsSceneEntryName(name) == true && otherEntry.isEmpty() == true)
        {
            otherEntry = path;
        }
    };
    for (const QString& path : paths)
    {
        const QStringList parts = path.split(QLatin1Char('/'));
        if (QDir::isAbsolutePath(path) == true || DescribeInvalidRelativePath(path).isEmpty() == false ||
            unique.contains(path) == true)
        {
            return false;
        }
        if (isFolder == true)
        {
            if (parts.size() < 2 || (root.isEmpty() == false && parts.first() != root))
            {
                return false;
            }
            root = parts.first();
            consider(parts.back(), path);
        }
        else if (parts.size() != 1)
        {
            return false;
        }
        else if (path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) == true)
        {
            if (paths.size() != 1)
            {
                return false; // An archive carries its own companions.
            }
            entry = path;
        }
        else
        {
            consider(path, path);
        }
        unique.insert(path);
    }
    entry = entry.isEmpty() == true ? otherEntry : entry;
    if (entry.isEmpty() == true)
    {
        return false;
    }
    outEntry = entry;
    return true;
}

QString
DescribeBrowserFolderFailure(const BrowserFolderFailure& failure, BrowserFolderIntent intent, qsizetype maximumFiles)
{
    const QString command = intent == BrowserFolderIntent::Project
                                ? QStringLiteral("File → Open Project...")
                                : QStringLiteral("World → Import scene… → Scene folder…");
    const QString folder = failure.Name.isEmpty() == true ? QStringLiteral("the folder") : failure.Name;
    const QString cause = failure.Error.isEmpty() == true ? QString() : QStringLiteral(" (%1)").arg(failure.Error);
    if (failure.Kind == QStringLiteral("activation"))
    {
        return QStringLiteral("The browser showed no folder picker because the request did not follow a click%1. "
                              "Choose %2 again.")
            .arg(cause, command);
    }
    if (failure.Kind == QStringLiteral("permission"))
    {
        return QStringLiteral("The browser did not allow the editor to edit %1%2. Choose File → Reopen Last Project "
                              "Folder again and allow editing when the browser asks, or choose the folder with %3.")
            .arg(folder, cause, command);
    }
    if (failure.Kind == QStringLiteral("no-remembered"))
    {
        return QStringLiteral("This browser has no project folder to reopen. Reopening needs a browser that can edit "
                              "folders on disk (Chrome or Edge) and a folder opened there once with %1.")
            .arg(command);
    }
    if (failure.Kind == QStringLiteral("too-many"))
    {
        return QStringLiteral("%1 has more than %2 files outside its generated folders (%3); the browser editor "
                              "opens at most %2. Move files the project does not need out of the folder, %4.")
            .arg(folder,
                 QString::number(maximumFiles),
                 GetGeneratedProjectFolderNames().join(QStringLiteral(", ")),
                 GetNextStepForLargeProjects());
    }
    if (failure.Kind == QStringLiteral("read"))
    {
        return QStringLiteral("Could not read %1 from %2 on disk%3. It may have been moved, deleted or changed since "
                              "the folder was opened. Restore the file, then open the folder again with %4.")
            .arg(failure.Path, folder, cause, command);
    }
    if (failure.Kind == QStringLiteral("write"))
    {
        return QStringLiteral("Could not save %1 to %2 on disk%3. Your edits are kept in this tab. Check that the "
                              "folder still exists and the browser may still edit it, then save again, or use "
                              "File → Download Project to keep a copy.")
            .arg(failure.Path, folder, cause);
    }
    return QStringLiteral("Could not list %1 in %2%3. Check that the folder still exists and is readable, then "
                          "choose it again with %4.")
        .arg(failure.Path.isEmpty() == true ? QStringLiteral("the files") : failure.Path, folder, cause, command);
}
} // namespace ludus::editor
