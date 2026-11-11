#include "entropy_fixture.h"
#include "internal/builtin_world.h"
#include "internal/controller.h"
#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_object_overlay.h"
#include "internal/world_document.h"
#include "internal/world_terrain.h"
#include "internal/world_workspace.h"
#include "scene_fixture.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRectF>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ludus::editor
{
namespace
{
QJsonObject GetDefaultRecipe(const WorldDocument& world)
{
    QJsonObject recipe;
    for (const QJsonValue value :
         world.Component(GetTerrainComponentId()).value(QStringLiteral("properties")).toArray())
    {
        const QJsonObject property = value.toObject();
        recipe.insert(property.value(QStringLiteral("id")).toString(), property.value(QStringLiteral("default")));
    }
    return recipe;
}
constexpr foundation::int32 TERRAIN_WAIT_MILLISECONDS = 15000;

QJsonObject GetTerrainRecipe(const WorldDocument& world, const QString& key)
{
    return world.Entity(key).value(QStringLiteral("components")).toObject().value(GetTerrainComponentId()).toObject();
}
} // namespace

TEST_CASE("Terrain extraction is seeded, resolution independent and safe at the admitted grid limit",
          "[editor][world][terrain]")
{
    QByteArray schema;
    QString error;
    INFO(error.toStdString());
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    QJsonObject recipe = GetDefaultRecipe(world);
    QString key;
    REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), {}, recipe, key, error) == true);
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene original;
    REQUIRE(RenderScene::Load(root.path(), world, original, error, false) == true);
    REQUIRE(original.Objects.size() == 1);
    const PreviewMesh& originalMesh = original.Meshes[original.Objects[0].Mesh];
    const foundation::uint32 samples = static_cast<foundation::uint32>(recipe.value(QStringLiteral("samples")).toInt());
    CHECK(originalMesh.Positions.size() == samples * samples);
    CHECK(originalMesh.Normals.size() == originalMesh.Positions.size());
    CHECK(originalMesh.Indices.size() == 6 * (samples - 1) * (samples - 1));
    CHECK(originalMesh.Positions.first().X == -16);
    CHECK(originalMesh.Positions.last().Z == 16);
    for (const foundation::math::Vector3 normal : originalMesh.Normals)
    {
        CHECK(normal.Y > 0);
    }

    recipe.insert(QStringLiteral("samples"), 17);
    PreviewMesh coarse;
    REQUIRE(BuildTerrainMesh(recipe, coarse, error) == true);
    REQUIRE(coarse.Positions.size() == 17 * 17);
    for (foundation::uint32 z = 0; z < 17; ++z)
    {
        for (foundation::uint32 x = 0; x < 17; ++x)
        {
            CHECK(coarse.Positions[z * 17 + x] == originalMesh.Positions[2 * z * samples + 2 * x]);
        }
    }
    recipe.insert(QStringLiteral("seed"), 9);
    PreviewMesh reseeded;
    REQUIRE(BuildTerrainMesh(recipe, reseeded, error) == true);
    CHECK(reseeded.Positions != coarse.Positions);

    recipe.insert(QStringLiteral("samples"), 64);
    REQUIRE(BuildTerrainMesh(recipe, reseeded, error) == true);
    CHECK(reseeded.Positions.size() == 64 * 64);
    recipe.insert(QStringLiteral("samples"), 65);
    REQUIRE(BuildTerrainMesh(recipe, reseeded, error) == false);
    CHECK(reseeded.Positions.size() == 64 * 64);

    const QJsonObject applied = world.Draft();
    recipe.insert(QStringLiteral("minimumElevation"), 12);
    CHECK(EditWorldTerrain(world, tests::MakeFixedEntropySource(), key, recipe, key, error) == false);
    CHECK(world.Draft() == applied);
}

TEST_CASE("Explore as Player uses explicit Static Colliders after World extraction",
          "[editor][world][terrain][first-person]")
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    QJsonObject recipe = GetDefaultRecipe(world);
    recipe.insert(QStringLiteral("shape"), QStringLiteral("Flat"));
    recipe.insert(QStringLiteral("minimumElevation"), 2);
    tests::SequentialEntropy sequence;
    const foundation::identity::EntropySource entropy = tests::MakeSequentialEntropySource(sequence);
    QString key;
    REQUIRE(EditWorldTerrain(world, entropy, {}, recipe, key, error) == true);
    QString cubeId;
    REQUIRE(CreateWorldObject(world, entropy, QStringLiteral("Cube"), cubeId, error) == true);
    QJsonObject cubeComponents = world.Entity(cubeId).value(QStringLiteral("components")).toObject();
    QJsonObject cubeTransform = cubeComponents.value(QStringLiteral("transform")).toObject();
    cubeTransform.insert(QStringLiteral("x"), 1);
    cubeTransform.insert(QStringLiteral("y"), 3);
    cubeComponents.insert(QStringLiteral("transform"), cubeTransform);
    REQUIRE(world.Edit(cubeId, cubeComponents, QStringLiteral("Cube"), error) == true);
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    REQUIRE(RenderScene::Load(root.path(), world, scene, error, false) == true);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    gameplay::camera::CameraSample spawn;
    spawn.Pose.Position = {0, 4, 0};
    spawn.Lens.Projection = gameplay::camera::CameraProjection::PerspectiveInfinite;
    spawn.Lens.NearPlane = 0.01F;
    CHECK(preview.BeginFirstPerson(spawn, error) == false);
    CHECK(error.contains(QStringLiteral("Add Static Collider")) == true);
    REQUIRE(world.Attach(key, error, GetStaticColliderComponentId()) == true);
    REQUIRE(RenderScene::Load(root.path(), world, scene, error, false) == true);
    REQUIRE(scene.Objects.size() == 2);
    REQUIRE(preview.SetScene(scene) == true);
    const bool isStarted = preview.BeginFirstPerson(spawn, error);
    INFO(error.toStdString());
    REQUIRE(isStarted == true);
    for (foundation::uint32 frame = 0; frame < 10; ++frame)
    {
        preview.AdvanceFirstPerson({1, 0, 0}, 0.1F, 3, false);
    }
    const foundation::math::Vector3d eyeWorld = preview.GetVisibleCameraSample().Pose.Position;
    CHECK(eyeWorld.X == Catch::Approx(3));
    CHECK(eyeWorld.Y == Catch::Approx(3.602).margin(0.003));

    REQUIRE(world.Attach(cubeId, error, GetStaticColliderComponentId()) == true);
    REQUIRE(RenderScene::Load(root.path(), world, scene, error, false) == true);
    REQUIRE(preview.SetScene(scene) == true);
    CHECK(preview.IsExploringFirstPerson() == false);
    REQUIRE(preview.BeginFirstPerson(spawn, error) == true);
    preview.AdvanceFirstPerson({1, 0, 0}, 0.1F, 3, false);
    CHECK(preview.GetVisibleCameraSample().Pose.Position.X < 0.3);
}

