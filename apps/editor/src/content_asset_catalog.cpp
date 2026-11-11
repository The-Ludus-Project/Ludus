#include "internal/content_browser.h"

#include <ludus/content/scene_cook/limits.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace ludus::editor
{
namespace
{
using foundation::usize;

/// Catalog location below the project root, as scene_assets.cpp publishes it.
constexpr QLatin1StringView SCENE_CATALOG_PATH("assets/catalog.json");

/// A material's source name, or an empty string when its description records none or cannot be read.
[[nodiscard]] QString ReadMaterialName(const QString& path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        return {};
    }
    const QByteArray bytes = file.read(static_cast<qint64>(content::scene_cook::MAX_METADATA_BYTES) + 1);
    if (static_cast<usize>(bytes.size()) > content::scene_cook::MAX_METADATA_BYTES)
    {
        return {};
    }
    const QJsonObject material = QJsonDocument::fromJson(bytes).object();
    return material.value(QStringLiteral("source_material")).toObject().value(QStringLiteral("name")).toString();
}
} // namespace

// The catalog is replaced as a whole when an import commits (assets/catalog.json is the commit point, see
// ludus/content/scene_cook/scene_assets.h), so one read observes one complete version without the writer lock.
content::Status ReadSceneAssetRows(const QString& root, QVector<ContentRow>& outRows, QString& outProblem)
{
    outRows.clear();
    outProblem.clear();
    QFile file(QDir(root).filePath(SCENE_CATALOG_PATH));
    if (file.exists() == false)
    {
        return content::Status::Ok;
    }
    if (file.open(QIODevice::ReadOnly) == false)
    {
        outProblem = QStringLiteral("assets/catalog.json cannot be read: %1").arg(file.errorString());
        return content::Status::IoError;
    }
    const QByteArray bytes = file.read(static_cast<qint64>(content::scene_cook::MAX_CATALOG_BYTES) + 1);
    if (static_cast<usize>(bytes.size()) > content::scene_cook::MAX_CATALOG_BYTES)
    {
        outProblem = QStringLiteral("assets/catalog.json is larger than the scene catalog limit.");
        return content::Status::Limit;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    const QJsonObject resources = document.object().value(QStringLiteral("resources")).toObject();
    if (error.error != QJsonParseError::NoError || document.isObject() == false)
    {
        outProblem = QStringLiteral("assets/catalog.json is not valid JSON: %1").arg(error.errorString());
        return content::Status::Invalid;
    }
    outRows.reserve(resources.size());
    for (QJsonObject::const_iterator record = resources.constBegin(); record != resources.constEnd(); ++record)
    {
        const QJsonObject fields = record.value().toObject();
        ContentRow row;
        if (TryParseSceneAssetKind(fields.value(QStringLiteral("kind")).toString(), row.Type) == false)
        {
            continue;
        }
        row.Id = record.key();
        row.Path = fields.value(QStringLiteral("path")).toString();
        row.Key = fields.value(QStringLiteral("key")).toString();
        for (const QJsonValue& dependency : fields.value(QStringLiteral("dependencies")).toArray())
        {
            row.Dependencies.append(dependency.toString());
        }
        if (row.Type == ContentType::Material && row.Path.isEmpty() == false)
        {
            row.Label = ReadMaterialName(QDir(root).filePath(row.Path));
        }
        outRows.append(row);
    }
    return content::Status::Ok;
}
} // namespace ludus::editor
