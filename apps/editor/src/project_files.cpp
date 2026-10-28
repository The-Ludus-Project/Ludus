#include "internal/project_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>

namespace ludus::editor
{
namespace
{
[[nodiscard]] bool IsLeafName(const QString& name)
{
    return name.isEmpty() == false && name.startsWith(QLatin1Char('.')) == false &&
           name.contains(QLatin1Char('/')) == false && name.contains(QLatin1Char('\\')) == false &&
           name.contains(QLatin1Char(':')) == false && name.contains(QChar::Null) == false;
}

[[nodiscard]] bool IsSafePath(const QString& root, const QFileInfo& file)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QString absolute = file.absoluteFilePath();
    const QString relative = QDir(canonicalRoot).relativeFilePath(absolute);
    if (canonicalRoot.isEmpty() == true || QDir::isAbsolutePath(relative) == true || relative == QStringLiteral("..") ||
        relative.startsWith(QStringLiteral("../")) == true || relative == QStringLiteral(".") ||
        relative.contains(QChar::Null) == true)
    {
        return false;
    }
    QString current = canonicalRoot;
    for (const QString& component : relative.split(QLatin1Char('/')))
    {
        current = QDir(current).filePath(component);
        if (QFileInfo(current).isSymLink() == true)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool CanWriteNewFile(const QString& root, const QString& relative, QString& outError)
{
    const QString path = QDir(root).filePath(relative);
    if (QDir::isAbsolutePath(relative) == true || IsSafePath(root, QFileInfo(path)) == false ||
        QFileInfo(path).exists() == true || QFileInfo(path).isSymLink() == true ||
        QFileInfo(QFileInfo(path).absolutePath()).isDir() == false)
    {
        outError = QStringLiteral("Choose a new file inside an existing project folder, without symbolic links.");
        return false;
    }
    return true;
}

struct CopyFolder final
{
    QString Relative;
    foundation::int32 Depth = 0;
};

struct PackageCopyPaths final
{
    QString Source;
    QString Destination;
};

[[nodiscard]] bool CopyPackage(const PackageCopyPaths& paths, QString& outError)
{
    QList<CopyFolder> pending{{ .Relative = {}, .Depth = 0 }};
    foundation::int64 bytes = 0;
    foundation::int32 entries = 0;
    while (pending.isEmpty() == false)
    {
        const CopyFolder folder = pending.takeFirst();
        const QDir directory(QDir(paths.Source).filePath(folder.Relative));
        if (directory.isReadable() == false)
        {
            outError = QStringLiteral("A package directory is unreadable.");
            return false;
        }
        const QFileInfoList files =
            directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& file : files)
        {
            ++entries;
            if (entries > ProjectFiles::MAX_COPY_ENTRIES || file.isSymLink() == true ||
                (file.isFile() == false && file.isDir() == false))
            {
                outError = QStringLiteral("Package contains symbolic links, special files or too many entries.");
                return false;
            }
            const QString relative = QDir(paths.Source).relativeFilePath(file.filePath());
            const QString target = QDir(paths.Destination).filePath(relative);
            if (file.isDir() == true)
            {
                if (folder.Depth + 1 >= ProjectFiles::MAX_COPY_DEPTH || QDir().mkdir(target) == false)
                {
                    outError = QStringLiteral("Package nesting is too deep or a directory could not be copied.");
                    return false;
                }
                pending.append({ .Relative = relative, .Depth = folder.Depth + 1 });
            }
            else
            {
                if (file.size() > ProjectFiles::MAX_COPY_BYTES - bytes)
                {
                    outError = QStringLiteral("Package exceeds the authored-file copy budget.");
                    return false;
                }
                bytes += file.size();
                if (QFile::copy(file.filePath(), target) == false)
                {
                    outError = QStringLiteral("Cannot copy a package file.");
                    return false;
                }
            }
        }
    }
    return true;
}
} // namespace

bool ProjectFiles::IsProjectFile(const QString& root, const QString& path)
{
    return IsSafePath(root, QFileInfo(path)) == true && QFileInfo(path).isFile() == true;
}

bool ProjectFiles::Create(const QString& root, const QString& relative, const QByteArray& bytes, QString& outError)
{
    if (bytes.size() > MAX_COPY_BYTES)
    {
        outError = QStringLiteral("File exceeds the authored-file creation budget.");
        return false;
    }
    if (CanWriteNewFile(root, relative, outError) == false)
    {
        return false;
    }
    QFile file(QDir(root).filePath(relative));
    if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly) == false)
    {
        outError = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size() || file.flush() == false)
    {
        outError = file.errorString();
        file.close();
        static_cast<void>(file.remove());
        return false;
    }
    file.close();
    return true;
}

