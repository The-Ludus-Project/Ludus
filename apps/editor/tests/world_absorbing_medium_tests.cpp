#include "entropy_fixture.h"
#include "internal/builtin_world.h"
#include "internal/controller.h"
#include "internal/render_scene.h"
#include "internal/world_absorbing_medium.h"
#include "internal/world_document.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>

#include <ludus/content/scene_format/material.h>
#include <ludus/graphics/renderer/reference.hpp>

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTest>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace ludus::editor
{
namespace
{
namespace math = foundation::math;
namespace renderer = graphics::renderer;
using foundation::float32;
using foundation::float64;
using foundation::int32;
using foundation::uint32;
using foundation::usize;

constexpr usize TRIANGLE_CORNERS = renderer::REFERENCE_MEDIUM_TRIANGLE_CORNERS;
/// A float32 Euler round trip places corners to within this, in metres.
constexpr float32 PLACEMENT_TOLERANCE_METRES = 1e-5F;

WorldDocument MakeDefaultWorld()
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    return world;
}

QJsonObject GetComponents(const WorldDocument& world, const QString& key)
{
    return world.Entity(key).value(QStringLiteral("components")).toObject();
}

QJsonObject GetMedium(const WorldDocument& world, const QString& key)
{
    return GetComponents(world, key).value(GetAbsorbingMediumComponentId()).toObject();
}

/// Replace one component's fields of an entity through the inspector's edit path.
bool EditComponent(WorldDocument& inoutWorld,
                   // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): entity key, then component id
                   const QString& key,
                   const QString& component,
                   const QJsonObject& changes,
                   QString& outError)
{
    QJsonObject components = GetComponents(inoutWorld, key);
    QJsonObject values = components.value(component).toObject();
    for (QJsonObject::const_iterator change = changes.constBegin(); change != changes.constEnd(); ++change)
    {
        values.insert(change.key(), change.value());
    }
    components.insert(component, values);
    return inoutWorld.Edit(key, components, inoutWorld.Entity(key).value(QStringLiteral("name")).toString(), outError);
}

RenderScene LoadScene(const WorldDocument& world)
{
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    QString error;
    const bool isLoaded = RenderScene::Load(root.path(), world, scene, error, false);
    INFO(error.toStdString());
    REQUIRE(isLoaded == true);
    return scene;
}

uint32 GetBits(float32 value)
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool AreCornersEqual(math::Vector3 a, math::Vector3 b)
{
    return GetBits(a.X) == GetBits(b.X) && GetBits(a.Y) == GetBits(b.Y) && GetBits(a.Z) == GetBits(b.Z);
}

/// Every directed edge appears once and its reverse once, with bit-identical corners, as RT1 requires; and each
/// triangle's right-hand normal points away from centreWorld, out of the convex box.
void CheckClosedOutwardBoundary(const QVector<math::Vector3>& cornersWorld, math::Vector3 centreWorld)
{
    REQUIRE(cornersWorld.size() == static_cast<qsizetype>(ABSORBING_MEDIUM_BOX_TRIANGLES * TRIANGLE_CORNERS));
    for (qsizetype first = 0; first < cornersWorld.size(); first += TRIANGLE_CORNERS)
    {
        const math::Vector3 a = cornersWorld[first];
        const math::Vector3 b = cornersWorld[first + 1];
        const math::Vector3 c = cornersWorld[first + 2];
        const math::Vector3 normal = math::Cross(b - a, c - a);
        CHECK(math::Length(normal) > 0);
        CHECK(math::Dot(normal, (a + b + c) * (1.0F / static_cast<float32>(TRIANGLE_CORNERS)) - centreWorld) > 0);
        for (usize edge = 0; edge < TRIANGLE_CORNERS; ++edge)
        {
            const math::Vector3 from = cornersWorld[first + static_cast<qsizetype>(edge)];
            const math::Vector3 to = cornersWorld[first + static_cast<qsizetype>((edge + 1) % TRIANGLE_CORNERS)];
            int32 forward = 0;
            int32 reverse = 0;
            for (qsizetype other = 0; other < cornersWorld.size(); other += TRIANGLE_CORNERS)
            {
                for (usize otherEdge = 0; otherEdge < TRIANGLE_CORNERS; ++otherEdge)
                {
                    const math::Vector3 otherFrom = cornersWorld[other + static_cast<qsizetype>(otherEdge)];
                    const math::Vector3 otherTo =
                        cornersWorld[other + static_cast<qsizetype>((otherEdge + 1) % TRIANGLE_CORNERS)];
                    if (AreCornersEqual(otherFrom, from) == true && AreCornersEqual(otherTo, to) == true)
                    {
                        ++forward;
                    }
                    if (AreCornersEqual(otherFrom, to) == true && AreCornersEqual(otherTo, from) == true)
                    {
                        ++reverse;
                    }
                }
            }
            CHECK(forward == 1);
            CHECK(reverse == 1);
        }
    }
}

/// Smallest and largest corner of a boundary along each axis.
math::Aabb3 GetBounds(const QVector<math::Vector3>& cornersWorld)
{
    math::Aabb3 bounds{cornersWorld[0], cornersWorld[0]};
    for (const math::Vector3& corner : cornersWorld)
    {
        bounds.Min = {math::Min(bounds.Min.X, corner.X),
                      math::Min(bounds.Min.Y, corner.Y),
                      math::Min(bounds.Min.Z, corner.Z)};
        bounds.Max = {math::Max(bounds.Max.X, corner.X),
                      math::Max(bounds.Max.Y, corner.Y),
                      math::Max(bounds.Max.Z, corner.Z)};
    }
    return bounds;
}
} // namespace