TEST_CASE("A grayscale heightmap keeps its sample orientation and persists without the PNG", "[editor][world][terrain]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString path = directory.filePath(QStringLiteral("heightmap.png"));
    QImage image(3, 2, QImage::Format_Grayscale16);
    foundation::uint16* top = reinterpret_cast<foundation::uint16*>(image.scanLine(0));
    foundation::uint16* bottom = reinterpret_cast<foundation::uint16*>(image.scanLine(1));
    top[0] = 0;
    top[1] = 32768;
    top[2] = 65535;
    bottom[0] = 65535;
    bottom[1] = 16384;
    bottom[2] = 0;
    REQUIRE(image.save(path) == true);

    QJsonObject heightmap;
    QString error;
    REQUIRE(ImportTerrainHeightmap(path, heightmap, error) == true);
    const QJsonArray rows = heightmap.value(QStringLiteral("rows")).toArray();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].toArray() == QJsonArray{0, 32768, 65535});
    CHECK(rows[1].toArray() == QJsonArray{65535, 16384, 0});
    QImage oversized(65, 2, QImage::Format_Grayscale8);
    oversized.fill(0);
    const QString invalid = directory.filePath(QStringLiteral("oversized.png"));
    REQUIRE(oversized.save(invalid) == true);
    const QJsonObject retained = heightmap;
    CHECK(ImportTerrainHeightmap(invalid, heightmap, error) == false);
    CHECK(heightmap == retained);
    QImage colour(2, 2, QImage::Format_RGB32);
    colour.fill(Qt::red);
    REQUIRE(colour.save(invalid) == true);
    CHECK(ImportTerrainHeightmap(invalid, heightmap, error) == false);
    CHECK(heightmap == retained);

    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    QJsonObject recipe = GetDefaultRecipe(world);
    recipe.insert(QStringLiteral("shape"), QStringLiteral("Heightmap"));
    recipe.insert(QStringLiteral("width"), 4);
    recipe.insert(QStringLiteral("depth"), 2);
    recipe.insert(QStringLiteral("minimumElevation"), -2);
    recipe.insert(QStringLiteral("maximumElevation"), 6);
    recipe.insert(QStringLiteral("heightmap"), heightmap);
    SetTerrainRecipeVersion(recipe);

    PreviewMesh mesh;
    REQUIRE(BuildTerrainMesh(recipe, mesh, error) == true);
    REQUIRE(mesh.Positions.size() == 6);
    CHECK(mesh.Indices.size() == 12);
    CHECK(mesh.Positions[0] == foundation::math::Vector3{-2, -2, -1});
    CHECK(mesh.Positions[2] == foundation::math::Vector3{2, 6, -1});
    CHECK(mesh.Positions[3] == foundation::math::Vector3{-2, 6, 1});
    CHECK(mesh.Positions[5] == foundation::math::Vector3{2, -2, 1});
    heightmap.insert(QStringLiteral("flipX"), true);
    recipe.insert(QStringLiteral("heightmap"), heightmap);
    REQUIRE(BuildTerrainMesh(recipe, mesh, error) == true);
    CHECK(mesh.Positions[0] == foundation::math::Vector3{-2, 6, -1});
    CHECK(mesh.Positions[5] == foundation::math::Vector3{2, 6, 1});
    heightmap.insert(QStringLiteral("flipZ"), true);
    recipe.insert(QStringLiteral("heightmap"), heightmap);
    REQUIRE(BuildTerrainMesh(recipe, mesh, error) == true);
    CHECK(mesh.Positions[1].Y < 1);
    CHECK(mesh.Positions[4].Y > 1);

    QString key;
    REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), {}, recipe, key, error) == true);
    REQUIRE(QFile::remove(path) == true);
    WorldDocument reopened;
    REQUIRE(reopened.LoadSchema(schema, error) == true);
    REQUIRE(reopened.Load(world.Encode(), error) == true);
    CHECK(GetTerrainRecipe(reopened, key) == recipe);
    RenderScene scene;
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error, false) == true);
    REQUIRE(scene.Objects.size() == 1);
    CHECK(scene.Meshes[scene.Objects[0].Mesh].Positions == mesh.Positions);
}

