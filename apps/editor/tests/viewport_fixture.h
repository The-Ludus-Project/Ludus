#pragma once

#include "internal/render_scene.h"
#include "internal/world_document.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/identity/uuid.hpp>

#include <ludus/platform/entropy.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <catch2/catch_test_macros.hpp>

namespace ludus::editor::tests
{
/// Shadowed point lamps in the lit regression scene: six cube faces each gives
/// 72 shadow regions, within the renderer's 80-region atlas.
inline constexpr foundation::int32 LAMP_COUNT = 12;
/// Extra copies of the sample cube. Each caster draws once per shadow region, so
/// five casters need 360 atlas draws: above the RHI's default 256 draws per plan,
/// like the supplied staircase2 scene (6 casters x 78 regions).
inline constexpr foundation::int32 EXTRA_CASTER_COUNT = 4;
/// Spacing between lamps along the row above the World origin, in world units.
inline constexpr foundation::float64 LAMP_SPACING = 1.5;
/// Spacing between the sample cube and its copies along x, in world units. The cube has
/// half-extent 1 and is turned 25 degrees about y, so it spans 2 (cos 25 + sin 25) = 2.66
/// units along x. Closer copies interpenetrate with coplanar tops, where float32 rounding
/// decides which copy, and so which texel, a ray sees; the reference parity test needs
/// every visible surface to be unambiguous.
inline constexpr foundation::float64 CASTER_SPACING = 3;
/// Height of the lamp row above the World origin, in world units.
inline constexpr foundation::float64 LAMP_HEIGHT = 3;
/// Optional mesh flaws and workload for MakeLitScene.
struct LitSceneOptions final
{
    /// Append one zero-area triangle (a repeated vertex) to the sample cube mesh.
    bool HasCollapsedTriangle = false;
    /// Append a cells x cells quad patch below the cube so the bake runs long enough to cancel.
    foundation::uint32 ExtraPatchCells = 0;
    /// Add the authored lamps; without them the World stays on the unlit authored path.
    bool HasLamps = true;
    /// Caller-supplied UUID source; a deterministic source freezes light selection in parity tests.
    /// Its context must remain alive until MakeLitScene returns.
    foundation::identity::EntropySource Entropy = platform::GetSystemEntropySource();
};
/// Reads a test input completely; a missing file fails the current test.
inline QByteArray ReadTestFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}
/// Copies the live-edit sample World into root, adds shadow-casting cube copies and
/// a row of authored point lamps, so the cooked scene takes the authored-lighting path.
inline void MakeLitScene(const QString& root, RenderScene& outScene, const LitSceneOptions& options = {})
{
    REQUIRE(QDir(root).mkpath(QStringLiteral("authoring")) == true);
    for (const QString& name : {QStringLiteral("world.schema.json"),
                                QStringLiteral("rendering.world.json"),
                                QStringLiteral("render.preview.json"),
                                QStringLiteral("cube.mesh.json"),
                                QStringLiteral("ludus.png")})
    {
        QFile target(root + QStringLiteral("/authoring/") + name);
        REQUIRE(target.open(QIODevice::WriteOnly) == true);
        const QByteArray bytes = ReadTestFile(QStringLiteral(":/samples/live-edit-game/authoring/") + name);
        REQUIRE(target.write(bytes) == bytes.size());
    }
    if (options.HasCollapsedTriangle == true || options.ExtraPatchCells != 0)
    {
        const QString meshPath = root + QStringLiteral("/authoring/cube.mesh.json");
        QJsonObject mesh = QJsonDocument::fromJson(ReadTestFile(meshPath)).object();
        QJsonArray positions = mesh.value(QStringLiteral("positions")).toArray();
        QJsonArray uvs = mesh.value(QStringLiteral("uvs")).toArray();
        QJsonArray indices = mesh.value(QStringLiteral("indices")).toArray();
        if (options.HasCollapsedTriangle == true)
        {
            indices.append(0);
            indices.append(0);
            indices.append(1);
        }
        const foundation::uint32 cells = options.ExtraPatchCells;
        const foundation::int32 first = static_cast<foundation::int32>(positions.size());
        for (foundation::uint32 y = 0; cells != 0 && y <= cells; ++y)
        {
            for (foundation::uint32 x = 0; x <= cells; ++x)
            {
                const foundation::float64 u = static_cast<foundation::float64>(x) / cells;
                const foundation::float64 v = static_cast<foundation::float64>(y) / cells;
                positions.append(QJsonArray{u * 4 - 2, -1.5, v * 4 - 2});
                uvs.append(QJsonArray{u, v});
            }
        }
        for (foundation::uint32 y = 0; y < cells; ++y)
        {
            for (foundation::uint32 x = 0; x < cells; ++x)
            {
                const foundation::int32 corner = first + static_cast<foundation::int32>(y * (cells + 1) + x);
                const foundation::int32 next = corner + static_cast<foundation::int32>(cells + 1);
                for (const foundation::int32 vertex : {corner, next, corner + 1, corner + 1, next, next + 1})
                {
                    indices.append(vertex);
                }
            }
        }
        mesh.insert(QStringLiteral("positions"), positions);
        mesh.insert(QStringLiteral("uvs"), uvs);
        mesh.insert(QStringLiteral("indices"), indices);
        QFile target(meshPath);
        REQUIRE(target.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
        REQUIRE(target.write(QJsonDocument(mesh).toJson()) > 0);
    }
    QString error;
    WorldDocument world;
    REQUIRE(world.LoadSchema(ReadTestFile(root + QStringLiteral("/authoring/world.schema.json")), error) == true);
    REQUIRE(world.Load(ReadTestFile(root + QStringLiteral("/authoring/rendering.world.json")), error) == true);
    for (foundation::int32 i = 0; i < EXTRA_CASTER_COUNT; ++i)
    {
        QString casterId;
        REQUIRE(world.DuplicateEntity(options.Entropy, error, QStringLiteral("starter.cube"), casterId) == true);
        QJsonObject components = world.Entity(casterId).value(QStringLiteral("components")).toObject();
        QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
        transform.insert(QStringLiteral("x"),
                         transform.value(QStringLiteral("x")).toDouble() +
                             static_cast<foundation::float64>(i + 1) * CASTER_SPACING);
        components.insert(QStringLiteral("transform"), transform);
        REQUIRE(world.Edit(casterId, components, QStringLiteral("Caster"), error) == true);
    }
    for (foundation::int32 i = 0; i < LAMP_COUNT && options.HasLamps == true; ++i)
    {
        // Lamps carry no renderable, so they add lights without adding meshes.
        QString lampId;
        REQUIRE(world.CreateEntity(options.Entropy, error, QStringLiteral("Lamp"), lampId) == true);
        QJsonObject components = world.Entity(lampId).value(QStringLiteral("components")).toObject();
        QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
        transform.insert(QStringLiteral("x"),
                         (static_cast<foundation::float64>(i) - static_cast<foundation::float64>(LAMP_COUNT) / 2) *
                             LAMP_SPACING);
        transform.insert(QStringLiteral("y"), LAMP_HEIGHT);
        components.insert(QStringLiteral("transform"), transform);
        components.insert(QStringLiteral("light"),
                          QJsonObject{{QStringLiteral("kind"), QStringLiteral("point")},
                                      {QStringLiteral("enabled"), true},
                                      {QStringLiteral("r"), 1},
                                      {QStringLiteral("g"), 1},
                                      {QStringLiteral("b"), 1},
                                      {QStringLiteral("intensity"), 20},
                                      {QStringLiteral("range"), 20},
                                      {QStringLiteral("innerAngle"), 25},
                                      {QStringLiteral("outerAngle"), 30}});
        REQUIRE(world.Edit(lampId, components, QStringLiteral("Lamp"), error) == true);
    }
    const bool isLoaded = RenderScene::Load(root, world, outScene, error);
    INFO(error.toStdString());
    REQUIRE(isLoaded == true);
    REQUIRE(outScene.Lit == options.HasLamps);
    REQUIRE(outScene.AuthoredLights.Count ==
            (options.HasLamps == true ? static_cast<foundation::uint32>(LAMP_COUNT) : 0U));
}
} // namespace ludus::editor::tests