TEST_CASE("Absorbing Medium creates an invisible box that extraction turns into a closed outward boundary",
          "[editor][world][absorbing-medium]")
{
    WorldDocument world = MakeDefaultWorld();
    QString key;
    QString error;
    REQUIRE(CreateWorldObject(world, tests::MakeFixedEntropySource(), QStringLiteral("Absorbing Medium"), key, error) ==
            true);
    const QJsonObject medium = GetMedium(world, key);
    CHECK(medium.value(QStringLiteral("width")).toDouble() == 1);
    CHECK(medium.value(QStringLiteral("absorptionB")).toDouble() == 2);

    // The schema's absorption limit is the engine's, not a copy in the component file.
    for (const QJsonValue property :
         world.Component(GetAbsorbingMediumComponentId()).value(QStringLiteral("properties")).toArray())
    {
        if (property.toObject().value(QStringLiteral("id")).toString().startsWith(QStringLiteral("absorption")))
        {
            CHECK(property.toObject().value(QStringLiteral("max")).toDouble() ==
                  static_cast<float64>(content::scene_format::MAX_MEDIUM_ABSORPTION_PER_WORLD));
        }
    }

    RenderScene scene = LoadScene(world);
    // Raster and render graphs see nothing: no mesh, object or surface.
    CHECK(scene.Objects.isEmpty() == true);
    CHECK(scene.Meshes.size() == LoadScene(MakeDefaultWorld()).Meshes.size());
    REQUIRE(scene.Medium.IsPresent() == true);
    CHECK(scene.Medium.Id == key);
    CHECK(scene.Medium.Name == QStringLiteral("Absorbing Medium"));
    CHECK(scene.Medium.AbsorptionPerWorld == math::Vector3{0.5F, 1, 2});
    CheckClosedOutwardBoundary(scene.Medium.BoundaryCornersWorld, {0, 0, 0});
    const math::Aabb3 unit = GetBounds(scene.Medium.BoundaryCornersWorld);
    CHECK(unit.Min == math::Vector3{-0.5F, -0.5F, -0.5F});
    CHECK(unit.Max == math::Vector3{0.5F, 0.5F, 0.5F});

    // The Transform places, turns and scales the box: 2 × 1 × 1 m scaled 1.5 along X, turned 90° about Y.
    REQUIRE(EditComponent(world,
                          key,
                          QStringLiteral("transform"),
                          {{QStringLiteral("x"), 1}, {QStringLiteral("ry"), 90}, {QStringLiteral("sx"), 1.5}},
                          error) == true);
    REQUIRE(EditComponent(world, key, GetAbsorbingMediumComponentId(), {{QStringLiteral("width"), 2}}, error) == true);
    scene = LoadScene(world);
    CheckClosedOutwardBoundary(scene.Medium.BoundaryCornersWorld, {1, 0, 0});
    const math::Aabb3 turned = GetBounds(scene.Medium.BoundaryCornersWorld);
    // Local X (3 m long after scale) now runs along world Z.
    CHECK(math::Abs(turned.Min.X - 0.5F) < PLACEMENT_TOLERANCE_METRES);
    CHECK(math::Abs(turned.Max.X - 1.5F) < PLACEMENT_TOLERANCE_METRES);
    CHECK(math::Abs(turned.Min.Z + 1.5F) < PLACEMENT_TOLERANCE_METRES);
    CHECK(math::Abs(turned.Max.Z - 1.5F) < PLACEMENT_TOLERANCE_METRES);

    // The boundary is generated by the same rules the renderer validates.
    QVector<math::Vector3> mirrored;
    math::Affine3 mirror;
    mirror.Columns[0] = {-1, 0, 0};
    REQUIRE(BuildAbsorbingMediumBoundary(GetMedium(world, key), mirror, mirrored, error) == true);
    CheckClosedOutwardBoundary(mirrored, {0, 0, 0});
}

