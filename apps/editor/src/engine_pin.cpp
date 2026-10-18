#include "internal/engine_pin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace ludus::editor
{
namespace
{
using foundation::int64;

// Mirrors ludus_tools.identity.MAX_MANIFEST_BYTES.
constexpr int64 MAX_MANIFEST_BYTES = int64{256} * 1024;

// Mirrors ludus_tools.identity.MANIFEST_RELPATH.
QString GetManifestRelativePath()
{
    return QStringLiteral("share/Ludus/LudusSdkManifest.json");
}

// Mirrors ludus_tools.engine_pin.EDITOR_INSTALL_RELPATH.
QString GetEditorInstallRelativePath()
{
    return QStringLiteral("out/install");
}
// Revisions are shown abbreviated like `git log --oneline`.
constexpr qsizetype SHORT_REVISION_LENGTH = 7;

bool IsBuildFolder(const QDir& folder)
{
    // Only a CMake build tree contains these; an installed engine never does.
    return folder.exists(QStringLiteral("CMakeCache.txt")) || folder.exists(QStringLiteral("build.ninja")) ||
           folder.exists(QStringLiteral("CMakeFiles")) || folder.exists(QStringLiteral("cmake/LudusSdkManifest.json"));
}

std::optional<QJsonObject> ReadManifest(const QDir& folder)
{
    QFile file(folder.filePath(GetManifestRelativePath()));
    if (file.open(QIODevice::ReadOnly) == false || file.size() > MAX_MANIFEST_BYTES)
    {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.read(MAX_MANIFEST_BYTES));
    if (document.isObject() == false)
    {
        return std::nullopt;
    }
    return document.object();
}
} // namespace

std::optional<EnginePinState> ParseEnginePinState(const QString& text) noexcept
{
    if (text == QStringLiteral("current"))
    {
        return EnginePinState::Current;
    }
    if (text == QStringLiteral("stale"))
    {
        return EnginePinState::Stale;
    }
    if (text == QStringLiteral("invalid"))
    {
        return EnginePinState::Invalid;
    }
    if (text == QStringLiteral("unknown"))
    {
        return EnginePinState::Unknown;
    }
    return std::nullopt;
}

bool ShouldOfferEditorEngine(const EnginePinStatus& status) noexcept
{
    return status.State == EnginePinState::Stale || status.State == EnginePinState::Invalid;
}

QString GetFlavorForProfile(const QString& profile)
{
    if (profile.endsWith(QStringLiteral("-debug")))
    {
        return QStringLiteral("Debug");
    }
    if (profile.endsWith(QStringLiteral("-development")))
    {
        return QStringLiteral("Development");
    }
    if (profile.endsWith(QStringLiteral("-release")))
    {
        return QStringLiteral("Release");
    }
    return {};
}

EngineInstallCheck CheckEngineInstallFolder(const QString& folder, const QString& expectedFlavor)
{
    EngineInstallCheck check;
    const QFileInfo info(folder);
    if (folder.isEmpty() || info.isAbsolute() == false)
    {
        check.Message = QStringLiteral("Enter the absolute path of an installed engine folder.");
        return check;
    }
    if (info.isDir() == false)
    {
        check.Message = QStringLiteral("This folder does not exist.");
        return check;
    }
    const QDir directory(folder);
    const std::optional<QJsonObject> manifest = ReadManifest(directory);
    if (manifest.has_value() == false)
    {
        check.Message =
            IsBuildFolder(directory)
                ? QStringLiteral("This is a CMake build folder, not an installed engine. Choose an install folder "
                                 "(for example out/install/<profile>-<id>), or choose Build and install the engine "
                                 "used by this editor.")
                : QStringLiteral("This folder is not an installed Ludus engine: %1 is missing or unreadable.")
                      .arg(GetManifestRelativePath());
        return check;
    }
    check.Flavor = manifest->value(QStringLiteral("build_flavor")).toString();
    check.Revision = manifest->value(QStringLiteral("source_revision")).toString();
    if (manifest->value(QStringLiteral("name")).toString() != QStringLiteral("Ludus") || check.Flavor.isEmpty())
    {
        check.Message = QStringLiteral("This folder's engine manifest is incomplete; reinstall the engine.");
        return check;
    }
    if (expectedFlavor.isEmpty() == false && check.Flavor != expectedFlavor)
    {
        check.Message = QStringLiteral("This engine was built for %1, but the project's profile needs %2.")
                            .arg(check.Flavor, expectedFlavor);
        return check;
    }
    check.IsInstall = true;
    check.Message =
        QStringLiteral("Installed Ludus engine · %1 · revision %2")
            .arg(check.Flavor,
                 check.Revision.isEmpty() ? QStringLiteral("unknown") : check.Revision.left(SHORT_REVISION_LENGTH));
    return check;
}

QStringList DiscoverEngineInstalls(const QString& toolingRoot, const QString& expectedFlavor)
{
    QStringList installs;
    if (toolingRoot.isEmpty())
    {
        return installs;
    }
    const QDir root(QDir(toolingRoot).filePath(GetEditorInstallRelativePath()));
    const QStringList names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& name : names)
    {
        const QString candidate = root.absoluteFilePath(name);
        if (CheckEngineInstallFolder(candidate, expectedFlavor).IsInstall)
        {
            installs.append(candidate);
        }
    }
    return installs;
}
} // namespace ludus::editor
