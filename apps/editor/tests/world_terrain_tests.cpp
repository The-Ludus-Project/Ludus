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
#include "internal/world_terrain_source.h"
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

void OpenTerrainCreation(WorldWorkspace& inoutWorld)
{
    QAction* addOrNull = inoutWorld.findChild<QAction*>(QStringLiteral("worldGenerateTerrain"));
    REQUIRE(addOrNull != nullptr);
    addOrNull->trigger();
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

TEST_CASE("Heightmap corners, axis flips and sculpt replay use local metres", "[editor][world][terrain]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString relative = QStringLiteral("elevations.png");
    const QString path = QDir(root).filePath(relative);
    QImage source(2, 2, QImage::Format_Grayscale16);
    REQUIRE(source.isNull() == false);
    foundation::uint16* north = reinterpret_cast<foundation::uint16*>(source.scanLine(0));
    foundation::uint16* south = reinterpret_cast<foundation::uint16*>(source.scanLine(1));
    north[0] = 0;
    north[1] = 65535;
    south[0] = 32768;
    south[1] = 0;
    REQUIRE(source.save(path, "PNG") == true);

    QString error;
    TerrainHeightmap heightmap;
    REQUIRE(LoadTerrainHeightmap(root, relative, {}, heightmap, error) == true);
    CHECK(heightmap.Image.format() == QImage::Format_Grayscale16);
    CHECK(heightmap.Sample(0, 0) == 0);
    CHECK(heightmap.Sample(1, 0) == 1);
    CHECK(heightmap.Sample(0, 1) == 32768.0 / 65535.0);
    CHECK(LoadTerrainHeightmap(root, QStringLiteral("../elevations.png"), {}, heightmap, error) == false);

    const QString oversized = QStringLiteral("oversized.png");
    QImage wide(TERRAIN_HEIGHTMAP_MAX_SIDE + 1, 2, QImage::Format_Grayscale8);
    wide.fill(0);
    REQUIRE(wide.save(QDir(root).filePath(oversized), "PNG") == true);
    CHECK(LoadTerrainHeightmap(root, oversized, {}, heightmap, error) == false);
    CHECK(error.contains(QStringLiteral("pixels per axis")) == true);
    CHECK(heightmap.Sample(1, 0) == 1);
    const QString color = QStringLiteral("color.png");
    QImage rgba(2, 2, QImage::Format_RGBA8888);
    rgba.fill(Qt::red);
    REQUIRE(rgba.save(QDir(root).filePath(color), "PNG") == true);
    CHECK(LoadTerrainHeightmap(root, color, {}, heightmap, error) == false);
    CHECK(error.contains(QStringLiteral("grayscale PNG")) == true);

    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    QJsonObject recipe = GetDefaultRecipe(world);
    recipe.insert(QStringLiteral("minimumElevation"), -4);
    recipe.insert(QStringLiteral("maximumElevation"), 12);
    recipe.insert(QStringLiteral("heightmap"),
                  QJsonObject{{QStringLiteral("path"), relative},
                              {QStringLiteral("sha256"), heightmap.Sha256},
                              {QStringLiteral("flip_x"), true},
                              {QStringLiteral("flip_z"), false}});
    PreviewMesh mesh;
    REQUIRE(BuildTerrainMesh(recipe, mesh, error, root) == true);
    CHECK(mesh.Positions.first().Y == 12);
    CHECK(mesh.Positions[32].Y == -4);
    const foundation::int32 center = 16 * 33 + 16;
    const foundation::float32 before = mesh.Positions[center].Y;
    const foundation::math::Vector3 normalBefore = mesh.Normals[center + 1];
    const TerrainBrush flatten{TerrainBrushKind::Flatten, 4, 1, 2.4, false};
    REQUIRE(AppendTerrainStroke(flatten, QJsonArray{QJsonArray{0, 0}}, recipe, error) == true);
    REQUIRE(BuildTerrainMesh(recipe, mesh, error, root) == true);
    CHECK(mesh.Positions[center].Y != before);
    CHECK(mesh.Positions[center].Y == 2.4F);
    CHECK(mesh.Normals[center + 1] != normalBefore);

    QJsonObject replacement = recipe;
    replacement.remove(QStringLiteral("heightmap"));
    replacement.insert(QStringLiteral("shape"), QStringLiteral("Flat"));
    replacement.insert(QStringLiteral("minimumElevation"), 8);
    REQUIRE(FindClippedTerrainStrokes(recipe, replacement).isEmpty() == true);
    REQUIRE(BuildTerrainMesh(replacement, mesh, error) == true);
    CHECK(mesh.Positions[center].Y == 2.4F);
    CHECK(mesh.Positions.first().Y == 8);

    replacement.insert(QStringLiteral("width"), 2);
    const QJsonArray clipped = FindClippedTerrainStrokes(recipe, replacement);
    REQUIRE(clipped.size() == 1);
    RemoveTerrainStrokes(clipped, replacement);
    REQUIRE(BuildTerrainMesh(replacement, mesh, error) == true);
    CHECK(mesh.Positions[center].Y == 8);

    QString key;
    REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), {}, recipe, key, error) == true);
    WorldDocument reopened;
    REQUIRE(reopened.LoadSchema(schema, error) == true);
    REQUIRE(reopened.Load(world.Encode(), error) == true);
    CHECK(reopened.Draft() == world.Draft());
    source.fill(0);
    REQUIRE(source.save(path, "PNG") == true);
    CHECK(BuildTerrainMesh(recipe, mesh, error, root) == false);
    CHECK(error.contains(QStringLiteral("saved heightmap bytes changed")) == true);
    CHECK(mesh.Positions[center].Y == 8);
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

    OpenTerrainCreation(workspace);
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
    OpenTerrainCreation(world);
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
TEST_CASE("Terrain dock imports a raster and commits one native brush stroke", "[editor][world][terrain]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString image = QStringLiteral("ground.png");
    QImage heightmap(2, 2, QImage::Format_Grayscale8);
    heightmap.fill(0);
    REQUIRE(heightmap.save(QDir(root).filePath(image), "PNG") == true);

    WorldWorkspace world;
    world.SetEntropySource(tests::MakeFixedEntropySource());
    world.SetProject(root, 1);
    world.resize(960, 700);
    world.show();
    REQUIRE(world.Create(QDir(root).filePath(QStringLiteral("terrain.world.json"))) == true);
    RenderPanel* renderOrNull = world.findChild<RenderPanel*>();
    REQUIRE(renderOrNull != nullptr);
    PreviewViewport& viewport = renderOrNull->GetViewport();
    OpenTerrainCreation(world);
    QTabBar* sourceModeOrNull = world.findChild<QTabBar*>(QStringLiteral("worldTerrainMode"));
    QLineEdit* pathOrNull = world.findChild<QLineEdit*>(QStringLiteral("worldTerrainHeightmapPath"));
    QPushButton* commitOrNull = world.findChild<QPushButton*>(QStringLiteral("worldTerrainCommit"));
    REQUIRE(sourceModeOrNull != nullptr);
    REQUIRE(pathOrNull != nullptr);
    REQUIRE(commitOrNull != nullptr);
    sourceModeOrNull->setCurrentIndex(2);
    pathOrNull->setText(image);
    REQUIRE(QTest::qWaitFor([&]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    REQUIRE(world.GetRenderPreview().Scene().Objects.size() == 1);
    const QString key = world.GetRenderPreview().Scene().Objects.first().Id;
    commitOrNull->click();
    const QJsonObject imported = world.Document().Draft();
    CHECK(GetTerrainRecipe(world.Document(), key)
              .value(QStringLiteral("heightmap"))
              .toObject()
              .value(QStringLiteral("path"))
              .toString() == image);

    if (PreviewViewport::CanPresent() == false)
    {
        return;
    }
    viewport.GetEditTerrainAction().trigger();
    QCheckBox* sculptOrNull = world.findChild<QCheckBox*>(QStringLiteral("worldTerrainSculpt"));
    QComboBox* brushOrNull = world.findChild<QComboBox*>(QStringLiteral("worldTerrainBrushKind"));
    QDoubleSpinBox* strengthOrNull = world.findChild<QDoubleSpinBox*>(QStringLiteral("worldTerrainBrushStrength"));
    QDoubleSpinBox* elevationOrNull = world.findChild<QDoubleSpinBox*>(QStringLiteral("worldTerrainBrushElevation"));
    REQUIRE(sculptOrNull != nullptr);
    REQUIRE(brushOrNull != nullptr);
    REQUIRE(strengthOrNull != nullptr);
    REQUIRE(elevationOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([&]() { return sculptOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    REQUIRE(sourceModeOrNull->currentIndex() == 2);
    sculptOrNull->click();
    REQUIRE(sculptOrNull->isChecked() == true);
    brushOrNull->setCurrentIndex(brushOrNull->findData(static_cast<foundation::int32>(TerrainBrushKind::Flatten)));
    strengthOrNull->setValue(1);
    elevationOrNull->setValue(2.4);
    RenderPreview& preview = world.GetRenderPreview();
    preview.SetAxisView(EditorAxisView::Top);
    preview.SetSelection(key);
    REQUIRE(preview.FrameSelection() == true);
    ViewportObjectInteraction& interaction = viewport.GetObjectInteraction();
    const QRectF frame(0, 0, 800, 600);
    ludus::graphics::renderer::ViewDescription view;
    QString viewError;
    REQUIRE(preview.DescribeView(800, 600, view, viewError) == true);
    interaction.SetImageSize({800, 600});
    interaction.SetPresentedView(view.WorldToClip);
    const auto project = [&view, &frame](foundation::float32 x) {
        const foundation::math::Vector4 clip = view.WorldToClip * foundation::math::Vector4{x, 0, 0, 1};
        return frame.center() + QPointF(static_cast<foundation::float64>(clip.X / clip.W) * frame.width() * 0.5,
                                        static_cast<foundation::float64>(-clip.Y / clip.W) * frame.height() * 0.5);
    };
    const QPointF startPixel = project(0);
    const QPointF endPixel = project(4);
    REQUIRE(interaction.Pick(endPixel, frame) == key);
    REQUIRE(interaction.Press(startPixel, frame) == true);
    REQUIRE(interaction.GetDraggedHandle().Kind == ViewportObjectInteraction::HandleKind::TerrainBrush);
    REQUIRE(interaction.Move(endPixel, false) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(QTest::qWaitFor([&]() { return world.Document().Draft() != imported; }, TERRAIN_WAIT_MILLISECONDS));
    const QJsonObject sculpted = world.Document().Draft();
    const QJsonObject layer = GetTerrainRecipe(world.Document(), key).value(QStringLiteral("sculpt")).toObject();
    REQUIRE(layer.value(QStringLiteral("strokes")).toArray().size() == 1);
    CHECK(layer.value(QStringLiteral("strokes")).toArray().first().toObject().value(QStringLiteral("target")) == 2.4);
    CHECK(layer.value(QStringLiteral("strokes"))
              .toArray()
              .first()
              .toObject()
              .value(QStringLiteral("points"))
              .toArray()
              .size() > 1);
    world.Undo();
    CHECK(world.Document().Draft() == imported);
    world.Redo();
    CHECK(world.Document().Draft() == sculpted);
    REQUIRE(QTest::qWaitFor([&]() { return sculptOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    sculptOrNull->click();
    REQUIRE(sculptOrNull->isChecked() == false);
    sourceModeOrNull->setCurrentIndex(0);
    QLineEdit* baseOrNull = world.findChild<QLineEdit*>(QStringLiteral("worldTerrainField:minimumElevation"));
    REQUIRE(baseOrNull != nullptr);
    baseOrNull->setText(QStringLiteral("5"));
    REQUIRE(QTest::qWaitFor([&]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    const PreviewObject& shown = preview.Scene().Objects.first();
    const PreviewMesh& mesh = preview.Scene().Meshes[shown.Mesh];
    CHECK(mesh.Positions.first().Y == 5);
    CHECK(mesh.Positions[16 * 33 + 16].Y == 2.4F);
    commitOrNull->click();
    CHECK(GetTerrainRecipe(world.Document(), key).contains(QStringLiteral("heightmap")) == false);
    CHECK(GetTerrainRecipe(world.Document(), key).value(QStringLiteral("sculpt")).toObject() == layer);
    world.Undo();
    CHECK(world.Document().Draft() == sculpted);
}
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