TEST_CASE("Absorbing Medium edits are validated where they occur and keep the applied World",
          "[editor][world][absorbing-medium]")
{
    WorldDocument world = MakeDefaultWorld();
    // Distinct UUIDs, so a refused second medium is refused for being a medium.
    tests::SequentialEntropy entropy;
    QString key;
    QString error;
    REQUIRE(CreateWorldObject(world,
                              tests::MakeSequentialEntropySource(entropy),
                              QStringLiteral("Absorbing Medium"),
                              key,
                              error) == true);
    const QJsonObject applied = world.Draft();
    const QString component = GetAbsorbingMediumComponentId();

    CHECK(EditComponent(world, key, component, {{QStringLiteral("absorptionR"), -1}}, error) == false);
    CHECK(world.Draft() == applied);
    const float64 aboveLimit = static_cast<float64>(content::scene_format::MAX_MEDIUM_ABSORPTION_PER_WORLD) * 2;
    CHECK(EditComponent(world, key, component, {{QStringLiteral("absorptionG"), aboveLimit}}, error) == false);
    CHECK(world.Draft() == applied);
    CHECK(EditComponent(world, key, component, {{QStringLiteral("depth"), 0}}, error) == false);
    CHECK(world.Draft() == applied);

    // A box scaled to reach past RT1's coordinate limit is refused with the repair; the edit before it stays.
    REQUIRE(EditComponent(world, key, component, {{QStringLiteral("width"), 10000}}, error) == true);
    const QJsonObject wide = world.Draft();
    CHECK(EditComponent(world, key, QStringLiteral("transform"), {{QStringLiteral("sx"), 1000}}, error) == false);
    CHECK(error.contains(QStringLiteral("Move it toward the origin or make it smaller")) == true);
    CHECK(world.Draft() == wide);
    REQUIRE(world.Undo() == true);
    REQUIRE(world.Draft() == applied);

    // One medium per World: a second preset and a duplicate are refused and name the existing medium.
    QString second;
    CHECK(CreateWorldObject(world,
                            tests::MakeSequentialEntropySource(entropy),
                            QStringLiteral("Absorbing Medium"),
                            second,
                            error) == false);
    CHECK(error.contains(QStringLiteral("second Absorbing Medium")) == true);
    CHECK(world.Draft() == applied);
    CHECK(world.DuplicateEntity(tests::MakeSequentialEntropySource(entropy), error, key, second) == false);
    CHECK(error.contains(QStringLiteral("second Absorbing Medium")) == true);
    CHECK(world.Draft() == applied);

    // The medium is an invisible box of its own.
    CHECK(world.Attach(key, error, QStringLiteral("renderable")) == false);
    CHECK(error.contains(QStringLiteral("Absorbing Medium")) == true);
    CHECK(world.Draft() == applied);

    // Zero is vacuum, which RT1 admits.
    CHECK(EditComponent(world, key, component, {{QStringLiteral("absorptionR"), 0}}, error) == true);
}

