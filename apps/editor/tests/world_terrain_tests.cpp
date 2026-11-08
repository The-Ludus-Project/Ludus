#include "entropy_fixture.h"
#include "internal/builtin_world.h"
#include "internal/controller.h"
#include "internal/level_workspace.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/world_document.h"
#include "internal/world_terrain.h"
#include "internal/world_workspace.h"
#include "scene_fixture.h"

#include <QAction>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
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
QDialog* FindOpenTerrainDialog(const WorldWorkspace& workspace)
{
    for (QDialog* dialog : workspace.findChildren<QDialog*>(QStringLiteral("worldTerrainGenerator")))
    {
        if (dialog->isVisible() == true)
        {
            return dialog;
        }
    }
    return nullptr;
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

TEST_CASE("Explore as Player lands on the generated terrain after World extraction",
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
    QString key;
    REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), {}, recipe, key, error) == true);
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    RenderScene scene;
    REQUIRE(RenderScene::Load(root.path(), world, scene, error, false) == true);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    gameplay::camera::CameraSample spawn;
    spawn.Pose.Position = {0, 4, 0};
    spawn.Lens.Projection = gameplay::camera::CameraProjection::PerspectiveInfinite;
    REQUIRE(preview.BeginFirstPerson(spawn, error) == true);
    for (foundation::uint32 frame = 0; frame < 10; ++frame)
    {
        preview.AdvanceFirstPerson({1, 0, 0}, 0.1F, 3, false);
    }
    const foundation::math::Vector3d eyeWorld = preview.GetVisibleCameraSample().Pose.Position;
    CHECK(eyeWorld.X == Catch::Approx(3));
    CHECK(eyeWorld.Y == Catch::Approx(3.602).margin(0.003));
}

TEST_CASE("Generate Terrain previews without editing and saves one reversible entity", "[editor][world][terrain]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString path = QDir(root).filePath(QStringLiteral("terrain.world.json"));
    WorldWorkspace workspace;
    workspace.SetEntropySource(tests::MakeFixedEntropySource());
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    const QJsonObject empty = workspace.Document().Draft();
    QAction* generateOrNull = workspace.findChild<QAction*>(QStringLiteral("worldGenerateTerrain"));
    RenderPanel* renderOrNull = workspace.findChild<RenderPanel*>();
    REQUIRE(generateOrNull != nullptr);
    REQUIRE(renderOrNull != nullptr);
    generateOrNull->trigger();
    QDialog* dialogOrNull = FindOpenTerrainDialog(workspace);
    REQUIRE(dialogOrNull != nullptr);
    QPushButton* previewOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainPreview"));
    QLineEdit* minimumOrNull =
        dialogOrNull->findChild<QLineEdit*>(QStringLiteral("worldTerrainField:minimumElevation"));
    const QLabel* statusOrNull = dialogOrNull->findChild<QLabel*>(QStringLiteral("worldTerrainStatus"));
    REQUIRE(previewOrNull != nullptr);
    REQUIRE(minimumOrNull != nullptr);
    REQUIRE(statusOrNull != nullptr);
    minimumOrNull->setText(QStringLiteral("12"));
    previewOrNull->click();
    CHECK(statusOrNull->text().contains(QStringLiteral("maximum elevation")) == true);
    CHECK(workspace.Document().Draft() == empty);

    minimumOrNull->setText(QStringLiteral("0"));
    QSignalSpy loaded(renderOrNull, &RenderPanel::Loaded);
    previewOrNull->click();
    REQUIRE(loaded.wait() == true);
    CHECK(renderOrNull->Preview().Scene().Objects.size() == 1);
    CHECK(workspace.Document().Draft() == empty);
    const QByteArray previewIdentity = renderOrNull->Preview().Scene().Identity;
    minimumOrNull->setText(QStringLiteral("12"));
    previewOrNull->click();
    CHECK(statusOrNull->text().contains(QStringLiteral("maximum elevation")) == true);
    CHECK(renderOrNull->Preview().Scene().Identity == previewIdentity);
    minimumOrNull->setText(QStringLiteral("0"));
    previewOrNull->click();
    CHECK(statusOrNull->text().contains(QStringLiteral("preview ready")) == true);
    dialogOrNull->reject();
    REQUIRE(loaded.wait() == true);
    CHECK(renderOrNull->Preview().Scene().Objects.isEmpty() == true);

    generateOrNull->trigger();
    dialogOrNull = FindOpenTerrainDialog(workspace);
    REQUIRE(dialogOrNull != nullptr);
    QPushButton* commitOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainCommit"));
    REQUIRE(commitOrNull != nullptr);
    commitOrNull->click();
    REQUIRE(loaded.wait() == true);
    const QJsonObject created = workspace.Document().Draft();
    const QJsonArray entities = created.value(QStringLiteral("entities")).toArray();
    REQUIRE(entities.size() == 1);
    CHECK(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(workspace.Document().Draft() == empty);
    workspace.Redo();
    CHECK(workspace.Document().Draft() == created);

    QPushButton* editOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldEditTerrainGeneration"));
    if (editOrNull == nullptr)
    {
        // Redo restores the document but does not recreate the previous selection.
        REQUIRE(workspace.SelectEntity(workspace.Document().GetEntityKey(entities.first().toObject())) == true);
        editOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldEditTerrainGeneration"));
    }
    REQUIRE(editOrNull != nullptr);
    editOrNull->click();
    dialogOrNull = FindOpenTerrainDialog(workspace);
    REQUIRE(dialogOrNull != nullptr);
    QLineEdit* seedOrNull = dialogOrNull->findChild<QLineEdit*>(QStringLiteral("worldTerrainField:seed"));
    commitOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainCommit"));
    REQUIRE(seedOrNull != nullptr);
    REQUIRE(commitOrNull != nullptr);
    seedOrNull->setText(QStringLiteral("42"));
    QSignalSpy applied(dialogOrNull, &QDialog::accepted);
    commitOrNull->click();
    REQUIRE((applied.isEmpty() == false || applied.wait() == true));
    CHECK(workspace.Document().Draft() != created);
    workspace.Undo();
    CHECK(workspace.Document().Draft() == created);
    workspace.Redo();

    REQUIRE(workspace.Save() == true);
    WorldWorkspace reopened;
    reopened.SetProject(root, 2);
    REQUIRE(reopened.Open(path) == true);
    CHECK(reopened.Document().Draft() == workspace.Document().Draft());
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
    QDialog* dialogOrNull = FindOpenTerrainDialog(workspace);
    REQUIRE(dialogOrNull != nullptr);
    QPushButton* previewOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainPreview"));
    REQUIRE(previewOrNull != nullptr);
    previewOrNull->click();
    REQUIRE(loaded.wait(SCENE_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(loaded.last().first().toBool() == true);
    CHECK(levelWorkspaceOrNull->PreviewScene().Objects.size() == importedObjects + 1);
    CHECK(workspace.Document().Encode() == bytes);

    dialogOrNull->reject();
    REQUIRE(loaded.wait(SCENE_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(loaded.last().first().toBool() == true);
    CHECK(levelWorkspaceOrNull->PreviewScene().Objects.size() == importedObjects);
    CHECK(workspace.Document().Encode() == bytes);
}
} // namespace ludus::editor