TEST_CASE("Flatten strokes retain their local target when the procedural base regenerates", "[editor][world][terrain]")
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    QJsonObject recipe = GetDefaultRecipe(world);
    QString key;
    REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), {}, recipe, key, error) == true);
    PreviewMesh original;
    REQUIRE(BuildTerrainMesh(recipe, original, error) == true);

    const QJsonArray points{QJsonArray{-3, 0}, QJsonArray{3, 0}};
    REQUIRE(AppendTerrainStroke(recipe, points, 4, TerrainBrush::Flatten, 2, error) == true);
    REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), key, recipe, key, error) == true);
    PreviewMesh sculpted;
    REQUIRE(BuildTerrainMesh(recipe, sculpted, error) == true);
    const qsizetype center = sculpted.Positions.size() / 2;
    CHECK(sculpted.Positions[center].Y == 2);
    CHECK(sculpted.Positions.first() == original.Positions.first());
    CHECK(sculpted.Normals != original.Normals);
    const QJsonObject sculptedWorld = world.Draft();
    REQUIRE(world.Undo() == true);
    CHECK(GetTerrainRecipe(world, key).contains(QStringLiteral("flattenStrokes")) == false);
    REQUIRE(world.Redo() == true);
    CHECK(world.Draft() == sculptedWorld);

    recipe.insert(QStringLiteral("seed"), 42);
    REQUIRE(BuildTerrainMesh(recipe, sculpted, error) == true);
    CHECK(sculpted.Positions[center].Y == 2);
    const qsizetype samples = recipe.value(QStringLiteral("samples")).toInt();
    CHECK(sculpted.Positions.mid(0, samples) != original.Positions.mid(0, samples));
    QJsonObject cropped = recipe;
    cropped.insert(QStringLiteral("width"), 8);
    CHECK(RemoveClippedTerrainStrokes(recipe, cropped) == 1);
    CHECK(cropped.contains(QStringLiteral("flattenStrokes")) == false);
    CHECK(cropped.value(QStringLiteral("version")) == 1);
    const QJsonObject unchanged = recipe;
    CHECK(AppendTerrainStroke(recipe, QJsonArray{QJsonValue(QJsonArray{100, 0})}, 4, TerrainBrush::Flatten, 2, error) ==
          false);
    CHECK(recipe == unchanged);
}

TEST_CASE("Mixed terrain strokes replay in order against the regenerated source", "[editor][world][terrain]")
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    QJsonObject recipe = GetDefaultRecipe(world);
    recipe.insert(QStringLiteral("shape"), QStringLiteral("Flat"));
    recipe.insert(QStringLiteral("width"), 4);
    recipe.insert(QStringLiteral("depth"), 4);
    recipe.insert(QStringLiteral("samples"), 5);

    const QJsonArray centerPoint{QJsonValue(QJsonArray{0, 0})};
    REQUIRE(centerPoint.size() == 1);
    REQUIRE(centerPoint.first().isArray() == true);
    INFO(error.toStdString());
    REQUIRE(AppendTerrainStroke(recipe, centerPoint, 1, TerrainBrush::Flatten, 2, error) == true);
    const QJsonArray flattenStrokes = recipe.value(QStringLiteral("flattenStrokes")).toArray();
    REQUIRE(flattenStrokes.size() == 1);
    REQUIRE(AppendTerrainStroke(recipe, centerPoint, 1, TerrainBrush::RaiseLower, 3, error) == true);
    PreviewMesh mesh;
    REQUIRE(BuildTerrainMesh(recipe, mesh, error) == true);
    const qsizetype center = mesh.Positions.size() / 2;
    CHECK(mesh.Positions[center].Y == 5);
    REQUIRE(AppendTerrainStroke(recipe, centerPoint, 1, TerrainBrush::RaiseLower, -1, error) == true);
    REQUIRE(AppendTerrainStroke(recipe, centerPoint, 2, TerrainBrush::Smooth, 1, error) == true);
    CHECK(recipe.value(QStringLiteral("version")) == 3);
    CHECK(recipe.contains(QStringLiteral("flattenStrokes")) == false);
    const QJsonArray mixedStrokes = recipe.value(QStringLiteral("sculptStrokes")).toArray();
    REQUIRE(mixedStrokes.size() == 4);
    CHECK(mixedStrokes.first() == flattenStrokes.first());
    REQUIRE(BuildTerrainMesh(recipe, mesh, error) == true);
    CHECK(mesh.Positions[center].Y == 0);
    CHECK(mesh.Positions[center - 1].Y == 1);
    CHECK(mesh.Positions[center + 1].Y == 1);
    CHECK(mesh.Positions.first().Y == 0);

    const QJsonObject previous = recipe;
    CHECK(AppendTerrainStroke(recipe, centerPoint, 1, TerrainBrush::RaiseLower, 0, error) == false);
    CHECK(AppendTerrainStroke(recipe, centerPoint, 1, TerrainBrush::Smooth, 2, error) == false);
    CHECK(recipe == previous);
    QJsonObject invalid = recipe;
    invalid.insert(QStringLiteral("version"), 2);
    CHECK(ValidateTerrainRecipe(invalid, error) == false);
    invalid = recipe;
    invalid.insert(QStringLiteral("flattenStrokes"), flattenStrokes);
    CHECK(ValidateTerrainRecipe(invalid, error) == false);

    recipe.insert(QStringLiteral("minimumElevation"), 10);
    recipe.insert(QStringLiteral("maximumElevation"), 10);
    QString key;
    REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), {}, recipe, key, error) == true);
    WorldDocument reopened;
    REQUIRE(reopened.LoadSchema(schema, error) == true);
    REQUIRE(reopened.Load(world.Encode(), error) == true);
    const QJsonObject regenerated = GetTerrainRecipe(reopened, key);
    CHECK(regenerated.value(QStringLiteral("sculptStrokes")).toArray() == mixedStrokes);
    REQUIRE(BuildTerrainMesh(regenerated, mesh, error) == true);
    CHECK(mesh.Positions[center].Y == 10);
    CHECK(mesh.Positions[center - 1].Y == 8.5F);
    CHECK(mesh.Positions[center + 1].Y == 8.5F);
    CHECK(mesh.Positions.first().Y == 10);

    QJsonObject cropped = regenerated;
    cropped.insert(QStringLiteral("width"), 2);
    CHECK(RemoveClippedTerrainStrokes(regenerated, cropped) == 1);
    CHECK(cropped.value(QStringLiteral("sculptStrokes")).toArray().size() == 3);
}

