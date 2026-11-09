#include "entropy_fixture.h"
#include "internal/builtin_world.h"
#include "internal/controller.h"
#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
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
    REQUIRE(QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
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
