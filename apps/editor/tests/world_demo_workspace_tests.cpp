#include "entropy_fixture.h"
#include "internal/editor_identity.h"
#include "internal/preview_shading.h"
#include "internal/render_panel.h"
#include "internal/render_scene.h"
#include "internal/world_demo_adapter.h"
#include "internal/world_workspace.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
namespace
{
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}
QByteArray Read(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
QString WorldFile(QTemporaryDir& directory)
{
    REQUIRE(directory.isValid());
    const auto path = QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("room.world.json"));
    const auto sample = ludus::world_demo::ExampleLevel();
    Write(path, QByteArray(sample.data(), static_cast<ludus::foundation::int64>(sample.size())));
    return path;
}
void Select(WorldWorkspace& view, const QString& id)
{
    auto* list = view.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(list != nullptr);
    for (ludus::foundation::int32 row = 0; row < list->count(); ++row)
    {
        if (list->item(row)->data(Qt::UserRole).toString() == id)
        {
            list->setCurrentRow(row);
            return;
        }
    }
    FAIL("Missing World entity");
}
QLineEdit* Field(WorldWorkspace& view, const QString& key)
{
    // In the editor window the entity inspector is the Properties tool window.
    QLineEdit* field = view.window()->findChild<QLineEdit*>(QStringLiteral("worldField:") + key);
    REQUIRE(field != nullptr);
    return field;
}
void Type(QLineEdit* field, const QString& value)
{
    field->setFocus();
    field->selectAll();
    QTest::keyClicks(field, value);
}
} // namespace
TEST_CASE("World demo uses World fields history save recovery and runtime data", "[editor][world][unified][world-demo]")
{
    QTemporaryDir directory;
    const auto path = WorldFile(directory);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    REQUIRE(view.Open(path));
    REQUIRE(IsWorldDemo(view.Document()));
    // World-demo Worlds render in the same bound viewport as every scene-less World.
    auto* boundViewport = view.findChild<QWidget*>(QStringLiteral("worldViewport"));
    REQUIRE(boundViewport != nullptr);
    CHECK(boundViewport->isHidden() == false);
    // Five gameplay entities and six scatter entities.
    CHECK(view.findChild<QListWidget*>(QStringLiteral("worldEntities"))->count() == 11);
    Select(view, QStringLiteral("player-start"));
    const auto original = view.Document().Draft();
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("-"));
    CHECK_FALSE(view.Save());
    CHECK(Field(view, QStringLiteral("transform/x"))->text() == QStringLiteral("-"));
    CHECK(view.Document().Draft() == original);
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("0.5"));
    REQUIRE(view.Save());
    CHECK_FALSE(view.Dirty());
    CHECK(view.Document()
              .Entity(QStringLiteral("player-start"))
              .value(QStringLiteral("components"))
              .toObject()
              .value(QStringLiteral("transform"))
              .toObject()
              .value(QStringLiteral("x")) == QJsonValue(.5));
    ludus::world_demo::Level runtime;
    const auto saved = Read(path);
    REQUIRE(
        ludus::world_demo::ReadLevel({saved.constData(), static_cast<ludus::foundation::usize>(saved.size())}, runtime)
            .Error == ludus::world_demo::LevelError::None);
    CHECK(runtime.Entities[0].Position.X == .5F);
    WorldWorkspace reopened;
    reopened.SetProject(directory.path(), 1);
    REQUIRE(reopened.Open(path));
    CHECK(reopened.Document().Draft() == view.Document().Draft());
    view.Undo();
    CHECK(view.Document().Draft() == original);
    CHECK(view.Dirty());
    view.Redo();
    CHECK_FALSE(view.Dirty());
    Select(view, QStringLiteral("guard-a"));
    Type(Field(view, QStringLiteral("enemy/health")), QStringLiteral("7"));
    REQUIRE(view.Apply());
    CHECK(QFileInfo::exists(path + QStringLiteral(".ludus-recovery")));
    WorldWorkspace recovered;
    recovered.SetProject(directory.path(), 1);
    REQUIRE(recovered.Open(path));
    REQUIRE(recovered.Recover());
    CHECK(recovered.Document().Draft() == view.Document().Draft());
    recovered.Undo();
    CHECK_FALSE(recovered.Dirty());
    Write(path, QByteArrayLiteral("{}"));
    CHECK_FALSE(view.Save());
    CHECK(view.Dirty());
    CHECK(Read(path) == QByteArrayLiteral("{}"));
}
TEST_CASE("World demo migration preserves legacy source and refuses overwrite and malformed candidates",
          "[editor][world][unified][world-demo]")
{
    QTemporaryDir directory;
    const auto worldPath = WorldFile(directory);
    ludus::world_demo::Level level;
    const auto source = ludus::world_demo::ExampleLevel();
    REQUIRE(ludus::world_demo::ReadLevel(source, level).Error == ludus::world_demo::LevelError::None);
    ludus::foundation::core::Array<char> legacy;
    REQUIRE(ludus::world_demo::WriteLevel(level, legacy));
    const QByteArray bytes(legacy.GetData(), static_cast<ludus::foundation::int64>(legacy.GetSize()));
    const auto path = QFileInfo(worldPath).absolutePath() + QStringLiteral("/legacy.json");
    Write(path, bytes);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    REQUIRE(view.Open(path));
    CHECK(view.Path().endsWith(QStringLiteral("legacy.world.json")));
    CHECK(view.Dirty());
    REQUIRE(view.Save());
    CHECK(Read(path) == bytes);
    CHECK(QFileInfo::exists(view.Path()));
    const auto original = view.Document().Draft();
    CHECK_FALSE(view.Open(path));
    CHECK(view.Document().Draft() == original);
    const auto bad = QFileInfo(worldPath).absolutePath() + QStringLiteral("/bad.json");
    Write(bad, bytes + bytes);
    CHECK_FALSE(view.Open(bad));
    CHECK(view.Document().Draft() == original);
}
TEST_CASE("World demo actors colliders settings and duplication share one inspector and validation",
          "[editor][world][unified][world-demo]")
{
    QTemporaryDir directory;
    const auto path = WorldFile(directory);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    REQUIRE(view.Open(path));
    Select(view, QStringLiteral("floor"));
    Type(Field(view, QStringLiteral("collision/half_x")), QStringLiteral("8"));
    REQUIRE(view.Apply());
    Select(view, QStringLiteral("level-settings"));
    Type(Field(view, QStringLiteral("settings/seed")), QStringLiteral("18446744073709551615"));
    REQUIRE(view.Save());
    ludus::world_demo::Level runtime;
    QString error;
    REQUIRE(ProjectWorldDemo(view.Document(), runtime, error));
    CHECK(runtime.Seed == ~ludus::foundation::uint64{0});
    CHECK(runtime.Collision[0].HalfExtent.X == 8);
    Select(view, QStringLiteral("guard-a"));
    view.findChild<QPushButton*>(QStringLiteral("worldDuplicate"))->click();
    // Five gameplay entities, six scatter entities and the duplicate.
    CHECK(view.Document().Draft().value(QStringLiteral("entities")).toArray().size() == 12);
    REQUIRE(view.Save());
    REQUIRE(ProjectWorldDemo(view.Document(), runtime, error));
    CHECK(runtime.EntityCount == 4);
    view.Undo();
    REQUIRE(ProjectWorldDemo(view.Document(), runtime, error));
    CHECK(runtime.EntityCount == 3);
    Select(view, QStringLiteral("level-settings"));
    Type(Field(view, QStringLiteral("settings/seed")), QStringLiteral("broken"));
    REQUIRE(view.Apply());
    const auto disk = Read(path);
    CHECK_FALSE(view.Save());
    CHECK(Read(path) == disk);
    CHECK(view.Dirty());
}
TEST_CASE("World demo renders its game extraction in the shared World viewport", "[editor][world][unified][world-demo]")
{
    QTemporaryDir directory;
    const auto path = WorldFile(directory);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    RenderPanel* panel = view.findChild<RenderPanel*>(QStringLiteral("worldRenderPanel"));
    REQUIRE(panel != nullptr);
    QSignalSpy loaded(panel, &RenderPanel::Loaded);
    REQUIRE(view.Open(path));
    REQUIRE(RenderScene::GetSource(directory.path(), view.Document()) == RenderSceneSource::WorldDemo);
    // No separate authoring window: the World shows the same bound viewport as every scene-less World.
    CHECK(view.findChild<QPushButton*>(QStringLiteral("worldPreview")) == nullptr);
    CHECK(&view.GetRenderPreview() == &panel->Preview());
    CHECK(panel->isHidden() == false);

    RenderScene scene;
    QString error;
    REQUIRE(RenderScene::Load(directory.path(), view.Document(), scene, error));
    INFO(error.toStdString());
    REQUIRE(scene.Meshes.size() == 1);
    CHECK(scene.Lit == false);
    REQUIRE(scene.Cameras.size() == 1);
    CHECK(scene.DefaultCamera == 0);
    CHECK(scene.Cameras[0].Sample.Lens.Projection == ludus::gameplay::camera::CameraProjection::OrthographicFinite);
    ludus::world_demo::Level level;
    REQUIRE(ProjectWorldDemo(view.Document(), level, error));
    // Collision boxes, scatter placements and every actor; the room authors scatter.
    CHECK(scene.Objects.size() > static_cast<qsizetype>(level.CollisionCount + level.EntityCount));
    const QString player = view.Document().FindEntityKey(QStringLiteral("player-start"));
    const auto findPlayer = [&player](const RenderScene& candidate) -> const PreviewObject* {
        for (const PreviewObject& object : candidate.Objects)
        {
            if (object.Id == player)
            {
                return &object;
            }
        }
        return nullptr;
    };
    const PreviewObject* playerObject = findPlayer(scene);
    REQUIRE(playerObject != nullptr);
    // The player's own colour and authored position, drawn by the game's builders.
    CHECK(playerObject->Tint.X == 1);
    CHECK(playerObject->Tint.W == 1);
    ludus::foundation::float32 authoredX = 0;
    for (ludus::foundation::usize i = 0; i < level.EntityCount; ++i)
    {
        if (level.Entities[i].Id.View() == "player-start")
        {
            authoredX = level.Entities[i].Position.X;
        }
    }
    CHECK(playerObject->Transform.Translation.X == authoredX);

    // The panel publishes the same extraction and opens through the game camera.
    REQUIRE((loaded.isEmpty() == false || loaded.wait(10000)));
    CHECK(loaded.last().at(0).toBool());
    CHECK(panel->Preview().Scene().Identity == scene.Identity);
    QComboBox* cameras = panel->findChild<QComboBox*>(QStringLiteral("worldPreviewCamera"));
    REQUIRE(cameras != nullptr);
    CHECK(cameras->currentData().toString() == QStringLiteral("world-demo.camera"));
    CHECK(panel->Preview().GetShading() == PreviewShading::Albedo);

    // An applied edit re-extracts; an invalid pending field changes nothing.
    Select(view, player);
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("-3"));
    REQUIRE(view.Apply());
    RenderScene edited;
    REQUIRE(RenderScene::Load(directory.path(), view.Document(), edited, error));
    const PreviewObject* movedPlayer = findPlayer(edited);
    REQUIRE(movedPlayer != nullptr);
    CHECK(movedPlayer->Transform.Translation.X == -3);
    CHECK(edited.Identity != scene.Identity);
    const QJsonObject applied = view.Document().Draft();
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("-"));
    CHECK_FALSE(view.Apply());
    CHECK(view.Document().Draft() == applied);
}
TEST_CASE("World demo entity IDs keep the legacy prefix and fail without entropy",
          "[editor][world][world-demo][identity]")
{
    namespace fixture = ludus::editor::tests;
    QTemporaryDir directory;
    const QString path = WorldFile(directory);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    REQUIRE(view.Open(path));
    REQUIRE(IsWorldDemo(view.Document()));
    WorldDocument candidate = view.Document();
    const QJsonObject before = candidate.Draft();
    QString error;
    QString id = QStringLiteral("unchanged");
    CHECK_FALSE(
        candidate.DuplicateEntity(fixture::MakeUnavailableEntropySource(), error, QStringLiteral("player-start"), id));
    CHECK(error == DescribeEntropyUnavailable());
    CHECK(id == QStringLiteral("unchanged"));
    CHECK(candidate.Draft() == before);
    CHECK_FALSE(candidate.CanUndo());
    REQUIRE(candidate.DuplicateEntity(fixture::MakeFixedEntropySource(), error, QStringLiteral("guard-a"), id));
    // `entity-` and the first 24 characters of the UUID text fit the sample's 32-byte name field.
    CHECK(id == QStringLiteral("entity-") + QLatin1String(fixture::FIXED_UUID_TEXT).left(24));
}