bool ProjectFiles::Add(const QString& root, const QString& source, const QString& relative, QString& outError)
{
    if (CanWriteNewFile(root, relative, outError) == false)
    {
        return false;
    }
    const QFileInfo file(source);
    const QString sourceRelative = QDir(QFileInfo(root).canonicalFilePath()).relativeFilePath(file.absoluteFilePath());
    const bool isProjectChild = QDir::isAbsolutePath(sourceRelative) == false &&
                                sourceRelative != QStringLiteral("..") &&
                                sourceRelative.startsWith(QStringLiteral("../")) == false;
    if (isProjectChild == true && IsSafePath(root, QFileInfo(source)) == false)
    {
        outError = QStringLiteral("Project source files cannot be read through child symbolic links.");
        return false;
    }
    if (file.isFile() == false || file.isSymLink() == true || file.size() > MAX_COPY_BYTES)
    {
        outError = QStringLiteral("Source must be a regular file within the authored-file copy budget.");
        return false;
    }
    if (QFile::copy(source, QDir(root).filePath(relative)) == false)
    {
        outError = QStringLiteral("Could not copy the file. The destination may already exist or be unwritable.");
        return false;
    }
    return true;
}

bool ProjectFiles::Duplicate(const QString& root,
                             const QString& source,
                             const QString& name,
                             bool shouldCopyPackage,
                             ProjectFileResult& outResult)
{
    if (IsProjectFile(root, source) == false || IsLeafName(name) == false)
    {
        outResult.Error = QStringLiteral("Select a regular project file and use a non-hidden leaf name for its copy.");
        return false;
    }
    const QString package = QFileInfo(source).absolutePath();
    const bool isPackage = shouldCopyPackage == true && package != QFileInfo(root).canonicalFilePath();
    const QString destination = QDir(isPackage == true ? QFileInfo(package).absolutePath() : package).filePath(name);
    if (CanWriteNewFile(root, QDir(root).relativeFilePath(destination), outResult.Error) == false)
    {
        return false;
    }
    if (isPackage == false)
    {
        if (Add(root, source, QDir(root).relativeFilePath(destination), outResult.Error) == false)
        {
            return false;
        }
        outResult.Path = destination;
        return true;
    }
    QTemporaryDir staging(QDir(root).filePath(QStringLiteral(".ludus-copy-XXXXXX")));
    const QString payload = staging.filePath(QStringLiteral("package"));
    if (staging.isValid() == false || QDir().mkdir(payload) == false ||
        CopyPackage({ .Source = package, .Destination = payload }, outResult.Error) == false ||
        QDir().rename(payload, destination) == false)
    {
        if (outResult.Error.isEmpty() == true)
        {
            outResult.Error = QStringLiteral("Could not publish the copied package. No destination was replaced.");
        }
        return false;
    }
    outResult.Path = QDir(destination).filePath(QFileInfo(source).fileName());
    return true;
}

bool ProjectFiles::Remove(const QString& root, const QString& source, ProjectFileResult& outResult)
{
    if (IsProjectFile(root, source) == false)
    {
        outResult.Error = QStringLiteral("Only regular project files without child symbolic links can be removed.");
        return false;
    }
    const QString trash = QDir(root).filePath(QStringLiteral(".ludus-trash"));
    if (IsSafePath(root, QFileInfo(trash)) == false || QDir().mkpath(trash) == false)
    {
        outResult.Error = QStringLiteral("Cannot create the project's recovery folder.");
        return false;
    }
    const QString folder = QDir(trash).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString destination = QDir(folder).filePath(QDir(root).relativeFilePath(source));
    if (QDir().mkpath(QFileInfo(destination).absolutePath()) == false || QFile::rename(source, destination) == false)
    {
        outResult.Error = QStringLiteral("Cannot move the file to the project's recovery folder.");
        return false;
    }
    outResult.Path = destination;
    return true;
}
} // namespace ludus::editor
