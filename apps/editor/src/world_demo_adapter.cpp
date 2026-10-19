#include "internal/world_demo_adapter.h"

#include "internal/editor_identity.h"
#include "internal/render_items.h"
#include "internal/world.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QCryptographicHash>
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
/// Smallest bounding radius in world units, so an empty room still frames sensibly.
constexpr foundation::float32 MIN_WORLD_DEMO_RADIUS = 0.001F;
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
bool LoadWorldDemoScene(const WorldDocument& document, RenderScene& outScene, QString& outError)
{
    world_demo::Level level;
    if (ProjectWorldDemo(document, level, outError) == false)
    {
        return false;
    }
    // The player's default world seed, so scatter matches what Play shows.
    world_demo::GameWorld world;
    world_demo::Status status = world.Prepare(level, world_demo::MAX_WORLD_ENTITIES, world_demo::WorldSeed{});
    while (status == world_demo::Status::Success && world.IsBuilt() == false)
    {
        status = world.BuildNext();
    }
    world_demo::RenderFrame frame;
    if (status != world_demo::Status::Success || world.Extract(0, frame, false, 0) == false)
    {
        outError = QStringLiteral("The World demo could not build its first frame (status %1). Authoring edits "
                                  "were retained.")
                       .arg(static_cast<foundation::uint32>(status));
        return false;
    }
    world_demo::FrameItems items;
    foundation::usize count = 0;
    if (world_demo::TryBuildFrameItems(frame, {}, items, count) != graphics::rhi::RasterStatus::Ready)
    {
        outError = QStringLiteral("The World demo's first frame has a draw the renderer cannot admit.");
        return false;
    }
    RenderScene candidate;
    PreviewMesh quad;
    quad.Id = QStringLiteral("world-demo.unit-quad");
    for (const foundation::math::Vector3& position : world_demo::UNIT_QUAD_POSITIONS)
    {
        quad.Positions.append(position);
        // Top-left UVs over the [-1, 1] quad; nothing samples them, the mesh contract needs one per vertex.
        quad.Uvs.append({(position.X + 1) * 0.5F, (1 - position.Y) * 0.5F});
        quad.Normals.append({0, 0, 1});
    }
    for (const foundation::uint32 index : world_demo::UNIT_QUAD_INDICES)
    {
        quad.Indices.append(index);
    }
    candidate.Meshes.append(quad);
    // Extract emits the floor, collision boxes, scatter, then actors in creation order. An
    // unticked world has every actor alive, so ordering the recipes the same way maps each draw
    // back to its World entity by label. The floor spans the settings entity's bounds.
    if (count < world_demo::ROOM_FLOOR_DRAWS + level.CollisionCount + level.EntityCount)
    {
        outError = QStringLiteral("The World demo's first frame is missing collision or actor draws.");
        return false;
    }
    const foundation::usize scatterEnd = count - level.EntityCount;
    foundation::StaticArray<foundation::usize, world_demo::MAX_WORLD_ENTITIES> actorRecipes{};
    for (foundation::usize i = 0; i < level.EntityCount; ++i)
    {
        actorRecipes[i] = i;
    }
    foundation::Sort(actorRecipes.GetData(),
                     level.EntityCount,
                     [&world, &level](foundation::usize first, foundation::usize second) {
                         const gameplay::world::EntityRegistry& registry = world.Registry();
                         return registry.GetCreationSequence(world.FindAuthored(level.Entities[first].Id.View())) <
                                registry.GetCreationSequence(world.FindAuthored(level.Entities[second].Id.View()));
                     });
    QString settingsKey;
    for (const QJsonValue& value : document.Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        if (entity.value(QStringLiteral("components")).toObject().contains(QStringLiteral("settings")) == true)
        {
            settingsKey = document.GetEntityKey(entity);
        }
    }
    const foundation::usize collisionEnd = world_demo::ROOM_FLOOR_DRAWS + level.CollisionCount;
    foundation::math::Vector3 low{};
    foundation::math::Vector3 high{};
    bool hasBounds = false;
    for (foundation::usize index = 0; index < count; ++index)
    {
        PreviewObject object;
        if (index < world_demo::ROOM_FLOOR_DRAWS)
        {
            object.Id = settingsKey;
            object.Name = QStringLiteral("Room floor");
        }
        else if (index < collisionEnd)
        {
            object.Name = Text(level.Collision[index - world_demo::ROOM_FLOOR_DRAWS].Id);
        }
        else if (index >= scatterEnd)
        {
            object.Name = Text(level.Entities[actorRecipes[index - scatterEnd]].Id);
        }
        if (index >= world_demo::ROOM_FLOOR_DRAWS && object.Name.isEmpty() == false)
        {
            object.Id = document.FindEntityKey(object.Name);
        }
        object.Transform = items[index].Transform;
        object.Tint = items[index].Material.Color;
        for (const foundation::math::Vector3& position : world_demo::UNIT_QUAD_POSITIONS)
        {
            const foundation::math::Vector3 positionWorld =
                foundation::math::TransformPoint(object.Transform, position);
            if (hasBounds == false)
            {
                low = positionWorld;
                high = positionWorld;
                hasBounds = true;
            }
            low = {foundation::math::Min(low.X, positionWorld.X),
                   foundation::math::Min(low.Y, positionWorld.Y),
                   foundation::math::Min(low.Z, positionWorld.Z)};
            high = {foundation::math::Max(high.X, positionWorld.X),
                    foundation::math::Max(high.Y, positionWorld.Y),
                    foundation::math::Max(high.Z, positionWorld.Z)};
        }
        candidate.Objects.append(object);
    }
    PreviewCamera camera;
    camera.Id = QStringLiteral("world-demo.camera");
    camera.Name = QStringLiteral("Game camera");
    camera.Sample = frame.Camera;
    camera.Trace = frame.CameraTrace;
    candidate.Cameras.append(camera);
    candidate.DefaultCamera = 0;
    candidate.Center = (low + high) * 0.5F;
    candidate.Radius = foundation::math::Max(MIN_WORLD_DEMO_RADIUS, foundation::math::Length(high - low) * 0.5F);
    // Extraction is deterministic, so the projected World's bytes identify the scene.
    candidate.AssetsIdentity =
        QCryptographicHash::hash(QByteArrayLiteral("world-demo.unit-quad.v1"), QCryptographicHash::Sha256);
    candidate.Identity =
        QCryptographicHash::hash(document.Encode() + candidate.AssetsIdentity, QCryptographicHash::Sha256);
    candidate.PacketIdentity = candidate.Identity;
    outScene = foundation::Move(candidate);
    outError.clear();
    return true;
}
} // namespace ludus::editor