TEST_CASE("Terrain dock adds and edits the exact ready recipe with Properties hidden", "[editor][world][terrain]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString path = QDir(root).filePath(QStringLiteral("terrain.world.json"));
    WorldWorkspace workspace;
    workspace.SetEntropySource(tests::MakeFixedEntropySource());
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    QWidget properties;
    workspace.TakePropertiesPanel(&properties)->hide();
    workspace.show();
    const QJsonObject empty = workspace.Document().Draft();

    RenderPanel* renderOrNull = workspace.findChild<RenderPanel*>();
    QWidget* panelOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldTerrainPanel"));
    REQUIRE(renderOrNull != nullptr);
    REQUIRE(panelOrNull != nullptr);
    PreviewViewport& viewport = renderOrNull->GetViewport();
    QPushButton* commitOrNull = panelOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainCommit"));
    QPushButton* cancelOrNull = panelOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainCancel"));
    QTabBar* modeOrNull = panelOrNull->findChild<QTabBar*>(QStringLiteral("worldTerrainMode"));
    QLineEdit* reliefOrNull = panelOrNull->findChild<QLineEdit*>(QStringLiteral("worldTerrainField:relief"));
    QLineEdit* baseOrNull = panelOrNull->findChild<QLineEdit*>(QStringLiteral("worldTerrainField:minimumElevation"));
    QLineEdit* widthOrNull = panelOrNull->findChild<QLineEdit*>(QStringLiteral("worldTerrainField:width"));
    QLineEdit* seedOrNull = panelOrNull->findChild<QLineEdit*>(QStringLiteral("worldTerrainField:seed"));
    QCheckBox* compareOrNull = panelOrNull->findChild<QCheckBox*>(QStringLiteral("worldTerrainCompare"));
    const QLabel* statusOrNull = panelOrNull->findChild<QLabel*>(QStringLiteral("worldTerrainStatus"));
    REQUIRE(commitOrNull != nullptr);
    REQUIRE(cancelOrNull != nullptr);
    REQUIRE(modeOrNull != nullptr);
    REQUIRE(reliefOrNull != nullptr);
    REQUIRE(baseOrNull != nullptr);
    REQUIRE(widthOrNull != nullptr);
    REQUIRE(seedOrNull != nullptr);
    REQUIRE(compareOrNull != nullptr);
    REQUIRE(statusOrNull != nullptr);

    const QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("worldEntityContextMenu"));
    REQUIRE(menuOrNull != nullptr);
    CHECK(menuOrNull->actions().contains(&viewport.GetAddTerrainAction()) == true);
    CHECK(menuOrNull->actions().contains(&viewport.GetEditTerrainAction()) == true);
    viewport.GetAddTerrainAction().trigger();
    REQUIRE(panelOrNull->isVisible() == true);
    CHECK(commitOrNull->isEnabled() == false);
    REQUIRE(QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    REQUIRE(renderOrNull->Preview().Scene().Objects.size() == 1);
    CHECK(workspace.Document().Draft() == empty);
    // Project and drag the same footprint handle that the native viewport uses.
    ViewportObjectInteraction& interaction = viewport.GetObjectInteraction();
    interaction.SetActive(true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    renderOrNull->Preview().SetAxisView(EditorAxisView::Top);
    const QRectF framePixel(0, 0, 800, 600);
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(framePixel, overlay);
    REQUIRE(overlay.OutlineLinesPixel.GetSize() > 4);
    REQUIRE(overlay.HandleFills.GetSize() == ViewportObjectOverlay::TERRAIN_EXTENT_HANDLES);
    const ViewportHandleFill& widthHandle = overlay.HandleFills[0];
    const QPointF pressPixel = (widthHandle.CornersPixel[0] + widthHandle.CornersPixel[2]) * 0.5;
    const foundation::float64 initialWidth = widthOrNull->text().toDouble();
    REQUIRE(interaction.Press(pressPixel, framePixel) == true);
    REQUIRE(interaction.GetDraggedHandle().Kind == ViewportObjectInteraction::HandleKind::TerrainExtent);
    REQUIRE(interaction.Move(pressPixel + QPointF(20, 0), false) == true);
    REQUIRE(interaction.Release() == true);
    CHECK(widthOrNull->text().toDouble() != initialWidth);
    REQUIRE(QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    CHECK(workspace.Document().Draft() == empty);
    const QByteArray validIdentity = renderOrNull->Preview().Scene().Identity;
    reliefOrNull->setText(QStringLiteral("-1"));
    CHECK(commitOrNull->isEnabled() == false);
    REQUIRE(QTest::qWaitFor([statusOrNull]() { return statusOrNull->text().contains(QStringLiteral("Relief")); },
                            TERRAIN_WAIT_MILLISECONDS));
    CHECK(renderOrNull->Preview().Scene().Identity == validIdentity);
    CHECK(workspace.Document().Draft() == empty);

    modeOrNull->setCurrentIndex(0);
    baseOrNull->setText(QStringLiteral("3"));
    {
        const bool isFlatReady =
            QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS);
        INFO(statusOrNull->text().toStdString());
        REQUIRE(isFlatReady == true);
    }
    REQUIRE(renderOrNull->Preview().Scene().Objects.size() == 1);
    const PreviewObject& preview = renderOrNull->Preview().Scene().Objects.first();
    const QString previewKey = preview.Id;
    CHECK(renderOrNull->Preview().Scene().Meshes[preview.Mesh].Positions.first().Y == 3);
    commitOrNull->click();
    const QJsonObject created = workspace.Document().Draft();
    CHECK(GetTerrainRecipe(workspace.Document(), previewKey).value(QStringLiteral("shape")) == QStringLiteral("Flat"));
    CHECK(GetTerrainRecipe(workspace.Document(), previewKey).value(QStringLiteral("minimumElevation")) == 3);
    CHECK(panelOrNull->isHidden() == true);
    workspace.Undo();
    CHECK(workspace.Document().Draft() == empty);
    workspace.Redo();
    CHECK(workspace.Document().Draft() == created);

    REQUIRE(workspace.SelectEntity(previewKey) == true);
    const QJsonObject occupiedNavigation = renderOrNull->Preview().Navigation();
    viewport.GetEditTerrainAction().trigger();
    REQUIRE(panelOrNull->isVisible() == true);
    CHECK(modeOrNull->currentIndex() == 0);
    const QJsonObject committedTransform = workspace.Document()
                                               .Entity(previewKey)
                                               .value(QStringLiteral("components"))
                                               .toObject()
                                               .value(QStringLiteral("transform"))
                                               .toObject();
    modeOrNull->setCurrentIndex(1);
    seedOrNull->setText(QStringLiteral("41"));
    seedOrNull->setText(QStringLiteral("42"));
    REQUIRE(QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    CHECK(workspace.Document().Draft() == created);
    CHECK(renderOrNull->Preview().Navigation() == occupiedNavigation);
    compareOrNull->click();
    REQUIRE(QTest::qWaitFor([compareOrNull]() { return compareOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    CHECK(commitOrNull->isEnabled() == false);
    compareOrNull->click();
    REQUIRE(QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    commitOrNull->click();
    CHECK(GetTerrainRecipe(workspace.Document(), previewKey).value(QStringLiteral("seed")) == 42);
    CHECK(GetTerrainRecipe(workspace.Document(), previewKey).value(QStringLiteral("roughness")) == 0.45);
    CHECK(workspace.Document()
              .Entity(previewKey)
              .value(QStringLiteral("components"))
              .toObject()
              .value(QStringLiteral("transform"))
              .toObject() == committedTransform);
    workspace.Undo();
    CHECK(workspace.Document().Draft() == created);
    workspace.Redo();

    REQUIRE(workspace.Save() == true);
    const QJsonObject saved = workspace.Document().Draft();
    viewport.GetEditTerrainAction().trigger();
    REQUIRE(panelOrNull->isVisible() == true);
    seedOrNull->setText(QStringLiteral("43"));
    workspace.Undo();
    CHECK(statusOrNull->text().contains(QStringLiteral("Refresh Draft")) == true);
    CHECK(commitOrNull->isEnabled() == false);
    workspace.Redo();
    CHECK(commitOrNull->isEnabled() == false);
    QPushButton* refreshOrNull = panelOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainRefresh"));
    REQUIRE(refreshOrNull != nullptr);
    refreshOrNull->click();
    REQUIRE(QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    CHECK(seedOrNull->text() == QStringLiteral("43"));
    cancelOrNull->click();
    CHECK(panelOrNull->isHidden() == true);
    CHECK(workspace.Document().Draft() == saved);

    viewport.GetEditTerrainAction().trigger();
    REQUIRE(panelOrNull->isVisible() == true);
    seedOrNull->setText(QStringLiteral("44"));
    REQUIRE(workspace.Open(path) == true);
    CHECK(panelOrNull->isHidden() == true);
    CHECK(workspace.Document().Draft() == saved);

    REQUIRE(workspace.SelectEntity(previewKey) == true);
    viewport.GetEditTerrainAction().trigger();
    QComboBox* brushOrNull = panelOrNull->findChild<QComboBox*>(QStringLiteral("worldTerrainSculptBrush"));
    QDoubleSpinBox* deltaOrNull = panelOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("worldTerrainBrushDelta"));
    QDoubleSpinBox* strengthOrNull =
        panelOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("worldTerrainBrushStrength"));
    QPushButton* pickOrNull = panelOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainPickHeight"));
    QDoubleSpinBox* targetOrNull = panelOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("worldTerrainBrushHeight"));
    QCheckBox* slopeOrNull = panelOrNull->findChild<QCheckBox*>(QStringLiteral("worldTerrainShowSlope"));
    REQUIRE(brushOrNull != nullptr);
    REQUIRE(deltaOrNull != nullptr);
    REQUIRE(strengthOrNull != nullptr);
    REQUIRE(pickOrNull != nullptr);
    REQUIRE(targetOrNull != nullptr);
    REQUIRE(slopeOrNull != nullptr);
    REQUIRE(panelOrNull->isVisible() == true);
    const bool canFlatten =
        QTest::qWaitFor([brushOrNull]() { return brushOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS);
    INFO(statusOrNull->text().toStdString());
    REQUIRE(canFlatten == true);
    interaction.SetActive(true);
    renderOrNull->Preview().SetAxisView(EditorAxisView::Top);
    interaction.ClearPresentedView();
    brushOrNull->setCurrentIndex(brushOrNull->findData(static_cast<foundation::int32>(TerrainBrush::Flatten)));
    REQUIRE(slopeOrNull->isVisible() == true);
    slopeOrNull->setChecked(true);
    REQUIRE(interaction.Hover(framePixel.center(), framePixel) == true);
    ViewportObjectOverlay slopeOverlay;
    interaction.BuildOverlay(framePixel, slopeOverlay);
    CHECK(slopeOverlay.ReadoutText.startsWith(QStringLiteral("Slope ")) == true);
    CHECK(slopeOverlay.ReadoutText.endsWith(QChar(u'°')) == true);
    slopeOrNull->setChecked(false);
    REQUIRE(interaction.Hover(framePixel.center(), framePixel) == true);
    interaction.BuildOverlay(framePixel, slopeOverlay);
    CHECK(slopeOverlay.ReadoutText.isEmpty() == true);
    pickOrNull->click();
    REQUIRE(interaction.Press(framePixel.center(), framePixel) == true);
    CHECK(pickOrNull->isChecked() == false);
    targetOrNull->setValue(2);
    const RenderScene committedScene = renderOrNull->Preview().Scene();
    REQUIRE(committedScene.Objects.size() == 1);
    const foundation::int32 terrainMesh = committedScene.Objects.first().Mesh;
    REQUIRE(interaction.Press(framePixel.center(), framePixel) == true);
    REQUIRE(interaction.Move(framePixel.center() + QPointF(40, 0), false) == true);
    // The held stroke reshapes the viewport terrain before release without editing the World.
    REQUIRE(QTest::qWaitFor(
        [renderOrNull, &committedScene]() {
            return renderOrNull->Preview().Scene().Identity != committedScene.Identity;
        },
        TERRAIN_WAIT_MILLISECONDS));
    CHECK(renderOrNull->Preview().Scene().Meshes[terrainMesh].Positions !=
          committedScene.Meshes[terrainMesh].Positions);
    CHECK(interaction.IsDragging() == true);
    CHECK(workspace.Document().Draft() == saved);
    const QByteArray partialScene = renderOrNull->Preview().Scene().Identity;
    REQUIRE(interaction.Move(framePixel.center() + QPointF(80, 0), false) == true);
    REQUIRE(QTest::qWaitFor(
        [renderOrNull, &committedScene, &partialScene]() {
            const QByteArray& identity = renderOrNull->Preview().Scene().Identity;
            return identity != partialScene && identity != committedScene.Identity;
        },
        TERRAIN_WAIT_MILLISECONDS));
    CHECK(interaction.IsDragging() == true);
    CHECK(workspace.Document().Draft() == saved);
    interaction.Cancel();
    CHECK(renderOrNull->Preview().Scene().Identity == committedScene.Identity);
    CHECK(interaction.Release() == false);
    CHECK(workspace.Document().Draft() == saved);
    {
        // Pausing sculpting mid-drag drops the stroke; the live preview must roll back rather than wait for a
        // release that never comes.
        const QDoubleSpinBox* radiusOrNull =
            panelOrNull->findChild<QDoubleSpinBox*>(QStringLiteral("worldTerrainBrushRadius"));
        REQUIRE(radiusOrNull != nullptr);
        const foundation::float32 radius = static_cast<foundation::float32>(radiusOrNull->value());
        REQUIRE(interaction.Press(framePixel.center(), framePixel) == true);
        REQUIRE(interaction.Move(framePixel.center() + QPointF(40, 0), false) == true);
        REQUIRE(QTest::qWaitFor(
            [renderOrNull, &committedScene]() {
                return renderOrNull->Preview().Scene().Identity != committedScene.Identity;
            },
            TERRAIN_WAIT_MILLISECONDS));
        interaction.SetTerrainBrush(previewKey, radius, false, false, false);
        CHECK(interaction.IsDragging() == false);
        CHECK(renderOrNull->Preview().Scene().Identity == committedScene.Identity);
        CHECK(interaction.Release() == false);
        CHECK(workspace.Document().Draft() == saved);
        interaction.SetTerrainBrush(previewKey, radius, false, false, true);
    }
    REQUIRE(interaction.Press(framePixel.center(), framePixel) == true);
    REQUIRE(interaction.Move(framePixel.center() + QPointF(40, 0), false) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(QTest::qWaitFor(
        [&workspace, &previewKey]() {
            return GetTerrainRecipe(workspace.Document(), previewKey).contains(QStringLiteral("flattenStrokes"));
        },
        TERRAIN_WAIT_MILLISECONDS));
    const QJsonArray strokes =
        GetTerrainRecipe(workspace.Document(), previewKey).value(QStringLiteral("flattenStrokes")).toArray();
    REQUIRE(strokes.size() == 1);
    CHECK(strokes[0].toObject().value(QStringLiteral("height")) == 2);
    CHECK(strokes[0].toObject().value(QStringLiteral("points")).toArray().size() == 2);
    const QJsonObject flattened = workspace.Document().Draft();
    const auto waitForStrokeCount = [&workspace, &previewKey](qsizetype count) {
        return QTest::qWaitFor(
            [&workspace, &previewKey, count]() {
                return GetTerrainRecipe(workspace.Document(), previewKey)
                           .value(QStringLiteral("sculptStrokes"))
                           .toArray()
                           .size() == count;
            },
            TERRAIN_WAIT_MILLISECONDS);
    };

    brushOrNull->setCurrentIndex(brushOrNull->findData(static_cast<foundation::int32>(TerrainBrush::RaiseLower)));
    REQUIRE(deltaOrNull->isVisible() == true);
    CHECK(targetOrNull->isHidden() == true);
    deltaOrNull->setValue(-1);
    // The Move tool would show transform handles if the brush let go of the viewport.
    interaction.SetTool(ViewportObjectInteraction::Tool::Move);
    REQUIRE(interaction.Press(framePixel.center(), framePixel) == true);
    REQUIRE(interaction.Release() == true);
    {
        // While the stroke's preview is pending, the brush still owns presses: no selection, no handles.
        CHECK(brushOrNull->isEnabled() == false);
        QSignalSpy selections(&interaction, &ViewportObjectInteraction::SelectionRequested);
        REQUIRE(interaction.Hover(framePixel.center(), framePixel) == true);
        ViewportObjectOverlay pendingOverlay;
        interaction.BuildOverlay(framePixel, pendingOverlay);
        CHECK(pendingOverlay.HandleStrokes.IsEmpty() == false);
        CHECK(pendingOverlay.HandleFills.IsEmpty() == true);
        CHECK(interaction.Press(framePixel.center(), framePixel) == true);
        CHECK(interaction.IsDragging() == false);
        CHECK(interaction.Release() == false);
        CHECK(selections.isEmpty() == true);
        CHECK(statusOrNull->text().contains(QStringLiteral("previous stroke")) == true);
    }
    REQUIRE(waitForStrokeCount(2) == true);
    {
        ViewportObjectOverlay readyOverlay;
        interaction.BuildOverlay(framePixel, readyOverlay);
        CHECK(readyOverlay.HandleFills.IsEmpty() == true);
    }
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    const QJsonObject lowered = workspace.Document().Draft();

    REQUIRE(interaction.Press(framePixel.center(), framePixel) == true);
    brushOrNull->setCurrentIndex(brushOrNull->findData(static_cast<foundation::int32>(TerrainBrush::Smooth)));
    CHECK(interaction.Release() == false);
    CHECK(workspace.Document().Draft() == lowered);
    CHECK(deltaOrNull->isHidden() == true);
    REQUIRE(strengthOrNull->isVisible() == true);
    strengthOrNull->setValue(1);
    REQUIRE(interaction.Press(framePixel.center(), framePixel) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(waitForStrokeCount(3) == true);
    const QJsonObject smoothed = workspace.Document().Draft();

    workspace.Undo();
    CHECK(workspace.Document().Draft() == lowered);
    workspace.Undo();
    CHECK(workspace.Document().Draft() == flattened);
    workspace.Undo();
    CHECK(workspace.Document().Draft() == saved);
    workspace.Redo();
    workspace.Redo();
    workspace.Redo();
    CHECK(workspace.Document().Draft() == smoothed);
    REQUIRE(workspace.Save() == true);
    REQUIRE(workspace.Open(path) == true);
    CHECK(workspace.Document().Draft() == smoothed);
}
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
TEST_CASE("Terrain draft publication keeps the delegated World view during a renderer refusal",
          "[editor][world][terrain][world-graph]")
{
    if (PreviewViewport::CanPresent() == false)
    {
        SKIP("Requires a native rendered World viewport.");
    }
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    RendererWorkspace renderer({});
    WorldWorkspace world;
    world.UseProjectGraph(renderer);
    renderer.SetPreviewSource(&world.GetRenderPreview());
    renderer.EnableWorldRendering();
    renderer.SetProject(root, 1);
    world.SetProject(root, 1);
    world.resize(1000, 700);
    world.show();
    REQUIRE(world.Create(QDir(root).filePath(QStringLiteral("terrain.world.json"))) == true);
    const QJsonObject emptyWorld = world.Document().Draft();
    RenderPanel* renderOrNull = world.findChild<RenderPanel*>();
    REQUIRE(renderOrNull != nullptr);
    PreviewViewport& viewport = renderOrNull->GetViewport();
    viewport.GetAddTerrainAction().trigger();
    const QLabel* statusOrNull = world.findChild<QLabel*>(QStringLiteral("worldTerrainStatus"));
    QPushButton* commitOrNull = world.findChild<QPushButton*>(QStringLiteral("worldTerrainCommit"));
    QLineEdit* widthOrNull = world.findChild<QLineEdit*>(QStringLiteral("worldTerrainField:width"));
    QPushButton* refreshOrNull = world.findChild<QPushButton*>(QStringLiteral("worldTerrainRefresh"));
    REQUIRE(statusOrNull != nullptr);
    REQUIRE(commitOrNull != nullptr);
    REQUIRE(widthOrNull != nullptr);
    REQUIRE(refreshOrNull != nullptr);
    REQUIRE(QTest::qWaitFor(
        [&]() {
            renderer.ServicePreview();
            return commitOrNull->isEnabled() == true && viewport.PresentedFrames() > 0;
        },
        TERRAIN_WAIT_MILLISECONDS));

    const QByteArray readyScene = world.GetRenderPreview().Scene().Identity;
    const foundation::uint64 readyFrames = viewport.PresentedFrames();
    const QString problem = QStringLiteral("Raster profile unavailable. Choose a runnable profile and refresh.");
    const QMetaObject::Connection refusal = QObject::connect(renderOrNull, &RenderPanel::Starting, &world, [&]() {
        world.GetRenderPreview().SetProjectGraphProblem(problem);
    });
    widthOrNull->setText(QStringLiteral("64"));
    CHECK(commitOrNull->isEnabled() == false);
    REQUIRE(QTest::qWaitFor([&]() { return statusOrNull->text().contains(problem); }, TERRAIN_WAIT_MILLISECONDS));
    CHECK(world.GetRenderPreview().Scene().Identity == readyScene);
    CHECK(viewport.PresentedFrames() >= readyFrames);
    CHECK(world.Document().Draft() == emptyWorld);

    QObject::disconnect(refusal);
    world.GetRenderPreview().SetProjectGraphProblem({});
    refreshOrNull->click();
    world.resize(960, 640);
    world.GetRenderPreview().Orbit(10, 5);
    REQUIRE(QTest::qWaitFor(
        [&]() {
            renderer.ServicePreview();
            return commitOrNull->isEnabled() == true && viewport.PresentedFrames() > readyFrames;
        },
        TERRAIN_WAIT_MILLISECONDS));
    CHECK(world.GetRenderPreview().Scene().Identity != readyScene);
    commitOrNull->click();
    REQUIRE(world.Document().Draft() != emptyWorld);
    world.StopPreview();
    renderer.SetPreviewSource(nullptr);
}
#endif
TEST_CASE("Cancelling terrain in an imported World restores the composed scene", "[editor][world][terrain]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString source = QDir(root).filePath(QStringLiteral("source"));
    QString error;
    INFO(error.toStdString());
    REQUIRE(tests::CreateFixtureSource(source) == true);
    REQUIRE(tests::PrepareImportedLevel(root, QStringLiteral("room.lulevel"), source, error) == true);
    QFile levelFile(QDir(root).filePath(QStringLiteral("room.lulevel")));
    REQUIRE(levelFile.open(QIODevice::ReadOnly) == true);
    const QJsonObject level = QJsonDocument::fromJson(levelFile.readAll()).object();
    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    REQUIRE(document.ImportScene(level, error) == true);
    QFile worldFile(QDir(root).filePath(QStringLiteral("imported.world.json")));
    REQUIRE(worldFile.open(QIODevice::WriteOnly) == true);
    const QByteArray bytes = document.Encode();
    REQUIRE(worldFile.write(bytes) == bytes.size());
    worldFile.close();

    ToolingPaths tooling;
    tooling.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    WorldWorkspace workspace(tooling, nullptr);
    workspace.SetProject(root, 1);
    workspace.show();
    LevelWorkspace* levelWorkspaceOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(levelWorkspaceOrNull != nullptr);
    QSignalSpy loaded(levelWorkspaceOrNull, &LevelWorkspace::OperationFinished);
    REQUIRE(workspace.Open(worldFile.fileName()) == true);
    constexpr foundation::int32 SCENE_TIMEOUT_MILLISECONDS = 15000;
    REQUIRE(loaded.wait(SCENE_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(loaded.last().first().toBool() == true);
    const qsizetype importedObjects = levelWorkspaceOrNull->PreviewScene().Objects.size();
    REQUIRE(importedObjects > 0);
    QAction* generateOrNull = workspace.findChild<QAction*>(QStringLiteral("worldGenerateTerrain"));
    REQUIRE(generateOrNull != nullptr);
    generateOrNull->trigger();
    QWidget* panelOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldTerrainPanel"));
    REQUIRE(panelOrNull != nullptr);
    QPushButton* cancelOrNull = panelOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainCancel"));
    REQUIRE(cancelOrNull != nullptr);
    REQUIRE(loaded.wait(SCENE_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(loaded.last().first().toBool() == true);
    CHECK(levelWorkspaceOrNull->PreviewScene().Objects.size() == importedObjects + 1);
    CHECK(workspace.Document().Encode() == bytes);

    cancelOrNull->click();
    REQUIRE(loaded.wait(SCENE_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(loaded.last().first().toBool() == true);
    CHECK(levelWorkspaceOrNull->PreviewScene().Objects.size() == importedObjects);
    CHECK(workspace.Document().Encode() == bytes);
}
} // namespace ludus::editor