TEST_CASE("Absorbing Medium edits undo, redo, save, reopen and remove with the same boundary",
          "[editor][world][absorbing-medium]")
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world = MakeDefaultWorld();
    QString key;
    REQUIRE(CreateWorldObject(world, tests::MakeFixedEntropySource(), QStringLiteral("Absorbing Medium"), key, error) ==
            true);
    const RenderScene created = LoadScene(world);
    REQUIRE(EditComponent(world,
                          key,
                          GetAbsorbingMediumComponentId(),
                          {{QStringLiteral("absorptionR"), 3}, {QStringLiteral("height"), 0.25}},
                          error) == true);
    REQUIRE(EditComponent(world, key, QStringLiteral("transform"), {{QStringLiteral("rz"), 30}}, error) == true);
    const RenderScene edited = LoadScene(world);
    CHECK(edited.Medium.AbsorptionPerWorld.X == 3);
    CHECK(edited.Identity != created.Identity);

    REQUIRE(world.Undo() == true);
    REQUIRE(world.Undo() == true);
    CHECK(LoadScene(world).Medium.BoundaryCornersWorld == created.Medium.BoundaryCornersWorld);
    CHECK(LoadScene(world).Medium.AbsorptionPerWorld == created.Medium.AbsorptionPerWorld);
    REQUIRE(world.Redo() == true);
    REQUIRE(world.Redo() == true);
    CHECK(LoadScene(world).Medium.BoundaryCornersWorld == edited.Medium.BoundaryCornersWorld);

    // Save and reopen: the same authored values give the same boundary, bit for bit.
    WorldDocument reopened;
    REQUIRE(reopened.LoadSchema(schema, error) == true);
    REQUIRE(reopened.Load(world.Encode(), error) == true);
    CHECK(GetMedium(reopened, key) == GetMedium(world, key));
    const RenderScene restored = LoadScene(reopened);
    REQUIRE(restored.Medium.BoundaryCornersWorld.size() == edited.Medium.BoundaryCornersWorld.size());
    for (qsizetype i = 0; i < restored.Medium.BoundaryCornersWorld.size(); ++i)
    {
        CHECK(AreCornersEqual(restored.Medium.BoundaryCornersWorld[i], edited.Medium.BoundaryCornersWorld[i]) == true);
    }
    CHECK(restored.Medium.AbsorptionPerWorld == edited.Medium.AbsorptionPerWorld);

    // Removing the entity removes the medium; Undo restores it.
    REQUIRE(reopened.RemoveEntity(key, error) == true);
    CHECK(LoadScene(reopened).Medium.IsPresent() == false);
    REQUIRE(reopened.Undo() == true);
    CHECK(LoadScene(reopened).Medium.BoundaryCornersWorld == edited.Medium.BoundaryCornersWorld);
}

TEST_CASE("The attenuation readout names each channel's 1/e distance and an explicit vacuum",
          "[editor][world][absorbing-medium]")
{
    float64 distance = 0;
    CHECK(TryComputeAttenuationDistance(0.5, distance) == true);
    CHECK(distance == 2);
    CHECK(TryComputeAttenuationDistance(0, distance) == false);
    CHECK(distance == 2);

    const QString ink = DescribeAbsorbingMedium({0.5, 1, 2}, 1);
    CHECK(ink.contains(QStringLiteral("R 2 m · G 1 m · B 0.5 m")) == true);
    // exp(-0.5), exp(-1) and exp(-2) of the light crossing the 1 m box.
    CHECK(ink.contains(QStringLiteral("R 60.7% · G 36.8% · B 13.5%")) == true);
    const QString tinted = DescribeAbsorbingMedium({0, 0.25, 0}, 0);
    CHECK(tinted.contains(QStringLiteral("R vacuum (no attenuation) · G 4 m · B vacuum (no attenuation)")) == true);
    CHECK(tinted.contains(QStringLiteral("shortest side")) == false);
    CHECK(DescribeAbsorbingMedium({0, 0, 0}, 1).startsWith(QStringLiteral("Vacuum")) == true);
}

