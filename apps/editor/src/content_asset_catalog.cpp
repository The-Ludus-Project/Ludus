#include "internal/content_browser.h"

#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/luscn.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace ludus::editor
{
namespace
{
using foundation::usize;
namespace format = content::scene_format;
constexpr usize MAX_COOKED_TEXTURE_FILE_BYTES = sizeof(format::TextureRecord) + format::MAX_TEXTURE_BYTES;

[[nodiscard]] std::string_view GetView(const QByteArray& bytes) noexcept
{
    return {bytes.constData(), static_cast<usize>(bytes.size())};
}

[[nodiscard]] content::Status ValidatePreviewArtifact(std::string_view root,
                                                      const QJsonObject& fields,
                                                      const QJsonObject& resources,
                                                      ContentRow& inoutRow,
                                                      QString& outProblem)
{
    const bool isTexture = inoutRow.Type == ContentType::Texture;
    content::Bytes bytes;
    const content::Status status =
        content::ReadFile(root,
                          GetView(inoutRow.Path.toUtf8()),
                          isTexture ? MAX_COOKED_TEXTURE_FILE_BYTES : content::scene_cook::MAX_METADATA_BYTES,
                          bytes);
    const QString identity = QStringLiteral("%1 (%2)").arg(inoutRow.Id, inoutRow.Path);
    if (status != content::Status::Ok)
    {
        outProblem = QStringLiteral("Could not read cooked artifact %1.").arg(identity);
        return status;
    }

    const content::Digest digest = content::Hash(bytes.Data());
    const QByteArray hexDigest =
        QByteArray::fromRawData(reinterpret_cast<const char*>(digest.Data), sizeof(digest.Data)).toHex();
    if (fields.value(QStringLiteral("sha256")).toString().toLatin1() != hexDigest ||
        fields.value(QStringLiteral("bytes")).toInteger(-1) != static_cast<qint64>(bytes.Data().size()))
    {
        outProblem = QStringLiteral("Cooked artifact %1 does not match its catalog digest or size.").arg(identity);
        return content::Status::Invalid;
    }
    if (isTexture == true)
    {
        if (IsValidCookedTexture(bytes.Data()) == true)
        {
            return content::Status::Ok;
        }
        outProblem = QStringLiteral("Cooked texture %1 has an invalid texel record.").arg(identity);
        return content::Status::Invalid;
    }

    const QByteArray view =
        QByteArray::fromRawData(bytes.String().data(), static_cast<qsizetype>(bytes.String().size()));
    const QJsonDocument material = QJsonDocument::fromJson(view);
    if (material.isObject() == false)
    {
        outProblem = QStringLiteral("Cooked material %1 is not a JSON object.").arg(identity);
        return content::Status::Invalid;
    }
    const QJsonObject dependencyKeys = fields.value(QStringLiteral("dependency_keys")).toObject();
    for (const QString& dependency : inoutRow.Dependencies)
    {
        const QString key = resources.value(dependency).toObject().value(QStringLiteral("key")).toString();
        if (key.isEmpty() == true || dependencyKeys.value(dependency).toString() != key)
        {
            outProblem = QStringLiteral("Cooked material %1 has a stale binding for %2.").arg(identity, dependency);
            return content::Status::Invalid;
        }
    }
    const QJsonObject description = material.object();
    const QString textureId = description.value(QStringLiteral("texture")).toString();
    if (textureId.isEmpty() == false &&
        (inoutRow.Dependencies.contains(textureId) == false ||
         resources.value(textureId).toObject().value(QStringLiteral("kind")).toString() != QStringLiteral("texture")))
    {
        outProblem = QStringLiteral("Cooked material %1 has no texture binding for %2.").arg(identity, textureId);
        return content::Status::Invalid;
    }
    inoutRow.Label =
        description.value(QStringLiteral("source_material")).toObject().value(QStringLiteral("name")).toString();
    return content::Status::Ok;
}
} // namespace

// The catalog is replaced as a whole when an import commits. Each preview artifact is opened at its
// catalog path and checked independently before any rows from this candidate are published.
content::Status ReadSceneAssetRows(const QString& root, QVector<ContentRow>& outRows, QString& outProblem)
{
    outProblem.clear();
    const QByteArray encodedRoot = root.toUtf8();
    content::Bytes bytes;
    const content::Status status =
        content::ReadFile(GetView(encodedRoot), COOKED_CATALOG_PATH, content::scene_cook::MAX_CATALOG_BYTES, bytes);
    if (status == content::Status::NotFound)
    {
        outRows.clear();
        return content::Status::Ok;
    }
    if (status != content::Status::Ok)
    {
        outProblem = QStringLiteral("Could not read assets/catalog.json within the scene catalog limit.");
        return status;
    }
    const QByteArray view =
        QByteArray::fromRawData(bytes.String().data(), static_cast<qsizetype>(bytes.String().size()));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(view, &error);
    const QJsonValue resourcesValue = document.object().value(QStringLiteral("resources"));
    if (error.error != QJsonParseError::NoError || resourcesValue.isObject() == false)
    {
        outProblem = QStringLiteral("assets/catalog.json has invalid JSON or is missing its resources object.");
        return content::Status::Invalid;
    }
    const QJsonObject resources = resourcesValue.toObject();
    QVector<ContentRow> rows;
    rows.reserve(resources.size());
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
        const QJsonArray dependencies = fields.value(QStringLiteral("dependencies")).toArray();
        row.Dependencies.reserve(dependencies.size());
        for (qsizetype index = 0; index < dependencies.size(); ++index)
        {
            row.Dependencies.append(dependencies.at(index).toString());
        }
        if (row.Type == ContentType::Texture || row.Type == ContentType::Material)
        {
            const content::Status artifactStatus =
                ValidatePreviewArtifact(GetView(encodedRoot), fields, resources, row, outProblem);
            if (artifactStatus != content::Status::Ok)
            {
                return artifactStatus;
            }
        }
        rows.append(row);
    }
    outRows.swap(rows);
    return content::Status::Ok;
}
} // namespace ludus::editor
