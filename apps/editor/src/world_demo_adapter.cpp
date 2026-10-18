#include "internal/world_demo_adapter.h"

#include "internal/editor_identity.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace ludus::editor
{
namespace
{
QJsonObject Transform(const world_demo::Recipe& recipe)
{
    return {{QStringLiteral("x"), recipe.Position.X},
            {QStringLiteral("y"), recipe.Position.Y},
            {QStringLiteral("rotation"), recipe.Rotation},
            {QStringLiteral("scale_x"), recipe.Scale.X},
            {QStringLiteral("scale_y"), recipe.Scale.Y}};
}
QString Text(const world_demo::Name& name)
{
    return QString::fromLatin1(name.Text);
}
} // namespace
bool IsWorldDemo(const WorldDocument& document) noexcept
{
    return document.Schema().value(QStringLiteral("id")) == QJsonValue(QStringLiteral("ludus.world-demo.v1"));
}
bool ProjectWorldDemo(const WorldDocument& document, world_demo::Level& output, QString& error)
{
    const auto bytes = document.Encode();
    const auto result =
        world_demo::ReadWorldLevel({bytes.constData(), static_cast<foundation::usize>(bytes.size())}, output);
    if (result.Error != world_demo::LevelError::None)
    {
        error = QStringLiteral("World demo validation failed (code %1). Each actor needs Transform and one role; "
                               "the World needs one player and one settings entity, valid targets and bounds.")
                    .arg(static_cast<foundation::uint8>(result.Error));
        return false;
    }
    return true;
}
bool ImportWorldDemo(WorldDocument& inoutDocument,
                     const foundation::identity::EntropySource& entropy,
                     const QByteArray& source,
                     QString& outError)
{
    world_demo::Level level;
    const auto result =
        world_demo::ReadLevel({source.constData(), static_cast<foundation::usize>(source.size())}, level);
    if (IsWorldDemo(inoutDocument) == false || result.Error != world_demo::LevelError::None)
    {
        outError = QStringLiteral("Invalid legacy world-demo level; the current World was preserved.");
        return false;
    }
    QJsonArray entities;
    for (foundation::usize i = 0; i < level.EntityCount; ++i)
    {
        const auto& recipe = level.Entities[i];
        QJsonObject role;
        QString kind;
        if (recipe.Type == world_demo::Kind::Exit)
        {
            kind = QStringLiteral("exit");
            role = {{QStringLiteral("half_x"), recipe.HalfExtent.X},
                    {QStringLiteral("half_y"), recipe.HalfExtent.Y},
                    {QStringLiteral("next_level"), Text(recipe.NextLevel)}};
        }
        else
        {
            kind = recipe.Type == world_demo::Kind::Player ? QStringLiteral("player") : QStringLiteral("enemy");
            role = {{QStringLiteral("speed"), recipe.Speed},
                    {QStringLiteral("health"), static_cast<foundation::int32>(recipe.Health)},
                    {QStringLiteral("sprite"), Text(recipe.Sprite)}};
            if (recipe.Type == world_demo::Kind::Enemy)
            {
                role.insert(QStringLiteral("target"), Text(recipe.Target));
            }
        }
        entities.append(QJsonObject{{QStringLiteral("id"), Text(recipe.Id)},
                                    {QStringLiteral("name"), Text(recipe.Id)},
                                    {QStringLiteral("components"),
                                     QJsonObject{{QStringLiteral("transform"), Transform(recipe)}, {kind, role}}}});
    }
    for (foundation::usize i = 0; i < level.CollisionCount; ++i)
    {
        const auto& box = level.Collision[i];
        entities.append(QJsonObject{{QStringLiteral("id"), Text(box.Id)},
                                    {QStringLiteral("name"), Text(box.Id)},
                                    {QStringLiteral("components"),
                                     QJsonObject{{QStringLiteral("collision"),
                                                  QJsonObject{{QStringLiteral("x"), box.Center.X},
                                                              {QStringLiteral("y"), box.Center.Y},
                                                              {QStringLiteral("half_x"), box.HalfExtent.X},
                                                              {QStringLiteral("half_y"), box.HalfExtent.Y}}}}}});
    }
    QString settingsId = QStringLiteral("level-settings");
    bool used = true;
    while (used)
    {
        used = false;
        for (const auto value : entities)
        {
            used = used || value.toObject().value(QStringLiteral("id")) == QJsonValue(settingsId);
        }
        if (used)
        {
            // Keep the legacy `settings-` prefix within the sample's 32-byte name field.
            constexpr qsizetype SETTINGS_UUID_CHARACTERS = 22;
            QString uuid;
            if (TryGenerateUuidText(entropy, uuid) == false)
            {
                outError = DescribeEntropyUnavailable();
                return false;
            }
            settingsId = QStringLiteral("settings-") + uuid.left(SETTINGS_UUID_CHARACTERS);
        }
    }
    entities.append(
        QJsonObject{{QStringLiteral("id"), settingsId},
                    {QStringLiteral("name"), QStringLiteral("Level settings")},
                    {QStringLiteral("components"),
                     QJsonObject{{QStringLiteral("settings"),
                                  QJsonObject{{QStringLiteral("id"), Text(level.Id)},
                                              {QStringLiteral("seed"), QString::number(level.Seed)},
                                              {QStringLiteral("min_x"), level.Minimum.X},
                                              {QStringLiteral("min_y"), level.Minimum.Y},
                                              {QStringLiteral("max_x"), level.Maximum.X},
                                              {QStringLiteral("max_y"), level.Maximum.Y},
                                              {QStringLiteral("camera_x"), level.Camera.X},
                                              {QStringLiteral("camera_y"), level.Camera.Y},
                                              {QStringLiteral("vertical_extent"), level.VerticalExtent}}}}}});
    return inoutDocument.Load(
        QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                  {QStringLiteral("schema"), QStringLiteral("ludus.world-demo.v1")},
                                  {QStringLiteral("entities"), entities}})
            .toJson(),
        outError);
}
bool CommitWorldDemoGesture(WorldDocument& document, const SceneSnapshot& projection, QString& error)
{
    auto candidate = document.Draft();
    auto entities = candidate.value(QStringLiteral("entities")).toArray();
    for (foundation::usize i = 0; i < projection.Level.EntityCount; ++i)
    {
        const auto& recipe = projection.Level.Entities[i];
        for (foundation::int32 row = 0; row < entities.size(); ++row)
        {
            auto entity = entities[row].toObject();
            // A recipe's Id is the entity's label in both World versions, unique within the World.
            if (document.GetEntityLabel(entity) == Text(recipe.Id))
            {
                auto components = entity.value(QStringLiteral("components")).toObject();
                auto transform = components.value(QStringLiteral("transform")).toObject();
                const auto next = Transform(recipe);
                for (auto value = next.begin(); value != next.end(); ++value)
                {
                    if (static_cast<foundation::float32>(transform.value(value.key()).toDouble()) !=
                        static_cast<foundation::float32>(value.value().toDouble()))
                    {
                        transform.insert(value.key(), value.value());
                    }
                }
                components.insert(QStringLiteral("transform"), transform);
                entity.insert(QStringLiteral("components"), components);
                entities[row] = entity;
            }
        }
    }
    candidate.insert(QStringLiteral("entities"), entities);
    return document.Restore(candidate, error);
}
} // namespace ludus::editor