TEST_CASE("Properties shows the medium's units and attenuation distance while its fields are typed",
          "[editor][world][absorbing-medium]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    ToolingPaths tooling;
    tooling.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    WorldWorkspace workspace(tooling, nullptr);
    workspace.SetProject(root, 1);
    const QString path = QDir(root).filePath(QStringLiteral("medium.world.json"));
    REQUIRE(workspace.Create(path) == true);
    QAction* createOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateAbsorbingMedium"));
    REQUIRE(createOrNull != nullptr);
    createOrNull->trigger();
    QString key;
    for (const QJsonValue value : workspace.Document().Draft().value(QStringLiteral("entities")).toArray())
    {
        if (value.toObject().value(QStringLiteral("components")).toObject().contains(GetAbsorbingMediumComponentId()))
        {
            key = workspace.Document().GetEntityKey(value.toObject());
        }
    }
    REQUIRE(key.isEmpty() == false);
    REQUIRE(workspace.SelectEntity(key) == true);

    const QLabel* readoutOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldMediumReadout"));
    REQUIRE(readoutOrNull != nullptr);
    CHECK(readoutOrNull->text().contains(QStringLiteral("R 2 m · G 1 m · B 0.5 m")) == true);
    CHECK(workspace.findChild<QLabel*>(QStringLiteral("worldMediumNote")) != nullptr);
    QLineEdit* redOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:absorbingMedium/absorptionR"));
    REQUIRE(redOrNull != nullptr);
    CHECK(redOrNull->accessibleName() == QStringLiteral("Absorption R"));
    CHECK(redOrNull->toolTip().contains(QStringLiteral("per metre")) == true);

    // Typing updates the readout before Apply; zero reads as vacuum.
    redOrNull->setFocus();
    redOrNull->selectAll();
    QTest::keyClicks(redOrNull, QStringLiteral("0"));
    CHECK(readoutOrNull->text().contains(QStringLiteral("R vacuum (no attenuation)")) == true);
    REQUIRE(workspace.Apply() == true);
    CHECK(GetMedium(workspace.Document(), key).value(QStringLiteral("absorptionR")).toDouble() == 0);

    // An out-of-range coefficient is refused in Properties and the applied value stays.
    REQUIRE(workspace.SelectEntity(key) == true);
    QLineEdit* greenOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:absorbingMedium/absorptionG"));
    REQUIRE(greenOrNull != nullptr);
    greenOrNull->setFocus();
    greenOrNull->selectAll();
    QTest::keyClicks(greenOrNull, QStringLiteral("-1"));
    CHECK(workspace.Apply() == false);
    CHECK(GetMedium(workspace.Document(), key).value(QStringLiteral("absorptionG")).toDouble() == 1);
    // Repairing the field applies it.
    greenOrNull->selectAll();
    QTest::keyClicks(greenOrNull, QStringLiteral("1.5"));
    REQUIRE(workspace.Apply() == true);
    CHECK(GetMedium(workspace.Document(), key).value(QStringLiteral("absorptionG")).toDouble() == 1.5);

    // Save and reopen keep the authored values.
    REQUIRE(workspace.Save() == true);
    REQUIRE(workspace.Open(path) == true);
    CHECK(GetMedium(workspace.Document(), key).value(QStringLiteral("absorptionR")).toDouble() == 0);
    CHECK(GetMedium(workspace.Document(), key).value(QStringLiteral("absorptionB")).toDouble() == 2);
    workspace.StopPreview();
}
} // namespace ludus::editor
