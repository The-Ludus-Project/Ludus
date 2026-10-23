#include "internal/browser_player.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace ludus::editor
{
namespace
{
/// Largest accepted `ludus.web.json` browser player descriptor.
constexpr qint64 MAX_WEB_DESCRIPTOR_BYTES = 1024;
} // namespace

QString BrowserPlayer(const QString& descriptor)
{
    QFile file(QDir(QFileInfo(descriptor).absolutePath()).filePath(QStringLiteral("ludus.web.json")));
    if (file.open(QIODevice::ReadOnly) == false || file.size() > MAX_WEB_DESCRIPTOR_BYTES)
    {
        return {};
    }
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    auto player = object.value(QStringLiteral("player")).toString();
    if (object.value(QStringLiteral("version")).toInt() != 1 ||
        (player != QStringLiteral("cornell-box") && player != QStringLiteral("live-edit-game") &&
         player != QStringLiteral("scripted-game")))
    {
        return {};
    }
    return player;
}
} // namespace ludus::editor
