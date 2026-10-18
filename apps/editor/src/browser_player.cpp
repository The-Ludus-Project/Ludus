#include "internal/browser_player.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace ludus::editor
{
QString BrowserPlayer(const QString& descriptor)
{
    QFile file(QDir(QFileInfo(descriptor).absolutePath()).filePath(QStringLiteral("ludus.web.json")));
    if (file.open(QIODevice::ReadOnly) == false || file.size() > 1024)
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
