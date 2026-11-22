#include "filesystem_fixture.h"

#include "design_tokens.h"
#include "entropy_fixture.h"
#include "internal/builtin_world.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/editor_identity.h"
#include "internal/inspector_section.h"
#include "internal/level_workspace.h"
#include "internal/main_window.h"
#include "internal/material_inspection.h"
#include "internal/preview_lighting.h"
#include "internal/preview_viewport.h"
#include "internal/render_graph_preview.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/renderer_workspace.h"
#include "internal/selection_outline_request.h"
#include "internal/tab_modified.h"
#include "internal/viewport_object_interaction.h"
#include "internal/world_celestial_light.h"
#include "internal/world_document.h"
#include "internal/world_earth_location.h"
#include "internal/world_earth_time_field.h"
#include "internal/world_earth_time_zone.h"
#include "internal/world_entity_search.h"
#include "internal/world_lighting_coverage.h"
#include "internal/world_lights_panel.h"
#include "internal/world_material_creation.h"
#include "internal/world_probe_volume.h"
#include "internal/world_surface_bake.h"
#include "internal/world_workspace.h"
#include "mouse_fixture.h"
#include "scene_fixture.h"
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPointF>
#include <cmath>
#include <ludus/foundation/base/compiler.h>
#include <ludus/gameplay/world/celestial_transform.hpp>

#include "internal/workspace_style.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLineF>
#include <QList>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QQuickWindow>
#include <QRect>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/gameplay/camera/camera.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/platform/entropy.h>
#include <ludus/runtime/resources/scene.h>

using namespace ludus::editor;
namespace
{
QByteArray Read(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}
QByteArray Schema()
{
    return Read(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/examples/live-edit-game/authoring/world.schema.json"));
}
QByteArray Starter()
{
    return Read(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/examples/live-edit-game/authoring/starter.world.json"));
}
QString Fixture(QTemporaryDir& dir)
{
    REQUIRE(dir.isValid());
    REQUIRE(QDir(QFileInfo(dir.path()).canonicalFilePath()).mkdir(QStringLiteral("authoring")));
    Write(QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("authoring/world.schema.json")),
          Schema());
    const auto path =
        QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("authoring/starter.world.json"));
    Write(path, Starter());
    return path;
}
WorldDocument Document()
{
    WorldDocument document;
    QString error;
    REQUIRE(document.LoadSchema(Schema(), error));
    REQUIRE(document.Load(Starter(), error));
    return document;
}
QJsonObject Property(const QString& key)
{
    const auto document = Document();
    const auto component = key.section(QLatin1Char('/'), 0, 0);
    const auto id = key.section(QLatin1Char('/'), 1, 1);
    for (const auto value : document.Component(component).value(QStringLiteral("properties")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == id)
        {
            return value.toObject();
        }
    }
    FAIL("Property fixture absent");
    return {};
}
QLineEdit* Field(WorldWorkspace& view, const QString& key)
{
    // In the editor window the entity inspector is the Properties tool window.
    QLineEdit* field = view.window()->findChild<QLineEdit*>(QStringLiteral("worldField:") + key);
    REQUIRE(field != nullptr);
    return field;
}
void Type(QLineEdit* field, const QString& text)
{
    field->setFocus();
    field->selectAll();
    QTest::keyClicks(field, text);
}
} // namespace
TEST_CASE("World authoring requires a valid project-owned schema and preserves failed loads", "[editor][world]")
{
    WorldDocument document;
    QString error;
    CHECK_FALSE(document.New(error));
    CHECK_FALSE(document.Load(QByteArrayLiteral("{}"), error));
    CHECK_FALSE(document.Loaded());
    CHECK_FALSE(document.LoadSchema(QByteArrayLiteral("{\"version\":1,\"version\":1}"), error));
    REQUIRE(document.LoadSchema(Schema(), error));
    REQUIRE(document.Load(Starter(), error));
    const auto before = document.Draft();
    CHECK_FALSE(document.Load(Starter().replace("\"version\": 1", "\"version\": 1, \"version\": 1"), error));
    CHECK(document.Draft() == before);
    auto unknown = before;
    unknown.insert(QStringLiteral("schema"), QStringLiteral("another-game"));
    CHECK_FALSE(document.Load(QJsonDocument(unknown).toJson(), error));
    CHECK(document.Draft() == before);
    CHECK_FALSE(document.Dirty());
    CHECK_FALSE(document.LoadSchema(Schema(), error));
}
TEST_CASE("World entity and component transactions retain stable identity and undo whole edits", "[editor][world]")
{
    auto document = Document();
    QString error, id = QStringLiteral("unchanged");
    CHECK_FALSE(document.CreateEntity(ludus::platform::GetSystemEntropySource(), error, QString(), id));
    CHECK(id == QStringLiteral("unchanged"));
    CHECK_FALSE(document.CanUndo());
    REQUIRE(document.CreateEntity(ludus::platform::GetSystemEntropySource(),
                                  error,
                                  QStringLiteral("灯光 — Éclairage"),
                                  id));
    const auto original = id;
    REQUIRE(document.Attach(id, error, QStringLiteral("presentation")));
    CHECK_FALSE(document.Attach(id, error, QStringLiteral("presentation")));
    CHECK_FALSE(document.Detach(id, error, QStringLiteral("transform")));
    REQUIRE(document.RenameEntity(id, error, QStringLiteral("Key light")));
    CHECK(document.Entity(id).value(QStringLiteral("id")) == QJsonValue(original));
    REQUIRE(document.Undo());
    CHECK(document.Entity(id).value(QStringLiteral("name")).toString() == QStringLiteral("灯光 — Éclairage"));
    REQUIRE(document.Redo());
    auto props = document.Entity(id).value(QStringLiteral("components")).toObject();
    auto presentation = props.value(QStringLiteral("presentation")).toObject();
    presentation.insert(QStringLiteral("asset"), QStringLiteral("not.declared"));
    props.insert(QStringLiteral("presentation"), presentation);
    const auto before = document.Draft();
    CHECK_FALSE(document.Edit(id, props, QStringLiteral("Changed"), error));
    CHECK(document.Draft() == before);
    REQUIRE(document.Detach(id, error, QStringLiteral("presentation")));
    REQUIRE(document.Undo());
    CHECK(document.Entity(id).value(QStringLiteral("components")).toObject().contains(QStringLiteral("presentation")));
    CHECK_FALSE(document.RemoveEntity(QStringLiteral("starter.cube"), error));
    CHECK(error.contains(QStringLiteral("Target entity")));
    REQUIRE(document.RemoveEntity(id, error));
    REQUIRE(document.Undo());
    CHECK_FALSE(document.Entity(id).isEmpty());
}
TEST_CASE("World entity IDs come from FoundationIdentity and fail without entropy", "[editor][world][identity]")
{
    namespace fixture = ludus::editor::tests;
    WorldDocument document = Document();
    const QJsonObject before = document.Draft();
    QString error;
    QString id = QStringLiteral("unchanged");
    CHECK_FALSE(document.CreateEntity(fixture::MakeUnavailableEntropySource(), error, QStringLiteral("Entity"), id));
    CHECK(error == DescribeEntropyUnavailable());
    CHECK(id == QStringLiteral("unchanged"));
    CHECK(document.Draft() == before);
    CHECK_FALSE(document.CanUndo());
    error.clear();
    CHECK_FALSE(
        document.DuplicateEntity(fixture::MakeUnavailableEntropySource(), error, QStringLiteral("starter.target"), id));
    CHECK(error == DescribeEntropyUnavailable());
    CHECK(id == QStringLiteral("unchanged"));
    CHECK(document.Draft() == before);
    CHECK_FALSE(document.CanUndo());
    // A null source is unavailable too; there is no fallback.
    CHECK_FALSE(
        document.CreateEntity(ludus::foundation::identity::EntropySource{}, error, QStringLiteral("Entity"), id));
    CHECK(id == QStringLiteral("unchanged"));
    CHECK(document.Draft() == before);

    REQUIRE(document.CreateEntity(fixture::MakeFixedEntropySource(), error, QStringLiteral("Entity"), id));
    CHECK(id == QLatin1String(fixture::FIXED_UUID_TEXT));
    QString copy;
    REQUIRE(document.DuplicateEntity(ludus::platform::GetSystemEntropySource(), error, id, copy));
    CHECK(copy.size() == 36);
    CHECK(copy != id);
    CHECK(copy == copy.toLower());
    CHECK(copy.at(14) == QLatin1Char('4'));
}
TEST_CASE("World scene import reports unavailable entropy before changing the World", "[editor][world][identity]")
{
    QTemporaryDir directory;
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    const QString path =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("main.world.json"));
    REQUIRE(workspace.Create(path));
    REQUIRE(workspace.Document().SceneLevel().isEmpty());
    const QJsonObject before = workspace.Document().Draft();
    workspace.SetEntropySource(ludus::editor::tests::MakeUnavailableEntropySource());
    CHECK_FALSE(workspace.ImportScene(directory.path()));
    CHECK(workspace.findChild<QLabel*>(QStringLiteral("worldStatus"))->text() == DescribeEntropyUnavailable());
    CHECK(workspace.Document().Draft() == before);
    LevelWorkspace* scene = workspace.findChild<LevelWorkspace*>();
    REQUIRE(scene != nullptr);
    CHECK_FALSE(scene->Busy());
    QAction* addOrNull = workspace.findChild<QAction*>(QStringLiteral("worldAdd"));
    REQUIRE(addOrNull != nullptr);
    REQUIRE(addOrNull->isEnabled());
    addOrNull->trigger();
    CHECK(workspace.findChild<QLabel*>(QStringLiteral("worldStatus"))->text() == DescribeEntropyUnavailable());
    CHECK(workspace.Document().Draft() == before);
    CHECK_FALSE(workspace.CanUndo());
}
TEST_CASE("World duplication remaps self references and preserves other references", "[editor][world]")
{
    auto document = Document();
    QString error;
    auto entity = document.Entity(QStringLiteral("starter.target"));
    auto components = entity.value(QStringLiteral("components")).toObject();
    auto behavior = components.value(QStringLiteral("behavior")).toObject();
    behavior.insert(QStringLiteral("target"), QStringLiteral("starter.target"));
    components.insert(QStringLiteral("behavior"), behavior);
    REQUIRE(document.Edit(QStringLiteral("starter.target"), components, QStringLiteral("Self"), error));
    QString copy;
    REQUIRE(document.DuplicateEntity(ludus::platform::GetSystemEntropySource(),
                                     error,
                                     QStringLiteral("starter.target"),
                                     copy));
    CHECK(copy != QStringLiteral("starter.target"));
    CHECK(document.Entity(copy)
              .value(QStringLiteral("components"))
              .toObject()
              .value(QStringLiteral("behavior"))
              .toObject()
              .value(QStringLiteral("target")) == QJsonValue(copy));
    REQUIRE(document.Undo());
    REQUIRE(document.Undo());
    REQUIRE(document.DuplicateEntity(ludus::platform::GetSystemEntropySource(),
                                     error,
                                     QStringLiteral("starter.target"),
                                     copy));
    CHECK(document.Entity(copy)
              .value(QStringLiteral("components"))
              .toObject()
              .value(QStringLiteral("behavior"))
              .toObject()
              .value(QStringLiteral("target")) == QJsonValue(QStringLiteral("starter.cube")));
}
TEST_CASE("World typed conversion rejects partial nonfinite fractional and unknown input without changing output",
          "[editor][world]")
{
    QString error;
    QJsonValue result = QStringLiteral("preserved");
    const auto number = Property(QStringLiteral("experiment/roughness"));
    for (const auto& bad : {QStringLiteral("-"),
                            QStringLiteral("nan"),
                            QStringLiteral("inf"),
                            QStringLiteral("1.2"),
                            QStringLiteral("0.5tail")})
    {
        CHECK_FALSE(WorldDocument::ParseValue(number, bad, result, error));
        CHECK(result == QJsonValue(QStringLiteral("preserved")));
    }
    REQUIRE(WorldDocument::ParseValue(number, QStringLiteral("0.25"), result, error));
    CHECK(result == QJsonValue(0.25));
    const auto integer = Property(QStringLiteral("experiment/samples"));
    CHECK_FALSE(WorldDocument::ParseValue(integer, QStringLiteral("2.5"), result, error));
    CHECK_FALSE(WorldDocument::ParseValue(integer, QStringLiteral("2147483648"), result, error));
    CHECK_FALSE(WorldDocument::ParseValue(integer, QStringLiteral("0"), result, error));
    REQUIRE(WorldDocument::ParseValue(integer, QStringLiteral("4096"), result, error));
    CHECK(result == QJsonValue(4096));
    CHECK_FALSE(WorldDocument::ParseValue(Property(QStringLiteral("presentation/style")),
                                          QStringLiteral("missing"),
                                          result,
                                          error));
    auto document = Document();
    const auto before = document.Draft();
    auto malformed = before;
    auto entities = malformed.value(QStringLiteral("entities")).toArray();
    entities.append(entities[0]);
    malformed.insert(QStringLiteral("entities"), entities);
    CHECK_FALSE(document.Restore(malformed, error, "Restore World"));
    CHECK(document.Draft() == before);
}
TEST_CASE("World inspector preserves invalid text disables selection and saves one atomic edit", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    auto* x = Field(view, QStringLiteral("transform/x"));
    auto* name = view.findChild<QLineEdit*>(QStringLiteral("worldEntityName"));
    REQUIRE(name != nullptr);
    const auto before = view.Document().Draft();
    Type(x, QStringLiteral("-"));
    Type(name, QStringLiteral("Artist cube"));
    CHECK(view.Dirty());
    CHECK_FALSE(listOrNull->isEnabled());
    CHECK_FALSE(view.CanUndo());
    CHECK_FALSE(view.Save());
    CHECK(view.Document().Draft() == before);
    CHECK(x->text() == QStringLiteral("-"));
    CHECK(name->text() == QStringLiteral("Artist cube"));
    Type(x, QStringLiteral("0.5"));
    REQUIRE(view.Save());
    CHECK_FALSE(view.Dirty());
    CHECK(listOrNull->topLevelItem(0)->text(0) == QStringLiteral("Artist cube"));
    CHECK(listOrNull->isEnabled());
    CHECK(view.Document().Entity(QStringLiteral("starter.cube")).value(QStringLiteral("name")) ==
          QJsonValue(QStringLiteral("Artist cube")));
    WorldWorkspace reopened;
    reopened.SetProject(dir.path(), 1);
    REQUIRE(reopened.Open(path));
    CHECK(reopened.Document().Draft() == view.Document().Draft());
    view.Undo();
    CHECK(view.Dirty());
    view.Redo();
    CHECK_FALSE(view.Dirty());
}
TEST_CASE("World saves refuse changed files schemas locks and symlink paths while retaining edits", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("3"));
    REQUIRE(view.Apply());
    const auto draft = view.Document().Draft();
    Write(path, Starter() + ' ');
    CHECK_FALSE(view.Save());
    CHECK(view.Document().Draft() == draft);
    CHECK(Read(path) == Starter() + ' ');
    Write(path, Starter());
    const auto schemaPath =
        QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("authoring/world.schema.json"));
    Write(schemaPath, Schema() + ' ');
    CHECK_FALSE(view.Save());
    Write(schemaPath, Schema());
    QLockFile lock(path + QStringLiteral(".ludus-editor-lock"));
    REQUIRE(lock.tryLock());
    CHECK_FALSE(view.Save());
    lock.unlock();
    REQUIRE(view.Save());
    REQUIRE(lock.tryLock());
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("5"));
    REQUIRE(view.Apply());
    CHECK(view.findChild<QLabel*>(QStringLiteral("worldStatus"))
              ->text()
              .contains(QStringLiteral("Recovery could not be written")));
    lock.unlock();
    REQUIRE(view.Save());

    const auto alias = QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("alias.world.json"));
    REQUIRE(CreateTestSymbolicLink(path, alias));
    WorldWorkspace second;
    second.SetProject(dir.path(), 2);
    CHECK_FALSE(second.Open(alias));
    CHECK_FALSE(second.Create(path));
    QTemporaryDir outside;
    const auto other =
        QDir(QFileInfo(outside.path()).canonicalFilePath()).filePath(QStringLiteral("outside.world.json"));
    Write(other, Starter());
    CHECK_FALSE(second.Open(other));
}
TEST_CASE("World creation publishes only on Save and recovery is explicit bound and undoable", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("7"));
    REQUIRE(view.Apply());
    CHECK(Read(path) == Starter());
    const auto recovery = path + QStringLiteral(".ludus-recovery");
    REQUIRE(QFile::exists(recovery));
    WorldWorkspace reopened;
    reopened.SetProject(dir.path(), 1);
    REQUIRE(reopened.Open(path));
    CHECK_FALSE(reopened.Dirty());
    REQUIRE(reopened.Recover());
    CHECK(reopened.Dirty());
    CHECK(reopened.Document().Draft() == view.Document().Draft());
    reopened.Undo();
    CHECK_FALSE(reopened.Dirty());
    reopened.Redo();
    REQUIRE(reopened.Save());
    CHECK_FALSE(QFile::exists(recovery));
    WorldWorkspace fresh;
    fresh.SetProject(dir.path(), 2);
    const auto newPath = QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("new.world.json"));
    REQUIRE(fresh.Create(newPath));
    CHECK(fresh.Dirty());
    CHECK_FALSE(QFile::exists(newPath));
    fresh.findChild<QAction*>(QStringLiteral("worldAdd"))->trigger();
    REQUIRE(fresh.Save());
    CHECK(QFile::exists(newPath));
    CHECK_FALSE(fresh.Dirty());
}
TEST_CASE("World schema and document resource limits are enforced", "[editor][world]")
{
    auto document = Document();
    QString error;
    QString id;
    REQUIRE(document.CreateEntity(ludus::platform::GetSystemEntropySource(), error, QStringLiteral("Entity"), id));
    QJsonObject bounded = document.Draft();
    QJsonArray entities = bounded.value(QStringLiteral("entities")).toArray();
    const QJsonObject created = document.Entity(id);
    while (entities.size() < WorldDocument::MAX_ENTITIES)
    {
        QJsonObject entity = created;
        entity.insert(QStringLiteral("id"), QStringLiteral("capacity.") + QString::number(entities.size()));
        entities.append(entity);
    }
    bounded.insert(QStringLiteral("entities"), entities);
    REQUIRE(document.Restore(bounded, error, "Restore World") == true);
    const auto before = document.Draft();
    CHECK_FALSE(
        document.CreateEntity(ludus::platform::GetSystemEntropySource(), error, QStringLiteral("Overflow"), id));
    CHECK(document.Draft() == before);
    CHECK_FALSE(document.Load(QByteArray(WorldDocument::MAX_BYTES + 1, ' '), error));
    CHECK(document.Draft() == before);
    WorldDocument fresh;
    CHECK_FALSE(fresh.LoadSchema(QByteArray(65537, ' '), error));
}
TEST_CASE("World context menu creates entities in the viewport and edits the hierarchy selection",
          "[editor][world][context-menu]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    WorldWorkspace workspace;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("context.world.json"))) == true);
    workspace.resize(1100, 720);
    workspace.show();
    QApplication::processEvents();
    PreviewViewport* viewportOrNull = workspace.findChild<PreviewViewport*>(QStringLiteral("worldViewport"));
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("worldEntityContextMenu"));
    REQUIRE(viewportOrNull != nullptr);
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(menuOrNull != nullptr);
    QAction* addOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("worldAdd"));
    QAction* duplicateOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("worldDuplicate"));
    QAction* deleteOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("worldDelete"));
    REQUIRE(addOrNull != nullptr);
    REQUIRE(duplicateOrNull != nullptr);
    REQUIRE(deleteOrNull != nullptr);
    const QJsonObject navigation = workspace.GetRenderPreview().Navigation();
    const QPoint viewportPoint = viewportOrNull->rect().center();
    QTest::mouseClick(viewportOrNull, Qt::RightButton, Qt::NoModifier, viewportPoint);
    REQUIRE(menuOrNull->isVisible() == true);
    CHECK(workspace.GetRenderPreview().Navigation() == navigation);
    CHECK(duplicateOrNull->isEnabled() == false);
    CHECK(deleteOrNull->isEnabled() == false);
    menuOrNull->setActiveAction(addOrNull);
    QTest::keyClick(menuOrNull, Qt::Key_Return);
    CHECK(entitiesOrNull->topLevelItemCount() == 1);

    addOrNull->trigger();
    REQUIRE(entitiesOrNull->topLevelItemCount() == 2);
    const QPoint firstRow = entitiesOrNull->visualItemRect(entitiesOrNull->topLevelItem(0)).center();
    QContextMenuEvent rowEvent(QContextMenuEvent::Mouse, firstRow, entitiesOrNull->viewport()->mapToGlobal(firstRow));
    QApplication::sendEvent(entitiesOrNull->viewport(), &rowEvent);
    REQUIRE(menuOrNull->isVisible() == true);
    CHECK(entitiesOrNull->topLevelItem(0)->isSelected() == true);
    CHECK(entitiesOrNull->selectedItems().size() == 1);
    CHECK(duplicateOrNull->isEnabled() == true);
    CHECK(deleteOrNull->isEnabled() == true);
    menuOrNull->close();

    entitiesOrNull->selectAll();
    const QPoint selectedRow = entitiesOrNull->visualItemRect(entitiesOrNull->topLevelItem(0)).center();
    QTest::mouseClick(entitiesOrNull->viewport(), Qt::RightButton, Qt::NoModifier, selectedRow);
    QContextMenuEvent groupEvent(QContextMenuEvent::Mouse,
                                 selectedRow,
                                 entitiesOrNull->viewport()->mapToGlobal(selectedRow));
    QApplication::sendEvent(entitiesOrNull->viewport(), &groupEvent);
    REQUIRE(menuOrNull->isVisible() == true);
    CHECK(entitiesOrNull->selectedItems().size() == 2);
    menuOrNull->close();
    duplicateOrNull->trigger();
    CHECK(entitiesOrNull->topLevelItemCount() == 4);
    workspace.Undo();
    CHECK(entitiesOrNull->topLevelItemCount() == 2);

    QAction* cubeOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateCube"));
    REQUIRE(cubeOrNull != nullptr);
    cubeOrNull->trigger();
    RenderPreview& preview = workspace.GetRenderPreview();
    REQUIRE(QTest::qWaitFor([&preview]() { return preview.Scene().Objects.size() == 1; }) == true);
    const QString cubeId = preview.Scene().Objects.first().Id;
    preview.FrameAll();
    entitiesOrNull->clearSelection();
    // Offscreen has no presented GPU image. Enable the input proxy to exercise its picking coordinates.
    viewportOrNull->GetObjectInteraction().SetActive(true);
    REQUIRE(viewportOrNull->layout()->count() >= 2);
    const QLayout* viewportLayout = viewportOrNull->layout();
    const QPoint frameCenter = viewportLayout->itemAt(viewportLayout->count() - 1)->geometry().center();
    QTest::mouseClick(viewportOrNull, Qt::RightButton, Qt::NoModifier, frameCenter);
    REQUIRE(menuOrNull->isVisible() == true);
    REQUIRE(entitiesOrNull->selectedItems().size() == 1);
    CHECK(entitiesOrNull->selectedItems().first()->data(0, Qt::UserRole).toString() == cubeId);
    menuOrNull->close();
}

TEST_CASE("Double-clicking a World entity frames it from the viewport or hierarchy", "[editor][world][framing]")
{
    using namespace ludus::foundation;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("framing.world.json"))) == true);
    workspace.resize(1100, 720);
    workspace.show();
    QApplication::processEvents();

    QAction* cubeOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateCube"));
    PreviewViewport* viewportOrNull = workspace.findChild<PreviewViewport*>(QStringLiteral("worldViewport"));
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(cubeOrNull != nullptr);
    REQUIRE(viewportOrNull != nullptr);
    REQUIRE(entitiesOrNull != nullptr);
    cubeOrNull->trigger();
    RenderPreview& preview = workspace.GetRenderPreview();
    REQUIRE(QTest::qWaitFor([&preview]() { return preview.Scene().Objects.size() == 1; }) == true);
    const QString cubeId = preview.Scene().Objects.first().Id;
    math::Aabb3 boundsWorld;
    REQUIRE(preview.Scene().TryGetBounds(cubeId, boundsWorld) == true);
    const math::Vector3 centerWorld = (boundsWorld.Min + boundsWorld.Max) * 0.5F;
    const QJsonObject draft = workspace.Document().Draft();
    preview.FrameAll();
    preview.SetFocusDistance(preview.GetFocusDistance() * 3);
    const float32 distanceBefore = preview.GetFocusDistance();

    SECTION("viewport picks the entity and ignores the background")
    {
        const QLayout* layoutOrNull = viewportOrNull->layout();
        REQUIRE(layoutOrNull != nullptr);
        REQUIRE(layoutOrNull->count() >= 2);
        const QRect framePixel = layoutOrNull->itemAt(layoutOrNull->count() - 1)->geometry();
        // Test the input proxy with the extracted scene, independent of GPU presentation.
        viewportOrNull->GetObjectInteraction().SetActive(true);
        viewportOrNull->GetObjectInteraction().SetTool(ViewportObjectInteraction::Tool::Select);
        entitiesOrNull->clearSelection();
        preview.Pan(8, 0, static_cast<float32>(framePixel.height()));
        const QJsonObject navigation = preview.Navigation();
        tests::DoubleClick(viewportOrNull, Qt::LeftButton, Qt::NoModifier, framePixel.topLeft() + QPoint(4, 4));
        CHECK(preview.Navigation() == navigation);
        tests::DoubleClick(viewportOrNull, Qt::LeftButton, Qt::AltModifier, framePixel.center());
        CHECK(preview.Navigation() == navigation);
        QTest::mouseClick(viewportOrNull, Qt::LeftButton, Qt::NoModifier, framePixel.center());
        REQUIRE(entitiesOrNull->selectedItems().size() == 1);
        CHECK(entitiesOrNull->selectedItems().first()->data(0, Qt::UserRole).toString() == cubeId);
        CHECK(preview.Navigation() == navigation);
        tests::DoubleClick(viewportOrNull, Qt::LeftButton, Qt::NoModifier, framePixel.center());
    }
    SECTION("hierarchy frames the clicked row")
    {
        REQUIRE(entitiesOrNull->topLevelItemCount() == 1);
        const QPoint rowPixel = entitiesOrNull->visualItemRect(entitiesOrNull->topLevelItem(0)).center();
        QTest::mouseClick(entitiesOrNull->viewport(), Qt::LeftButton, Qt::NoModifier, rowPixel);
        tests::DoubleClick(entitiesOrNull->viewport(), Qt::LeftButton, Qt::NoModifier, rowPixel);
    }
    CHECK(preview.GetFocusDistance() < distanceBefore);
    PreviewStartView view;
    REQUIRE(preview.TryDescribeStartView(view) == true);
    CHECK(view.TargetWorld.X == Catch::Approx(centerWorld.X).margin(0.00001));
    CHECK(view.TargetWorld.Y == Catch::Approx(centerWorld.Y).margin(0.00001));
    CHECK(view.TargetWorld.Z == Catch::Approx(centerWorld.Z).margin(0.00001));
    CHECK(workspace.Document().Draft() == draft);
}

TEST_CASE("World layout remains usable in a compact desktop window", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    ApplyWorkspaceStyle(&view);
    view.resize(800, 640);
    view.show();
    QTreeWidget* entitiesOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    QTest::qWait(10);
    CHECK(view.minimumSizeHint().width() <= 800);
    for (QLineEdit* field : view.findChildren<QLineEdit*>())
    {
        // Collapsed camera settings contain a spin-box editor that has not been laid out yet.
        // Its visible geometry is checked after expanding those settings in render_authoring_tests.
        if (field->isVisible() == true)
        {
            INFO(field->objectName().toStdString());
            CHECK(field->height() >= field->minimumSizeHint().height());
        }
    }
    CHECK(view.findChild<QPushButton*>(QStringLiteral("worldApply"))->geometry().bottom() <= view.height());
    // File and inspector commands sit in compact rows beside their related controls.
    const auto isSameRow = [&view](const char* first, const char* second) {
        QWidget* firstOrNull = view.findChild<QWidget*>(QString::fromLatin1(first));
        QWidget* secondOrNull = view.findChild<QWidget*>(QString::fromLatin1(second));
        REQUIRE(firstOrNull != nullptr);
        REQUIRE(secondOrNull != nullptr);
        const QRect firstRect(firstOrNull->mapTo(&view, QPoint(0, 0)), firstOrNull->size());
        const QRect secondRect(secondOrNull->mapTo(&view, QPoint(0, 0)), secondOrNull->size());
        return firstRect.top() < secondRect.bottom() && secondRect.top() < firstRect.bottom();
    };
    CHECK(isSameRow("worldNew", "worldOpen") == true);
    CHECK(isSameRow("worldNew", "worldImportScene") == true);
    CHECK(isSameRow("worldNew", "worldPath") == true);
    // Apply and Reset stay out of the way until there is an unapplied edit.
    const QWidget* footerOrNull = view.findChild<QWidget*>(QStringLiteral("worldCommitFooter"));
    REQUIRE(footerOrNull != nullptr);
    CHECK(footerOrNull->isVisible() == false);
    // Add Component is one menu button under the entity's name and identifier, at its natural width.
    CHECK(view.findChild<QWidget*>(QStringLiteral("worldComponentChoice")) == nullptr);
    const QWidget* attachOrNull = view.findChild<QWidget*>(QStringLiteral("worldAttach"));
    REQUIRE(attachOrNull != nullptr);
    if (attachOrNull->isVisible() == true)
    {
        CHECK(attachOrNull->width() >= attachOrNull->sizeHint().width());
        const QWidget* identifierOrNull = view.findChild<QWidget*>(QStringLiteral("worldEntityIdentifier"));
        REQUIRE(identifierOrNull != nullptr);
        CHECK(attachOrNull->mapTo(&view, QPoint(0, 0)).y() >
              identifierOrNull->mapTo(&view, QPoint(0, 0)).y() + identifierOrNull->height() - 1);
    }
    // The viewport's commands are one menu on the header row, not a row of their own.
    CHECK(isSameRow("worldViewportMenu", "worldPath") == true);
    CHECK(view.findChild<QPushButton*>(QStringLiteral("worldPreview3D"))->isHidden() == true);
    QLabel* pathLabel = view.findChild<QLabel*>(QStringLiteral("worldPath"));
    REQUIRE(pathLabel != nullptr);
    // The header reads as a breadcrumb; the full file name stays in the tooltip and accessible name.
    CHECK(pathLabel->text() == QStringLiteral("authoring › starter"));
    CHECK(pathLabel->accessibleName().contains(QStringLiteral("authoring/starter.world.json")) == true);
    CHECK(pathLabel->toolTip().contains(QDir::toNativeSeparators(path)) == true);
    CHECK(pathLabel->wordWrap() == false);
    // Restore Recovery appears only while a recovery file exists.
    CHECK(view.findChild<QPushButton*>(QStringLiteral("worldRecover"))->isHidden() == true);
    // The navigation hint is a tooltip button on the viewport's controls row, not a line of its own.
    QToolButton* hint = view.findChild<QToolButton*>(QStringLiteral("viewportNavigationHint"));
    REQUIRE(hint != nullptr);
    CHECK(hint->toolTip().contains(QStringLiteral("orbit")) == true);
    CHECK(isSameRow("viewportNavigationHint", "viewportShading") == true);
    // A long entity name plus persisted UUID must not widen the inspector.
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(1), 0, QItemSelectionModel::ClearAndSelect);
    auto* name = view.findChild<QLineEdit*>(QStringLiteral("worldEntityName"));
    Type(name, QString(120, QLatin1Char('T')));
    REQUIRE(view.Apply());
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(1), 0, QItemSelectionModel::ClearAndSelect);
    QTest::qWait(10);
    CHECK(view.minimumSizeHint().width() <= 800);

    if (qEnvironmentVariableIsSet("LUDUS_WORLD_SCREENSHOT"))
    {
        REQUIRE(view.grab().save(qEnvironmentVariable("LUDUS_WORLD_SCREENSHOT")));
    }
}

TEST_CASE("World failed replacement keeps pending fields and a cancelled close preserves work", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    QTreeWidget* entitiesOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    auto* field = Field(view, QStringLiteral("transform/x"));
    Type(field, QStringLiteral("-"));
    CHECK_FALSE(
        view.Open(QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("missing.world.json"))));
    CHECK(field->text() == QStringLiteral("-"));
    CHECK(view.Dirty());
    CHECK_FALSE(view.Create(path));
    CHECK(field->text() == QStringLiteral("-"));
    QTimer::singleShot(0, []() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(box != nullptr);
        box->done(QMessageBox::Cancel);
    });
    CHECK_FALSE(view.ConfirmDiscard());
    CHECK(view.Dirty());
    CHECK(field->text() == QStringLiteral("-"));
}

TEST_CASE("World menu Save and history own the active document and Close Project can cancel", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    ProjectDescriptor descriptor;
    descriptor.Name = QStringLiteral("World team");
    descriptor.ProviderKind = Provider::Cmake;
    descriptor.SourceDir = QStringLiteral(".");
    descriptor.Preset = QStringLiteral("linux-clang-development");
    descriptor.Target = QStringLiteral("sample");
    descriptor.RunCwd = QStringLiteral(".");
    const auto project = QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("ludus.project.json"));
    Write(project, SerializeDescriptor(descriptor));
    EditorController controller(
        {QStringLiteral("/missing/python"), QStringLiteral("/missing/editor_tool.py"), QStringLiteral("/missing")});
    MainWindow window(&controller);
    controller.OpenProject(project);
    window.show();
    QTest::qWait(100);
    auto* world = window.findChild<WorldWorkspace*>();
    REQUIRE(world != nullptr);
    REQUIRE(world->Open(path));
    world->setFocus();
    QTreeWidget* entitiesOrNull = world->findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    Type(Field(*world, QStringLiteral("transform/x")), QStringLiteral("4"));
    auto* save = window.findChild<QAction*>(QStringLiteral("document.save"));
    REQUIRE(save != nullptr);
    CHECK(save->text() == QStringLiteral("&Save World"));
    REQUIRE(save->isEnabled());
    save->trigger();
    CHECK_FALSE(world->Dirty());
    CHECK(Read(project) == SerializeDescriptor(descriptor));
    entitiesOrNull->setFocus();
    auto* undo = window.findChild<QAction*>(QStringLiteral("document.undo"));
    REQUIRE(undo != nullptr);
    undo->trigger();
    CHECK(world->Dirty());
    QTimer::singleShot(0, []() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(box != nullptr);
        box->done(QMessageBox::Cancel);
    });
    REQUIRE(QMetaObject::invokeMethod(&window, "OnCloseProject", Qt::DirectConnection));
    CHECK(controller.State().DescriptorPath == project);
    CHECK(world->Dirty());
    world->Redo();
    CHECK_FALSE(world->Dirty());
}

TEST_CASE("World tags and asset kinds allow custom schemas while rejecting the wrong resource type", "[editor][world]")
{
    auto schema = QJsonDocument::fromJson(Schema()).object();
    auto definitions = schema.value(QStringLiteral("components")).toArray();
    definitions.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("qa.marker")},
                                   {QStringLiteral("label"), QStringLiteral("QA marker")},
                                   {QStringLiteral("help"), QStringLiteral("Mark entities for a test pass.")},
                                   {QStringLiteral("required"), false},
                                   {QStringLiteral("properties"), QJsonArray{}}});
    schema.insert(QStringLiteral("components"), definitions);
    WorldDocument document;
    QString error;
    REQUIRE(document.LoadSchema(QJsonDocument(schema).toJson(), error));
    REQUIRE(document.Load(Starter(), error));
    REQUIRE(document.Attach(QStringLiteral("starter.cube"), error, QStringLiteral("qa.marker")));
    auto components = document.Entity(QStringLiteral("starter.cube")).value(QStringLiteral("components")).toObject();
    auto presentation = components.value(QStringLiteral("presentation")).toObject();
    presentation.insert(QStringLiteral("sound"), QStringLiteral("mesh.cube"));
    components.insert(QStringLiteral("presentation"), presentation);
    const auto before = document.Draft();
    CHECK_FALSE(document.Edit(QStringLiteral("starter.cube"), components, QStringLiteral("Cube"), error));
    CHECK(error.contains(QStringLiteral("audio")));
    CHECK(document.Draft() == before);
}

TEST_CASE("World recovery rejects damaged payloads and stale schemas without replacing the document", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    WorldWorkspace source;
    source.SetProject(dir.path(), 1);
    REQUIRE(source.Open(path));
    QTreeWidget* entitiesOrNull = source.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    Type(Field(source, QStringLiteral("transform/x")), QStringLiteral("1"));
    REQUIRE(source.Apply());
    const auto recovery = path + QStringLiteral(".ludus-recovery");
    const auto valid = Read(recovery);
    WorldWorkspace restored;
    restored.SetProject(dir.path(), 1);
    REQUIRE(restored.Open(path));
    const auto before = restored.Document().Draft();
    Write(recovery, valid.left(40));
    CHECK_FALSE(restored.Recover());
    CHECK(restored.Document().Draft() == before);
    Write(recovery, valid + 'x');
    CHECK_FALSE(restored.Recover());
    CHECK_FALSE(restored.Dirty());
    Write(recovery, valid);
    const auto schemaPath =
        QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("authoring/world.schema.json"));
    Write(schemaPath, Schema() + ' ');
    CHECK_FALSE(restored.Recover());
    CHECK(restored.Document().Draft() == before);
    Write(schemaPath, Schema());
    REQUIRE(restored.Recover());
    REQUIRE(restored.Dirty());
    // Never replace an applied dirty draft with recovery without a loss-of-work decision.
    CHECK_FALSE(restored.Recover());
}

namespace
{
WorldDocument UnboundDocument()
{
    auto schema = QJsonDocument::fromJson(Schema()).object();
    schema.remove(QStringLiteral("scene_transform"));
    WorldDocument document;
    QString error;
    REQUIRE(document.LoadSchema(QJsonDocument(schema).toJson(), error));
    REQUIRE(document.Load(Starter(), error));
    return document;
}
QJsonObject ImportedLevel()
{
    return QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("id"), QStringLiteral("a8517f65-8127-4bf4-99c2-e53b1a6d9700")},
        {QStringLiteral("name"), QStringLiteral("Room")},
        {QStringLiteral("profile"), QStringLiteral("resident-reference-v1")},
        {QStringLiteral("imports"), QJsonArray{}},
        {QStringLiteral("camera"),
         QJsonObject{{QStringLiteral("eye"), QJsonArray{0, 0, 3}},
                     {QStringLiteral("target"), QJsonArray{0, 0, 0}},
                     {QStringLiteral("up"), QJsonArray{0, 1, 0}},
                     {QStringLiteral("fov"), 1.0}}},
        {QStringLiteral("objects"),
         QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9700")},
                         {QStringLiteral("name"), QStringLiteral("Imported pickup")},
                         {QStringLiteral("parent"), QString()},
                         {QStringLiteral("mesh"), QStringLiteral("mesh/00000000-0000-4000-8000-000000000001")},
                         {QStringLiteral("material"), QStringLiteral("material/00000000-0000-4000-8000-000000000002")},
                         {QStringLiteral("transform"), QJsonArray{1, 0, 0, 0, 1, 0, 0, 0, 1, 2, 3, 4}}}}},
    };
}
/// ImportedLevel() with `count` distinct objects, one per cooked mesh and material slot like a
/// best-effort import such as Bistro's.
QJsonObject ImportedLevel(ludus::foundation::int32 count)
{
    QJsonObject level = ImportedLevel();
    const QJsonObject source = level.value(QStringLiteral("objects")).toArray()[0].toObject();
    QJsonArray objects;
    for (ludus::foundation::int32 i = 0; i < count; ++i)
    {
        const QString suffix = QStringLiteral("%1").arg(i, 12, 10, QLatin1Char('0'));
        QJsonObject object = source;
        object.insert(QStringLiteral("id"), QStringLiteral("c0000000-0000-4000-8000-") + suffix);
        object.insert(QStringLiteral("name"), QStringLiteral("Imported object %1").arg(i + 1));
        object.insert(QStringLiteral("mesh"), QStringLiteral("mesh/00000000-0000-4000-8000-") + suffix);
        object.insert(QStringLiteral("material"), QStringLiteral("material/00000000-0000-4000-8000-") + suffix);
        objects.append(object);
    }
    level.insert(QStringLiteral("objects"), objects);
    return level;
}
} // namespace
TEST_CASE("A World admits every object of an import-profile scene", "[editor][world][unified]")
{
    // Bistro's best-effort import cooks 136 objects, more than the resident reference profile's 128.
    const ludus::foundation::int32 importObjects =
        static_cast<ludus::foundation::int32>(ludus::content::scene_format::IMPORT_LIMITS.Objects);
    WorldDocument document = UnboundDocument();
    const ludus::foundation::int32 authored =
        static_cast<ludus::foundation::int32>(document.Draft().value(QStringLiteral("entities")).toArray().size());
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(importObjects), error) == true);
    CHECK(document.Draft().value(QStringLiteral("entities")).toArray().size() == authored + importObjects);
    CHECK(document.SceneLevel().value(QStringLiteral("objects")).toArray().size() == importObjects);
    CHECK(document.Encode().size() <= WorldDocument::MAX_BYTES);

    WorldDocument overflowing = UnboundDocument();
    const QJsonObject before = overflowing.Draft();
    CHECK(overflowing.ImportScene(ImportedLevel(WorldDocument::MAX_ENTITIES - authored + 1), error) == false);
    CHECK(error.contains(QStringLiteral("at most %1 entities").arg(WorldDocument::MAX_ENTITIES)) == true);
    CHECK(overflowing.Draft() == before);
}
TEST_CASE("Imported scene instances share entity components placement persistence and history",
          "[editor][world][unified]")
{
    auto document = UnboundDocument();
    const auto original = document.Draft();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error));
    const auto id = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9700");
    REQUIRE(document.Entity(id).value(QStringLiteral("components")).toObject().contains(QStringLiteral("transform")));
    REQUIRE(document.Attach(id, error, QStringLiteral("behavior")));
    const auto beforePlacement = document.Draft();
    auto scene = document.Entity(id).value(QStringLiteral("scene")).toObject();
    scene.insert(QStringLiteral("position"), QJsonArray{5, 6, 7});
    scene.insert(QStringLiteral("rotation"), QJsonArray{0, 0, 90});
    scene.insert(QStringLiteral("scale"), QJsonArray{2, 3, 4});
    auto components = document.Entity(id).value(QStringLiteral("components")).toObject();
    auto behavior = components.value(QStringLiteral("behavior")).toObject();
    behavior.insert(QStringLiteral("priority"), 8);
    components.insert(QStringLiteral("behavior"), behavior);
    REQUIRE(document.Edit(id, components, QStringLiteral("Pickup"), error, &scene));
    auto projected = document.SceneLevel().value(QStringLiteral("objects")).toArray()[0].toObject();
    const auto transform = projected.value(QStringLiteral("transform")).toArray();
    CHECK(transform[9] == QJsonValue(5));
    CHECK(transform[10] == QJsonValue(6));
    CHECK(transform[11] == QJsonValue(7));
    CHECK(transform[1].toDouble() > 1.99);
    CHECK(transform[3].toDouble() < -2.99);
    CHECK(transform[8] == QJsonValue(4));
    REQUIRE(document.Undo());
    CHECK(document.Draft() == beforePlacement);
    REQUIRE(document.Redo());
    WorldDocument reopened;
    auto schema = QJsonDocument::fromJson(Schema()).object();
    schema.remove(QStringLiteral("scene_transform"));
    REQUIRE(reopened.LoadSchema(QJsonDocument(schema).toJson(), error));
    REQUIRE(reopened.Load(document.Encode(), error));
    CHECK(reopened.Draft() == document.Draft());
    CHECK(reopened.SceneLevel() == document.SceneLevel());
    QString copy;
    REQUIRE(document.DuplicateEntity(ludus::platform::GetSystemEntropySource(), error, id, copy));
    CHECK(document.Entity(copy).value(QStringLiteral("scene")) == document.Entity(id).value(QStringLiteral("scene")));
    CHECK(document.SceneLevel().value(QStringLiteral("objects")).toArray().size() == 2);
    REQUIRE(document.RemoveEntity(copy, error));
    CHECK(document.SceneLevel().value(QStringLiteral("objects")).toArray().size() == 1);
    REQUIRE(document.Undo()); // delete
    REQUIRE(document.Undo()); // duplicate
    REQUIRE(document.Undo()); // placement and behavior edit
    REQUIRE(document.Undo()); // attach
    REQUIRE(document.Undo()); // import
    CHECK(document.Draft() == original);
    CHECK(document.SceneLevel().isEmpty());
}
TEST_CASE("Invalid scene placement and parent references preserve the shared document", "[editor][world][unified]")
{
    auto document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error));
    const auto before = document.Draft();
    const auto id = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9700");
    const auto entity = document.Entity(id);
    for (const auto& key : {QStringLiteral("scale"), QStringLiteral("rotation"), QStringLiteral("parent")})
    {
        auto scene = entity.value(QStringLiteral("scene")).toObject();
        scene.insert(key,
                     key == QStringLiteral("parent")  ? QJsonValue(id)
                     : key == QStringLiteral("scale") ? QJsonValue(QJsonArray{0, 1, 1})
                                                      : QJsonValue(QJsonArray{0, 0, 361}));
        CHECK_FALSE(document.Edit(id,
                                  entity.value(QStringLiteral("components")).toObject(),
                                  QStringLiteral("changed"),
                                  error,
                                  &scene));
        CHECK(document.Draft() == before);
    }
    auto imported = ImportedLevel();
    auto objects = imported.value(QStringLiteral("objects")).toArray();
    objects.append(objects[0]);
    imported.insert(QStringLiteral("objects"), objects);
    WorldDocument empty;
    auto schema = QJsonDocument::fromJson(Schema()).object();
    schema.remove(QStringLiteral("scene_transform"));
    REQUIRE(empty.LoadSchema(QJsonDocument(schema).toJson(), error));
    REQUIRE(empty.New(error));
    CHECK_FALSE(empty.ImportScene(imported, error));
    CHECK(empty.Draft().value(QStringLiteral("entities")).toArray().isEmpty());
}
TEST_CASE("The scene start view is one undoable World edit validated like the imported camera",
          "[editor][world][unified][start-view]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    const QJsonObject camera{{QStringLiteral("eye"), QJsonArray{1, 2, 3}},
                             {QStringLiteral("target"), QJsonArray{0, 0, 0}},
                             {QStringLiteral("up"), QJsonArray{0, 1, 0}},
                             {QStringLiteral("fov"), 0.8}};
    const QJsonObject empty = document.Draft();
    CHECK_FALSE(document.SetSceneStartView(camera, error));
    CHECK(error.isEmpty() == false);
    CHECK(document.Draft() == empty);
    REQUIRE(document.ImportScene(ImportedLevel(), error));
    const QJsonObject imported = document.Draft();
    REQUIRE(document.SetSceneStartView(camera, error));
    CHECK(document.SceneLevel().value(QStringLiteral("camera")).toObject() == camera);
    CHECK(document.SceneLevel().value(QStringLiteral("objects")).toArray().size() == 1);
    const QJsonObject accepted = document.Draft();
    QJsonObject degenerate = camera;
    degenerate.insert(QStringLiteral("target"), camera.value(QStringLiteral("eye")));
    QJsonObject wide = camera;
    wide.insert(QStringLiteral("fov"), 3.5);
    for (const QJsonObject& rejected : {degenerate, wide})
    {
        CHECK_FALSE(document.SetSceneStartView(rejected, error));
        CHECK(document.Draft() == accepted);
    }
    REQUIRE(document.Undo());
    CHECK(document.Draft() == imported);
    REQUIRE(document.Redo());
    CHECK(document.SceneLevel().value(QStringLiteral("camera")).toObject() == camera);
}
TEST_CASE("World provides scene authoring without a project component schema", "[editor][world][unified]")
{
    QTemporaryDir directory;
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    const auto path = QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("main.world.json"));
    REQUIRE(workspace.Create(path));
    REQUIRE(workspace.Save());
    WorldWorkspace reopened;
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path));
    CHECK(reopened.Document().Draft() == workspace.Document().Draft());
    CHECK_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("authoring/world.schema.json"))));
    const auto root = QFileInfo(directory.path()).canonicalFilePath();
    REQUIRE(QDir(root).mkdir(QStringLiteral("authoring")));
    REQUIRE(CreateTestSymbolicLink(QDir(root).filePath(QStringLiteral("missing.schema.json")),
                                   QDir(root).filePath(QStringLiteral("authoring/world.schema.json"))));
    WorldWorkspace unsafe;
    unsafe.SetProject(root, 3);
    CHECK_FALSE(unsafe.Create(QDir(root).filePath(QStringLiteral("unsafe.world.json"))));
}
TEST_CASE("Editor shows the unified World as a document beside tool windows", "[editor][world][unified]")
{
    QTemporaryDir directory;
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    EditorController controller(tools, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    DocumentWell& well = window.GetDocumentWell();
    ludus::foundation::int32 worlds = 0;
    for (QWidget* document : well.GetDocuments())
    {
        const QString title = well.GetDocumentTitle(document);
        worlds += qobject_cast<WorldWorkspace*>(document) != nullptr ? 1 : 0;
        CHECK(title != QStringLiteral("3D Levels"));
        CHECK(title != QStringLiteral("Scene"));
    }
    // One World document; its entity inspector is the Properties tool window.
    CHECK(worlds == 1);
    CHECK(window.centralWidget() == &well);
    QDockWidget* properties = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    REQUIRE(properties != nullptr);
    CHECK(properties->isAncestorOf(window.findChild<QWidget*>(QStringLiteral("worldProperties"))) == true);
}

TEST_CASE("World tab titles follow document changes without a render graph refresh", "[editor][world][tabs]")
{
    QTemporaryDir directory;
    const QString firstPath = Fixture(directory);
    const QString secondPath = QFileInfo(firstPath).dir().filePath(QStringLiteral("staircase2.world.json"));
    Write(secondPath, Starter().replace("Starter Cube", "Staircase Landing"));
    ToolingPaths tools;
    EditorController controller(tools, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    WorldWorkspace* world = window.findChild<WorldWorkspace*>();
    REQUIRE(world != nullptr);
    DocumentWell& well = window.GetDocumentWell();
    world->SetProject(directory.path(), 1);
    well.SetDocumentAvailable(world, true);
    REQUIRE(well.OpenDocument(world));
    // The well owns the actual tab strip as well as the title used when reopening it.
    QTabWidget* group = window.findChild<QTabWidget*>(QStringLiteral("documentGroup"));
    REQUIRE(group != nullptr);
    // The title stays the file name; unsaved changes show as the tab's dot and in its tooltip.
    const auto checkTitle = [&well, world, group](const QString& expected, bool isModified = false) {
        CHECK(well.GetDocumentTitle(world) == expected);
        CHECK(well.IsDocumentModified(world) == isModified);
        const ludus::foundation::int32 index = group->indexOf(world);
        REQUIRE(index >= 0);
        CHECK(group->tabText(index) == expected);
        CHECK(IsTabModified(*group->tabBar(), index) == isModified);
        CHECK(group->tabToolTip(index) ==
              (isModified == true ? expected + QStringLiteral(" (unsaved changes)") : expected));
    };

    REQUIRE(world->Open(firstPath));
    checkTitle(QStringLiteral("starter.world.json"));
    REQUIRE(world->Open(secondPath));
    CHECK(well.GetActiveDocumentOrNull() == world);
    CHECK(world->Path() == secondPath);
    CHECK(world->Document().Entity(QStringLiteral("starter.cube")).value(QStringLiteral("name")).toString() ==
          QStringLiteral("Staircase Landing"));
    checkTitle(QStringLiteral("staircase2.world.json"));
    CHECK_FALSE(world->Open(directory.filePath(QStringLiteral("missing.world.json"))));
    CHECK(world->Path() == secondPath);
    checkTitle(QStringLiteral("staircase2.world.json"));

    QAction* addOrNull = world->findChild<QAction*>(QStringLiteral("worldAdd"));
    REQUIRE(addOrNull != nullptr);
    REQUIRE(addOrNull->isEnabled());
    addOrNull->trigger();
    REQUIRE(world->Dirty());
    checkTitle(QStringLiteral("staircase2.world.json"), true);
    world->Undo();
    REQUIRE(world->Dirty() == false);
    checkTitle(QStringLiteral("staircase2.world.json"));

    well.CloseDocument(world);
    REQUIRE(world->Open(firstPath));
    CHECK(well.GetDocumentTitle(world) == QStringLiteral("starter.world.json"));
    REQUIRE(well.OpenDocument(world));
    checkTitle(QStringLiteral("starter.world.json"));
    REQUIRE(world->Create(QFileInfo(firstPath).dir().filePath(QStringLiteral("new.world.json"))));
    checkTitle(QStringLiteral("new.world.json"), true);
    REQUIRE(world->Save());
    checkTitle(QStringLiteral("new.world.json"));
    world->SetProject({}, 2);
    checkTitle(QStringLiteral("World"));
}

TEST_CASE("Renderer preview follows the viewport the World shows", "[editor][world][render-graph]")
{
    QTemporaryDir directory;
    const QString starterPath = Fixture(directory);
    QString prepareError;
    INFO(prepareError.toStdString());
    const QString external = QDir(directory.path()).filePath(QStringLiteral("external"));
    REQUIRE(ludus::editor::tests::CreateFixtureSource(external));
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(directory.path(),
                                                       QStringLiteral("scenes/room.lulevel"),
                                                       external,
                                                       prepareError));
    const QString levelPath =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("scenes/room.lulevel"));
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    EditorController controller(tools, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    WorldWorkspace* world = window.findChild<WorldWorkspace*>();
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    LevelWorkspace* level = window.findChild<LevelWorkspace*>();
    REQUIRE(world != nullptr);
    REQUIRE(renderer != nullptr);
    REQUIRE(level != nullptr);
    const auto waitUntilSettled = [world]() {
        QElapsedTimer timer;
        timer.start();
        while (world->CanSave() == false && timer.elapsed() < 15000)
        {
            QTest::qWait(10);
        }
        REQUIRE(world->CanSave() == true);
    };
    QSignalSpy changed(world, &WorldWorkspace::RenderPreviewChanged);
    world->SetProject(directory.path(), 1);

    // A schema-bound World shows its render panel, never the scene viewport.
    REQUIRE(world->Open(starterPath));
    waitUntilSettled();
    RenderPreview* const panel = &world->GetRenderPreview();
    CHECK(panel != &level->GetRenderPreview());
    CHECK(renderer->GetPreviewSourceOrNull() == panel);

    // An imported-scene World shows the level viewport; the Renderer tab follows it, and the
    // shown scene is cooked (geometry only in its decoded definition).
    const qsizetype before = changed.count();
    REQUIRE(world->Open(levelPath));
    waitUntilSettled();
    CHECK(&world->GetRenderPreview() == &level->GetRenderPreview());
    CHECK(renderer->GetPreviewSourceOrNull() == &level->GetRenderPreview());
    CHECK(changed.count() == before + 1);
    REQUIRE(level->PreviewScene().Definition != nullptr);
    CHECK(level->PreviewScene().Meshes.at(0).Positions.isEmpty() == true);

    // Returning to the schema-bound World switches back. Opening a .lulevel converts it into an
    // unsaved World; save it first so reopening does not ask to discard it.
    REQUIRE(world->Save() == true);
    waitUntilSettled();
    REQUIRE(world->Dirty() == false);
    REQUIRE(world->Open(starterPath));
    waitUntilSettled();
    CHECK(&world->GetRenderPreview() == panel);
    CHECK(renderer->GetPreviewSourceOrNull() == panel);
    CHECK(changed.count() == before + 2);
    world->StopPreview();
}

TEST_CASE("World Apply clicked while the draft preview assembles applies after that preview",
          "[editor][world][unified]")
{
    QTemporaryDir directory;
    static_cast<void>(Fixture(directory));
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    QString prepareError;
    const QString external = QDir(directory.path()).filePath(QStringLiteral("external"));
    REQUIRE(ludus::editor::tests::CreateFixtureSource(external));
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(directory.path(),
                                                       QStringLiteral("scenes/room.lulevel"),
                                                       external,
                                                       prepareError));
    INFO(prepareError.toStdString());
    const QString levelPath =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("scenes/room.lulevel"));
    WorldWorkspace workspace(tools, nullptr);
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(levelPath));
    LevelWorkspace* sceneOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(sceneOrNull != nullptr);
    constexpr ludus::foundation::int32 OPERATION_TIMEOUT_MILLISECONDS = 15000;
    REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.CanSave(); }, OPERATION_TIMEOUT_MILLISECONDS));
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    QPushButton* applyOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldApply"));
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(applyOrNull != nullptr);
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    const QJsonObject before = workspace.Document().Draft();
    Type(Field(workspace, QStringLiteral("transform/x")), QStringLiteral("0.25"));
    // Shortly after typing, the draft preview assembles the fields as a scene operation.
    REQUIRE(QTest::qWaitFor([sceneOrNull]() { return sceneOrNull->Busy(); }, OPERATION_TIMEOUT_MILLISECONDS));
    // A click there used to reach a disabled button and was lost.
    REQUIRE(applyOrNull->isEnabled());
    applyOrNull->click();
    CHECK(workspace.Document().Draft() == before);
    REQUIRE(QTest::qWaitFor([&workspace, &before]() { return workspace.Document().Draft() != before; },
                            OPERATION_TIMEOUT_MILLISECONDS));
    CHECK(Field(workspace, QStringLiteral("transform/x"))->text() == QStringLiteral("0.25"));
    REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.CanSave(); }, OPERATION_TIMEOUT_MILLISECONDS));
    CHECK(applyOrNull->isEnabled() == false);
}
TEST_CASE("Converted cooked levels edit placement and gameplay in World and survive reopening",
          "[editor][world][unified]")
{
    QTemporaryDir directory;
    const auto path = Fixture(directory);
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    QString prepareError;
    INFO(prepareError.toStdString());
    const QString external = QDir(directory.path()).filePath(QStringLiteral("external"));
    REQUIRE(ludus::editor::tests::CreateFixtureSource(external));
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(directory.path(),
                                                       QStringLiteral("scenes/room.lulevel"),
                                                       external,
                                                       prepareError));
    const auto levelPath =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("scenes/room.lulevel"));
    const auto originalLevel = Read(levelPath);
    WorldWorkspace workspace(tools, nullptr);
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(levelPath));
    const auto drain = [&workspace]() {
        QElapsedTimer timer;
        timer.start();
        while (workspace.CanSave() == false && timer.elapsed() < 15000)
        {
            QTest::qWait(10);
        }
        INFO(workspace.findChild<QLabel*>(QStringLiteral("worldStatus"))->text().toStdString());
        INFO(workspace.findChild<LevelWorkspace*>()->StatusText().toStdString());
        REQUIRE(workspace.CanSave());
    };
    drain();
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(entitiesOrNull->topLevelItemCount() == 1);
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    CHECK(workspace.findChild<QLineEdit*>(QStringLiteral("worldField:scene/position0")) == nullptr);
    // The selected instance shows the decoded material the viewport renders, read-only.
    REQUIRE(workspace.findChild<InspectorSection*>(QStringLiteral("worldMaterial")) != nullptr);
    const QLabel* modelOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldMaterial:Model"));
    REQUIRE(modelOrNull != nullptr);
    CHECK(modelOrNull->text().isEmpty() == false);
    CHECK(workspace.findChild<QLabel*>(QStringLiteral("worldMaterial:Reflectance")) != nullptr);
    Type(Field(workspace, QStringLiteral("transform/x")), QStringLiteral("0.25"));
    REQUIRE(workspace.Apply());
    drain();
    QAction* behaviorOrNull = workspace.findChild<QAction*>(QStringLiteral("worldAttach:behavior"));
    REQUIRE(behaviorOrNull != nullptr);
    behaviorOrNull->trigger();
    drain();
    Type(Field(workspace, QStringLiteral("behavior/priority")), QStringLiteral("7"));
    REQUIRE(workspace.Apply());
    drain();
    REQUIRE(workspace.Save());
    drain();
    CHECK_FALSE(workspace.Dirty());
    const auto document = workspace.Document().Draft();
    REQUIRE(document.value(QStringLiteral("entities"))
                .toArray()[0]
                .toObject()
                .value(QStringLiteral("components"))
                .toObject()
                .contains(QStringLiteral("behavior")));
    CHECK(Read(levelPath) == originalLevel);
    workspace.resize(1100, 800);
    workspace.show();
    QTest::qWait(50);
    if (qEnvironmentVariableIsSet("LUDUS_UNIFIED_WORLD_SCREENSHOT"))
    {
        REQUIRE(workspace.grab().save(qEnvironmentVariable("LUDUS_UNIFIED_WORLD_SCREENSHOT")));
    }
    workspace.StopPreview();
    WorldWorkspace reopened(tools, nullptr);
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("scenes/room.world.json"))));
    QElapsedTimer timer;
    timer.start();
    while (reopened.CanSave() == false && timer.elapsed() < 15000)
    {
        QTest::qWait(10);
    }
    REQUIRE(reopened.CanSave());
    CHECK(reopened.Document().Draft() == document);
    CHECK_FALSE(reopened.Dirty());
    reopened.StopPreview();
    for (const auto& file :
         {QStringLiteral("render.preview.json"), QStringLiteral("cube.mesh.json"), QStringLiteral("ludus.png")})
    {
        Write(QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("authoring/") + file),
              Read(QStringLiteral(":/samples/live-edit-game/authoring/") + file));
    }
    WorldDocument mixedDocument;
    QString error;
    REQUIRE(mixedDocument.LoadSchema(Schema(), error));
    REQUIRE(mixedDocument.Load(Read(QStringLiteral(":/samples/live-edit-game/authoring/rendering.world.json")), error));
    REQUIRE(mixedDocument.ImportScene(QJsonDocument::fromJson(originalLevel).object(), error));
    const auto mixedPath =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("scenes/mixed.world.json"));
    Write(mixedPath, mixedDocument.Encode());
    WorldWorkspace mixed(tools, nullptr);
    mixed.SetProject(directory.path(), 3);
    REQUIRE(mixed.Open(mixedPath));
    timer.restart();
    while (mixed.CanSave() == false && timer.elapsed() < 15000)
    {
        QTest::qWait(10);
    }
    REQUIRE(mixed.CanSave());
    auto* service = mixed.findChild<LevelWorkspace*>();
    REQUIRE(service != nullptr);
    CHECK(service->PreviewScene().Objects.size() == 2);
    QTreeWidget* mixedEntitiesOrNull = mixed.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(mixedEntitiesOrNull != nullptr);
    CHECK(mixedEntitiesOrNull->topLevelItemCount() == 4);
    CHECK(mixedEntitiesOrNull->isEnabled());
    mixedEntitiesOrNull->setCurrentItem(mixedEntitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    Type(Field(mixed, QStringLiteral("transform/x")), QStringLiteral("-1"));
    REQUIRE(mixed.Save()); // Save waits for the newly applied render snapshot.
    timer.restart();
    while (mixed.CanSave() == false && timer.elapsed() < 15000)
    {
        QTest::qWait(10);
    }
    REQUIRE(mixed.CanSave());
    CHECK_FALSE(mixed.Dirty());
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    QWidget* viewport = mixed.findChild<QWidget*>(QStringLiteral("worldSceneViewport"));
    REQUIRE(viewport != nullptr);
    CHECK_FALSE(viewport->isWindow());
#endif
    mixed.resize(1100, 800);
    mixed.show();
    QTest::qWait(50);
    if (qEnvironmentVariableIsSet("LUDUS_UNIFIED_WORLD_SCREENSHOT"))
    {
        REQUIRE(mixed.grab().save(qEnvironmentVariable("LUDUS_UNIFIED_WORLD_SCREENSHOT")));
    }
    mixed.StopPreview();
    REQUIRE(mixed.Save());
    timer.restart();
    while (mixed.CanSave() == false && timer.elapsed() < 15000)
    {
        QTest::qWait(10);
    }
    REQUIRE(mixed.CanSave());
    CHECK_FALSE(service->PreviewRunning()); // Asset verification does not need a viewport.
    CHECK_FALSE(service->Dirty());          // The service owns no second unsaved document.
    REQUIRE(mixed.Open(mixedPath));
    timer.restart();
    while (mixed.CanSave() == false && timer.elapsed() < 15000)
    {
        QTest::qWait(10);
    }
    REQUIRE(mixed.CanSave());
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    CHECK(service->PreviewRunning() == PreviewViewport::CanPresent());
#endif
    const auto second =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("scenes/second.lulevel"));
    REQUIRE(QFile::copy(levelPath, second));
    REQUIRE(mixed.Open(second)); // No hidden 3D Levels discard prompt.
    timer.restart();
    while (mixed.CanSave() == false && timer.elapsed() < 15000)
    {
        QTest::qWait(10);
    }
    REQUIRE(mixed.CanSave());
    CHECK(mixed.Document().Draft().value(QStringLiteral("entities")).toArray().size() == 1);
    CHECK_FALSE(service->Dirty());
    mixed.StopPreview();
    (void)path;
}

TEST_CASE("World selection uses native toggle range and Select All shortcuts", "[editor][world][selection]")
{
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    WorldWorkspace view;
    view.SetProject(directory.path(), 1);
    REQUIRE(view.Open(path) == true);
    view.resize(1000, 700);
    view.show();
    view.activateWindow();
    QApplication::processEvents();
    QTreeWidget* listOrNull = view.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(listOrNull != nullptr);
    REQUIRE(listOrNull->topLevelItemCount() == 2);
    listOrNull->setFocus();
    QTest::mouseClick(listOrNull->viewport(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      listOrNull->visualItemRect(listOrNull->topLevelItem(0)).center());
    CHECK(listOrNull->selectedItems().size() == 1);
    QTest::mouseClick(listOrNull->viewport(),
                      Qt::LeftButton,
                      Qt::ControlModifier,
                      listOrNull->visualItemRect(listOrNull->topLevelItem(1)).center());
    CHECK(listOrNull->selectedItems().size() == 2);
    QTest::mouseClick(listOrNull->viewport(),
                      Qt::LeftButton,
                      Qt::ControlModifier,
                      listOrNull->visualItemRect(listOrNull->topLevelItem(0)).center());
    CHECK(listOrNull->selectedItems().size() == 1);
    QTest::mouseClick(listOrNull->viewport(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      listOrNull->visualItemRect(listOrNull->topLevelItem(0)).center());
    QTest::mouseClick(listOrNull->viewport(),
                      Qt::LeftButton,
                      Qt::ShiftModifier,
                      listOrNull->visualItemRect(listOrNull->topLevelItem(1)).center());
    CHECK(listOrNull->selectedItems().size() == 2);
    QTest::keyClick(listOrNull, Qt::Key_Escape);
    CHECK(listOrNull->selectedItems().isEmpty() == true);
    QTest::keySequence(listOrNull, QKeySequence(QKeySequence::SelectAll));
    CHECK(listOrNull->selectedItems().size() == 2);
    CHECK(view.findChild<QLineEdit*>(QStringLiteral("worldEntityName")) == nullptr);
    const QJsonObject session = view.Session();
    QTest::keyClick(listOrNull, Qt::Key_Escape);
    view.RestoreSession(session);
    CHECK(listOrNull->selectedItems().size() == 2);
    listOrNull->setFocus();
    QApplication::processEvents();
    REQUIRE(listOrNull->hasFocus() == true);
    REQUIRE(view.findChild<QAction*>(QStringLiteral("worldDuplicate"))->isEnabled() == true);
    QTest::keySequence(listOrNull, QKeySequence(Qt::CTRL | Qt::Key_D));
    INFO(view.findChild<QLabel*>(QStringLiteral("worldStatus"))->text().toStdString());
    CHECK(listOrNull->topLevelItemCount() == 4);
    CHECK(listOrNull->selectedItems().size() == 2);
    view.Undo();
    CHECK(listOrNull->topLevelItemCount() == 2);
    view.Redo();
    CHECK(listOrNull->topLevelItemCount() == 4);
    listOrNull->setCurrentItem(listOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    listOrNull->setFocus();
    QTest::keyClick(listOrNull, Qt::Key_F2);
    QLineEdit* name = view.findChild<QLineEdit*>(QStringLiteral("worldEntityName"));
    REQUIRE(name != nullptr);
    CHECK(name->hasFocus() == true);
    CHECK(name->selectedText() == name->text());
    // A focused text field keeps its own Select All; it cannot select entities.
    QTest::keySequence(name, QKeySequence(QKeySequence::SelectAll));
    CHECK(listOrNull->selectedItems().size() == 1);
    listOrNull->setFocus();
    QTest::keySequence(listOrNull, QKeySequence(QKeySequence::SelectAll));
    QTimer::singleShot(0, []() {
        QMessageBox* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(confirmation != nullptr);
        confirmation->button(QMessageBox::Yes)->click();
    });
    REQUIRE(view.findChild<QAction*>(QStringLiteral("worldDelete"))->isEnabled() == true);
    QTest::keySequence(listOrNull, QKeySequence(QKeySequence::Delete));
    INFO(view.findChild<QLabel*>(QStringLiteral("worldStatus"))->text().toStdString());
    CHECK(listOrNull->topLevelItemCount() == 0);
    view.Undo();
    CHECK(listOrNull->topLevelItemCount() == 4);
}

TEST_CASE("World group operations preserve references and publish one transaction", "[editor][world][selection]")
{
    WorldDocument document = Document();
    const QJsonObject original = document.Draft();
    QString error;
    QStringList copies;
    const QStringList sources{QStringLiteral("starter.cube"), QStringLiteral("starter.target")};
    REQUIRE(document.DuplicateEntities(ludus::platform::GetSystemEntropySource(), sources, copies, error) == true);
    REQUIRE(copies.size() == 2);
    CHECK(document.Entity(copies[1])
              .value(QStringLiteral("components"))
              .toObject()
              .value(QStringLiteral("behavior"))
              .toObject()
              .value(QStringLiteral("target"))
              .toString() == copies[0]);
    REQUIRE(document.Undo() == true);
    CHECK(document.Draft() == original);
    CHECK(document.CanUndo() == false);
    REQUIRE(document.Redo() == true);
    REQUIRE(document.RemoveEntities(copies, error) == true);
    CHECK(document.Draft() == original);
    REQUIRE(document.Undo() == true);
    CHECK(document.Entity(copies[0]).isEmpty() == false);
    const QJsonObject beforeFailure = document.Draft();
    CHECK(document.RemoveEntities({QStringLiteral("starter.cube")}, error) == false);
    CHECK(document.Draft() == beforeFailure);
    CHECK(document.DuplicateEntities(ludus::editor::tests::MakeUnavailableEntropySource(), sources, copies, error) ==
          false);
    CHECK(document.Draft() == beforeFailure);
    CHECK(copies.size() == 2);
    CHECK(document.DuplicateEntities(ludus::platform::GetSystemEntropySource(),
                                     {QStringLiteral("starter.cube"), QStringLiteral("missing")},
                                     copies,
                                     error) == false);
    CHECK(document.Draft() == beforeFailure);
    REQUIRE(document.RemoveEntities(sources, error) == true);
    CHECK(document.Entity(QStringLiteral("starter.cube")).isEmpty() == true);
    CHECK(document.Entity(QStringLiteral("starter.target")).isEmpty() == true);
    REQUIRE(document.Undo() == true);
    CHECK(document.Draft() == beforeFailure);
}

TEST_CASE("World group duplication rolls back when only the first copy fits", "[editor][world][selection]")
{
    WorldDocument document = Document();
    const QJsonObject original = document.Draft();
    QJsonObject bounded = original;
    QJsonArray entities = bounded.value(QStringLiteral("entities")).toArray();
    while (entities.size() < WorldDocument::MAX_ENTITIES - 1)
    {
        QJsonObject entity = entities[0].toObject();
        entity.insert(QStringLiteral("id"), QStringLiteral("capacity.") + QString::number(entities.size()));
        entities.append(entity);
    }
    bounded.insert(QStringLiteral("entities"), entities);
    QString error;
    REQUIRE(document.Restore(bounded, error, "Restore World") == true);
    const QStringList unchanged{QStringLiteral("unchanged")};
    QStringList copies = unchanged;
    CHECK(document.DuplicateEntities(ludus::platform::GetSystemEntropySource(),
                                     {QStringLiteral("starter.cube"), QStringLiteral("starter.target")},
                                     copies,
                                     error) == false);
    CHECK(copies == unchanged);
    CHECK(document.Draft() == bounded);
    REQUIRE(document.Undo() == true);
    CHECK(document.Draft() == original);
    CHECK(document.CanUndo() == false);
}
namespace
{
/// Restores the editor's saved Play mode when a test that changes it ends.
class PlayModePreference final
{
public:
    PlayModePreference() : mSaved(QSettings().value(GetKey()))
    {
        QSettings().remove(GetKey());
    }
    ~PlayModePreference()
    {
        if (mSaved.isValid() == true)
        {
            QSettings().setValue(GetKey(), mSaved);
            return;
        }
        QSettings().remove(GetKey());
    }
    PlayModePreference(const PlayModePreference&) = delete;
    PlayModePreference& operator=(const PlayModePreference&) = delete;
    [[nodiscard]] LUDUS_INLINE static QString GetKey()
    {
        return QStringLiteral("play/mode");
    }

private:
    QVariant mSaved;
};
} // namespace
TEST_CASE("Play mode is a remembered editor preference that defaults to the viewport", "[editor][world][play]")
{
    const PlayModePreference preference;
    WorldWorkspace first;
    CHECK(first.GetPlayMode() == PlayMode::InViewport);
    QComboBox* box = first.findChild<QComboBox*>(QStringLiteral("worldPlayMode"));
    REQUIRE(box != nullptr);
    CHECK(box->currentData().toInt() == static_cast<int>(PlayMode::InViewport));

    // Choosing In separate window saves it for every later World view, not in project data.
    const int separate = box->findData(static_cast<int>(PlayMode::SeparateWindow));
    REQUIRE(separate >= 0);
    box->setCurrentIndex(separate);
    Q_EMIT box->activated(separate);
    CHECK(first.GetPlayMode() == PlayMode::SeparateWindow);
    CHECK(QSettings().value(PlayModePreference::GetKey()).toString() == QStringLiteral("window"));
    WorldWorkspace second;
    CHECK(second.GetPlayMode() == PlayMode::SeparateWindow);

    second.SetPlayMode(PlayMode::InViewport);
    CHECK(QSettings().value(PlayModePreference::GetKey()).toString() == QStringLiteral("viewport"));
    const WorldWorkspace third;
    CHECK(third.GetPlayMode() == PlayMode::InViewport);
}
TEST_CASE("Separate-window Play leaves the viewport alone and a refusal offers it without launching",
          "[editor][world][play]")
{
    const PlayModePreference preference;
    QTemporaryDir dir;
    const QString path = Fixture(dir);
    WorldWorkspace view;
    QList<PlayMode> requests;
    QObject::connect(&view, &WorldWorkspace::PlayRequested, &view, [&requests](PlayMode mode) {
        requests.append(mode);
    });
    QPushButton* offer = view.findChild<QPushButton*>(QStringLiteral("worldPlayElsewhere"));
    REQUIRE(offer != nullptr);

    // Without a World in the viewport, Play in the viewport says why and starts nothing.
    QString address;
    CHECK(view.PreparePlay(PlayMode::InViewport, address) == PlayWindow::Unavailable);
    CHECK(address.isEmpty() == true);
    CHECK(view.GetPlayUnavailableText().contains(QStringLiteral("needs this World shown in the World viewport")));
    CHECK(view.GetPlayUnavailableText().contains(QStringLiteral("The game was not started")));
    CHECK(offer->isHidden() == false);
    CHECK(view.IsPlayingInViewport() == false);

    // A separate window needs no hosted window: no address, and the viewport keeps editing.
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    CHECK(view.PreparePlay(PlayMode::SeparateWindow, address) == PlayWindow::SeparateWindow);
    CHECK(address.isEmpty() == true);
    CHECK(view.IsPlayingInViewport() == false);
    CHECK(view.GetPlayUnavailableText().isEmpty() == true);
    CHECK(offer->isHidden() == true);

    // Play saved World asks for the chosen mode.
    QPushButton* play = view.findChild<QPushButton*>(QStringLiteral("worldPlay"));
    REQUIRE(play != nullptr);
    // This World has no Play adapter, so the button and the box that configures it are both hidden.
    QComboBox* box = view.findChild<QComboBox*>(QStringLiteral("worldPlayMode"));
    REQUIRE(box != nullptr);
    CHECK(play->isHidden() == true);
    CHECK(box->isHidden() == true);
    play->click();
    REQUIRE(requests.size() == 1);
    CHECK(requests.front() == PlayMode::InViewport);

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // Offscreen tests have no on-screen GPU viewport, so the viewport refuses with its reason.
    if (PreviewViewport::GetPresentation() != ViewportPresentation::SharedDevice)
    {
        CHECK(view.PreparePlay(PlayMode::InViewport, address) == PlayWindow::Unavailable);
        CHECK(address.isEmpty() == true);
        CHECK(view.GetPlayUnavailableText().isEmpty() == false);
        CHECK(view.IsPlayingInViewport() == false);
    }
#else
    CHECK(view.PreparePlay(PlayMode::InViewport, address) == PlayWindow::Unavailable);
    CHECK(view.GetPlayUnavailableText().contains(QStringLiteral("needs")));
#endif
    if (view.GetPlayUnavailableText().isEmpty() == false)
    {
        // The offer plays this once in a separate window and keeps the preference.
        offer->click();
        REQUIRE(requests.size() == 2);
        CHECK(requests.back() == PlayMode::SeparateWindow);
        CHECK(view.GetPlayMode() == PlayMode::InViewport);
        CHECK(offer->isHidden() == true);
        CHECK(view.GetPlayUnavailableText().isEmpty() == true);
    }
    view.EndPlayInViewport();
    view.StopPreview();
}
TEST_CASE("The World viewport is labelled while the game plays in its own window", "[editor][world][play]")
{
    QTemporaryDir dir;
    const QString path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    view.StopPreview();
    QLabel* status = view.findChild<QLabel*>(QStringLiteral("worldPreviewStatus"));
    REQUIRE(status != nullptr);
    view.SetPlayingElsewhere(true);
    CHECK(status->text().startsWith(QStringLiteral("Playing in a separate window.")));
    view.SetPlayingElsewhere(false);
    CHECK(status->text() == QStringLiteral("Viewport paused. World data is preserved."));
}
TEST_CASE("Viewport status messages reach the status bar without resizing the World viewport", "[editor][world][play]")
{
    // A resize cancels a held drag or terrain stroke, so a status line that wraps to a new line count above the
    // viewport would cancel the stroke whenever the preview reported progress.
    QTemporaryDir dir;
    const QString path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    view.StopPreview();
    view.resize(view.minimumSizeHint());
    view.show();
    RenderPanel* renderOrNull = view.findChild<RenderPanel*>();
    REQUIRE(renderOrNull != nullptr);
    const PreviewViewport& viewport = renderOrNull->GetViewport();
    const auto settleLayout = []() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    };
    settleLayout();
    REQUIRE(viewport.isVisible() == true);
    const QSize viewportSize = viewport.size();
    QSignalSpy messages(&view, &WorldWorkspace::StatusMessage);

    view.SetPlayingElsewhere(true);
    settleLayout();
    CHECK(viewport.size() == viewportSize);
    REQUIRE(messages.size() == 1);
    CHECK(messages.back()[0].toString().startsWith(QStringLiteral("Playing in a separate window.")) == true);
    CHECK(messages.back()[1].toBool() == false);

    // A failure several lines long at this width stays out of the World's layout and is kept in the status bar.
    const QString failure = QStringLiteral("The cooked scene could not be read. ").repeated(12);
    renderOrNull->ShowSceneFailure(failure);
    settleLayout();
    CHECK(viewport.size() == viewportSize);
    REQUIRE(messages.size() == 2);
    CHECK(messages.back()[0].toString() == QStringLiteral("Scene import failed: ") + failure);
    CHECK(messages.back()[1].toBool() == true);
    const QToolButton* menuOrNull = view.findChild<QToolButton*>(QStringLiteral("worldViewportMenu"));
    REQUIRE(menuOrNull != nullptr);
    CHECK(menuOrNull->toolTip().endsWith(failure) == true);

    view.SetPlayingElsewhere(false);
    settleLayout();
    CHECK(viewport.size() == viewportSize);
    CHECK(messages.back()[0].toString() == QStringLiteral("Viewport paused. World data is preserved."));
}

namespace
{
RenderScene MakeInteractionScene()
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("object-interaction-fixture");
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9700");
    object.Transform.Translation = {2, 3, 4};
    scene.Objects.append(object);
    PreviewCamera camera;
    camera.Id = QStringLiteral("interaction-camera");
    camera.Sample.Pose.Position = {2, 3, 10};
    camera.Sample.Lens.Projection = ludus::gameplay::camera::CameraProjection::PerspectiveInfinite;
    camera.Sample.Lens.NearPlane = 0.01F;
    scene.Cameras.append(camera);
    return scene;
}
void ConfigureInteractionPreview(RenderPreview& inoutPreview, const RenderScene& scene)
{
    REQUIRE(inoutPreview.SetScene(scene) == true);
    inoutPreview.Navigate(0, {0, 0}, 1, false);
}
} // namespace
TEST_CASE("Viewport object picking uses the nearest triangle and respects empty space and activation",
          "[editor][world][object-interaction]")
{
    RenderScene scene = MakeInteractionScene();
    PreviewObject behind = scene.Objects[0];
    behind.Id = QStringLiteral("behind");
    behind.Transform.Translation.Z -= 2;
    scene.Objects.prepend(behind);
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    const QRectF frame{20, 40, 800, 600};
    CHECK(interaction.Pick(frame.center(), frame) == scene.Objects[1].Id);
    CHECK(interaction.Pick({10, 10}, frame).isEmpty() == true);
    CHECK(interaction.Pick(frame.topLeft(), frame).isEmpty() == true);
    interaction.SetActive(false);
    CHECK(interaction.Pick(frame.center(), frame).isEmpty() == true);
    interaction.SetActive(true);
    scene.Objects.removeLast();
    scene.Identity.append('2');
    ConfigureInteractionPreview(preview, scene);
    CHECK(interaction.Pick(frame.center(), frame) == QStringLiteral("behind"));
}
TEST_CASE("Viewport picking skips objects by mesh bounds and follows a replaced scene's geometry",
          "[editor][world][object-interaction]")
{
    RenderScene scene = MakeInteractionScene();
    // A sliver whose bounds the centre ray enters, nearer than the target, but whose triangle it misses.
    PreviewMesh sliver;
    sliver.Positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}};
    sliver.Indices = {0, 1, 2};
    scene.Meshes.append(sliver);
    PreviewObject boxedMiss = scene.Objects[0];
    boxedMiss.Id = QStringLiteral("boxed-miss");
    boxedMiss.Mesh = 1;
    boxedMiss.Transform.Translation = {2.5F, 2.5F, 5};
    // Far off the ray: its bounds reject it before any triangle test.
    PreviewObject aside = scene.Objects[0];
    aside.Id = QStringLiteral("aside");
    aside.Transform.Translation = {40, 3, 5};
    // Farther along the ray and listed first, so the target must still replace a hit already found.
    PreviewObject behind = scene.Objects[0];
    behind.Id = QStringLiteral("behind");
    behind.Transform.Translation.Z -= 2;
    scene.Objects.prepend(behind);
    scene.Objects.append(boxedMiss);
    scene.Objects.append(aside);
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    const QRectF frame{20, 40, 800, 600};
    CHECK(interaction.Pick(frame.center(), frame) == scene.Objects[1].Id);
    CHECK(interaction.Pick(frame.center(), frame) == scene.Objects[1].Id);

    // A new scene identity rebuilds the cached bounds: the target's mesh now lies off the ray.
    for (ludus::foundation::math::Vector3& positionMesh : scene.Meshes[0].Positions)
    {
        positionMesh.X += 20;
    }
    scene.Identity.append('2');
    ConfigureInteractionPreview(preview, scene);
    CHECK(interaction.Pick(frame.center(), frame).isEmpty() == true);
}
TEST_CASE("Viewport transform drags preview a highlighted silhouette and commit one undoable placement",
          "[editor][world][object-interaction]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    const QString id = scene.Objects[0].Id;
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, id, true);
    const QJsonObject before = document.Draft();
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    QObject::connect(&interaction,
                     &ViewportObjectInteraction::PlacementCommitted,
                     &interaction,
                     [&document, &error](const QString& entityId, const QJsonObject& placement) {
                         const QJsonObject entity = document.Entity(entityId);
                         REQUIRE(document.Edit(entityId,
                                               entity.value(QStringLiteral("components")).toObject(),
                                               entity.value(QStringLiteral("name")).toString(),
                                               error,
                                               &placement) == true);
                     });
    QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        interaction.Paint(painter, image.rect());
    }
    bool hasHighlight = false;
    for (ludus::foundation::int32 y = 0; y < image.height(); ++y)
    {
        for (ludus::foundation::int32 x = 0; x < image.width(); ++x)
        {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 0 && color.red() > 200 && color.green() > 120 && color.blue() < 120)
            {
                hasHighlight = true;
            }
        }
    }
    CHECK(hasHighlight == true);
    const QRectF frame{0, 0, 800, 600};
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    REQUIRE(interaction.Move(frame.center() + QPointF(110, 0), true) == true);
    CHECK(document.Draft() == before);
    REQUIRE(interaction.Release() == true);
    CHECK(committed.count() == 1);
    CHECK(document.Draft() != before);
    CHECK(document.ScenePlacement(document.Entity(id)).value(QStringLiteral("position")).toArray()[0].toDouble() > 2);
    REQUIRE(document.Undo() == true);
    CHECK(document.Draft() == before);
    REQUIRE(document.Redo() == true);
    CHECK(document.Draft() != before);
    CHECK(interaction.Release() == false);
}
TEST_CASE("Viewport gestures cancel without a document edit and readonly objects keep their selection highlight",
          "[editor][world][object-interaction]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    const QRectF frame{0, 0, 800, 600};
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    REQUIRE(interaction.Move(frame.center() + QPointF(110, 0), false) == true);
    interaction.Cancel();
    CHECK(interaction.Release() == false);
    CHECK(committed.count() == 0);
    interaction.SetWorld(document, scene.Objects[0].Id, false);
    CHECK(interaction.Press(frame.center(), frame) == true);
    CHECK(interaction.Move(frame.center() + QPointF(100, 0), false) == false);
    CHECK(committed.count() == 0);
}
TEST_CASE("Viewport scale and rotation tools edit only their selected placement component",
          "[editor][world][object-interaction]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    const QRectF frame{0, 0, 800, 600};
    interaction.SetTool(ViewportObjectInteraction::Tool::Scale);
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    REQUIRE(interaction.Move(frame.center() + QPointF(110, 0), true) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(committed.count() == 1);
    const QJsonObject scaled = committed[0][1].toJsonObject();
    CHECK(scaled.value(QStringLiteral("scale")).toArray() == QJsonArray{1.5, 1, 1});
    CHECK(scaled.value(QStringLiteral("position")) == QJsonArray{2, 3, 4});
    CHECK(scaled.value(QStringLiteral("rotation")) == QJsonArray{0, 0, 0});
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    REQUIRE(interaction.Move(frame.center() - QPointF(400, 0), true) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(committed.count() == 2);
    const QJsonArray reducedScale = committed[1][1].toJsonObject().value(QStringLiteral("scale")).toArray();
    CHECK(reducedScale[0].toDouble() > 0);
    CHECK(reducedScale[1].toDouble() == 1);
    interaction.SetTool(ViewportObjectInteraction::Tool::Rotate);
    const ludus::foundation::float64 diagonal = 80 * ludus::foundation::math::Sqrt(0.5);
    const QPointF ringPixel = frame.center() + QPointF(diagonal, -diagonal);
    REQUIRE(interaction.Press(ringPixel, frame) == true);
    REQUIRE(interaction.Move(ringPixel + QPointF(-20, -20), true) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(committed.count() == 3);
    const QJsonObject rotated = committed[2][1].toJsonObject();
    CHECK(rotated.value(QStringLiteral("rotation")).toArray()[2].toDouble() != 0);
    CHECK(rotated.value(QStringLiteral("rotation")).toArray()[0].toDouble() == 0);
    CHECK(rotated.value(QStringLiteral("rotation")).toArray()[1].toDouble() == 0);
    CHECK(rotated.value(QStringLiteral("scale")) == QJsonArray{1, 1, 1});
}
namespace
{
/// Opaque axis colour of a design token, as the overlay draws it at rest.
QRgb GetAxisToken(int axis)
{
    constexpr ludus::editor::design::ThemeColour AXES[] = {ludus::editor::design::ThemeColour::AxisX,
                                                           ludus::editor::design::ThemeColour::AxisY,
                                                           ludus::editor::design::ThemeColour::AxisZ};
    return ludus::editor::design::GetThemeArgb(AXES[axis], true);
}
} // namespace
TEST_CASE("Transform handles use the design tokens, solid heads and highlight the handle under the pointer",
          "[editor][world][object-interaction][gizmo]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    const QRectF frame{0, 0, 800, 600};
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(frame, overlay);

    // At rest the X handle is its token colour at the normal width, ending in a triangle and a pivot marker.
    REQUIRE(overlay.HandleStrokes.IsEmpty() == false);
    const ViewportHandleStroke rest = overlay.HandleStrokes[0];
    CHECK(rest.Color == GetAxisToken(0));
    CHECK(rest.LinePixel.length() == Catch::Approx(80));
    REQUIRE(overlay.HandleFills.GetSize() >= 2);
    const ViewportHandleFill& arrow = overlay.HandleFills[0];
    CHECK(arrow.CornerCount == 3);
    CHECK(arrow.Color == GetAxisToken(0));
    CHECK(arrow.CornersPixel[0] == rest.LinePixel.p2());
    const ViewportHandleFill& pivot = overlay.HandleFills[overlay.HandleFills.GetSize() - 1];
    CHECK(pivot.CornerCount == 4);
    const QPointF pivotCenter = QRectF(pivot.CornersPixel[0], pivot.CornersPixel[2]).center();
    CHECK(pivotCenter.x() == Catch::Approx(rest.LinePixel.p1().x()));
    CHECK(pivotCenter.y() == Catch::Approx(rest.LinePixel.p1().y()));
    CHECK(overlay.HandleFills.GetSize() <= ViewportObjectOverlay::MAX_HANDLE_FILLS);

    // Hovering announces itself once, emphasizes only that handle and clears when the pointer moves away.
    QSignalSpy changed(&interaction, &ViewportObjectInteraction::Changed);
    CHECK(interaction.GetHoveredAxis() == -1);
    CHECK(interaction.Hover(frame.center() + QPointF(70, 0), frame) == true);
    CHECK(interaction.GetHoveredAxis() == 0);
    CHECK(changed.count() == 1);
    CHECK(interaction.Hover(frame.center() + QPointF(72, 2), frame) == true);
    CHECK(changed.count() == 1);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.HandleStrokes[0].Color != rest.Color);
    CHECK(overlay.HandleStrokes[0].WidthPixels > rest.WidthPixels);
    CHECK(interaction.Hover(frame.center() + QPointF(0, 250), frame) == false);
    CHECK(interaction.GetHoveredAxis() == -1);
    CHECK(changed.count() == 2);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.HandleStrokes[0].Color == rest.Color);
    CHECK(overlay.HandleStrokes[0].WidthPixels == rest.WidthPixels);

    // Leaving the viewport drops the highlight.
    static_cast<void>(interaction.Hover(frame.center() + QPointF(70, 0), frame));
    interaction.ClearHover();
    CHECK(interaction.GetHoveredAxis() == -1);

    // A drag keeps its handle emphasized and ignores hover.
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    CHECK(interaction.IsDragging() == true);
    CHECK(interaction.Hover(frame.center() + QPointF(70, 0), frame) == false);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.HandleStrokes[0].Color != rest.Color);
    CHECK(overlay.HandleStrokes[0].WidthPixels > rest.WidthPixels);
    for (usize index = 1; index < overlay.HandleStrokes.GetSize(); ++index)
    {
        // Every other handle recedes towards the viewport while one axis is dragged.
        for (int axis = 0; axis < ViewportObjectInteraction::AXIS_COUNT; ++axis)
        {
            if (overlay.HandleStrokes[index].WidthPixels > 1)
            {
                CHECK(overlay.HandleStrokes[index].Color != GetAxisToken(axis));
            }
        }
    }
    interaction.Cancel();
    CHECK(interaction.IsDragging() == false);

    // Without edit permission or while inactive nothing can be hovered.
    interaction.SetWorld(document, scene.Objects[0].Id, false);
    CHECK(interaction.Hover(frame.center() + QPointF(70, 0), frame) == false);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    interaction.SetActive(false);
    CHECK(interaction.Hover(frame.center() + QPointF(70, 0), frame) == false);
}
TEST_CASE("Scale handles end in a cube and rotation rings dim their far half",
          "[editor][world][object-interaction][gizmo]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    const QRectF frame{0, 0, 800, 600};
    ViewportObjectOverlay overlay;

    interaction.SetTool(ViewportObjectInteraction::Tool::Scale);
    interaction.BuildOverlay(frame, overlay);
    REQUIRE(overlay.HandleFills.GetSize() >= 2);
    CHECK(overlay.HandleFills[0].CornerCount == 4);
    CHECK(overlay.HandleFills[0].Color == GetAxisToken(0));
    const QRectF cube(overlay.HandleFills[0].CornersPixel[0], overlay.HandleFills[0].CornersPixel[2]);
    CHECK(cube.center().x() == Catch::Approx(frame.center().x() + 80));

    interaction.SetTool(ViewportObjectInteraction::Tool::Rotate);
    interaction.BuildOverlay(frame, overlay);
    // Rings are strokes only, plus the pivot marker.
    CHECK(overlay.HandleFills.GetSize() == 1);
    usize frontCount = 0;
    usize backCount = 0;
    for (const ViewportHandleStroke& stroke : overlay.HandleStrokes)
    {
        bool isToken = false;
        for (int axis = 0; axis < ViewportObjectInteraction::AXIS_COUNT; ++axis)
        {
            isToken = isToken == true || stroke.Color == GetAxisToken(axis);
        }
        (isToken == true ? frontCount : backCount) += 1;
    }
    CHECK(frontCount > 0);
    CHECK(backCount > 0);
    CHECK(overlay.HandleStrokes.GetSize() <= ViewportObjectOverlay::MAX_HANDLE_STROKES);
}
namespace
{
/// Centre of a four-corner fill, in frame pixels.
QPointF GetFillCentre(const ludus::editor::ViewportHandleFill& fill)
{
    return (fill.CornersPixel[0] + fill.CornersPixel[1] + fill.CornersPixel[2] + fill.CornersPixel[3]) * 0.25;
}
/// The committed position after pressing at pressPixel and releasing at releasePixel, with the move tool.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): press then release is the drag order.
QJsonArray DragCommittedPosition(ViewportObjectInteraction& inoutInteraction,
                                 QSignalSpy& inoutCommitted,
                                 const QRectF& frame,
                                 QPointF pressPixel,
                                 QPointF releasePixel,
                                 bool shouldSnap)
{
    const qsizetype commitsBefore = inoutCommitted.count();
    REQUIRE(inoutInteraction.Press(pressPixel, frame) == true);
    REQUIRE(inoutInteraction.Move(releasePixel, shouldSnap) == true);
    REQUIRE(inoutInteraction.Release() == true);
    REQUIRE(inoutCommitted.count() == commitsBefore + 1);
    return inoutCommitted.last()[1].toJsonObject().value(QStringLiteral("position")).toArray();
}
// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace
TEST_CASE("Move plane handles sit beside the pivot, lose to a shaft under the pointer and drag in their plane",
          "[editor][world][object-interaction][gizmo]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    const QRectF frame{0, 0, 800, 600};
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(frame, overlay);

    // Viewed down -Z the Z axis is end-on, so only the XY plane faces the camera: an arrowhead each for X and Y, the
    // plane's casing and solid square, and the pivot marker last.
    REQUIRE(overlay.HandleFills.GetSize() == 5);
    const ViewportHandleFill casing = overlay.HandleFills[2];
    const ViewportHandleFill square = overlay.HandleFills[3];
    CHECK(casing.CornerCount == 4);
    CHECK(square.CornerCount == 4);
    CHECK(casing.Color ==
          ludus::editor::design::GetThemeArgb(ludus::editor::design::ThemeColour::SurfaceViewport, true));
    CHECK(square.Color != GetAxisToken(2));
    CHECK(qBlue(square.Color) > qRed(square.Color));
    CHECK(overlay.HandleFills[4].CornerCount == 4);
    const QPointF squarePixel = GetFillCentre(square);
    CHECK(squarePixel.x() > frame.center().x() + 22);
    CHECK(squarePixel.y() < frame.center().y() - 22);
    CHECK(QRectF(casing.CornersPixel[0], casing.CornersPixel[2]).width() >
          QRectF(square.CornersPixel[0], square.CornersPixel[2]).width());

    // The square is a handle of its own; a shaft within a few pixels still wins over it.
    CHECK(interaction.Hover(squarePixel, frame) == true);
    CHECK(interaction.GetHoveredHandle().Kind == ViewportObjectInteraction::HandleKind::Plane);
    CHECK(interaction.GetHoveredHandle().Index == 2);
    CHECK(interaction.GetHoveredAxis() == -1);
    CHECK(interaction.HasHoveredHandle() == true);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.HandleFills[3].Color != square.Color);
    CHECK(interaction.Hover(frame.center() + QPointF(40, 2), frame) == true);
    CHECK(interaction.GetHoveredHandle().Kind == ViewportObjectInteraction::HandleKind::Axis);
    CHECK(interaction.GetHoveredAxis() == 0);
    CHECK(interaction.Hover(frame.center() + QPointF(10, -10), frame) == false);
    CHECK(interaction.HasHoveredHandle() == false);

    // Dragging along one axis of the plane moves exactly as that axis' own handle does, and the plane's normal stays
    // put.
    const QJsonArray byAxis = DragCommittedPosition(interaction,
                                                    committed,
                                                    frame,
                                                    frame.center() + QPointF(70, 0),
                                                    frame.center() + QPointF(110, 0),
                                                    false);
    const QJsonArray byPlane =
        DragCommittedPosition(interaction, committed, frame, squarePixel, squarePixel + QPointF(40, 0), false);
    CHECK(byPlane[0].toDouble() == Catch::Approx(byAxis[0].toDouble()).margin(0.001));
    CHECK(byPlane[0].toDouble() > 2);
    CHECK(byPlane[1].toDouble() == Catch::Approx(3).margin(0.001));
    CHECK(byPlane[2].toDouble() == 4);

    // A diagonal drag moves both plane axes, up on the screen being +Y.
    const QJsonArray diagonal =
        DragCommittedPosition(interaction, committed, frame, squarePixel, squarePixel + QPointF(40, -40), false);
    CHECK(diagonal[0].toDouble() == Catch::Approx(byAxis[0].toDouble()).margin(0.001));
    CHECK(diagonal[1].toDouble() == Catch::Approx(3 + (byAxis[0].toDouble() - 2)).margin(0.001));
    CHECK(diagonal[2].toDouble() == 4);

    // Shift snaps each plane axis to a tenth of a metre.
    const QJsonArray snapped =
        DragCommittedPosition(interaction, committed, frame, squarePixel, squarePixel + QPointF(37, -53), true);
    for (int axis = 0; axis < 2; ++axis)
    {
        const ludus::foundation::float64 tenths = snapped[axis].toDouble() * 10;
        CHECK(tenths == Catch::Approx(std::round(tenths)).margin(0.0001));
    }
    CHECK(snapped[2].toDouble() == 4);
}
TEST_CASE("Move plane drags show the distance moved beside the pointer and clear it with the gesture",
          "[editor][world][object-interaction][gizmo]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    const QRectF frame{0, 0, 800, 600};
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.ReadoutText.isEmpty() == true);
    const QPointF squarePixel = GetFillCentre(overlay.HandleFills[3]);
    const QRgb restColour = overlay.HandleFills[3].Color;

    // The label appears with the drag, follows the pointer and names both axes of the plane.
    REQUIRE(interaction.Press(squarePixel, frame) == true);
    CHECK(interaction.GetDraggedHandle().Kind == ViewportObjectInteraction::HandleKind::Plane);
    const QPointF movedPixel = squarePixel + QPointF(40, -40);
    REQUIRE(interaction.Move(movedPixel, false) == true);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.ReadoutText.startsWith(QStringLiteral("X +")) == true);
    CHECK(overlay.ReadoutText.contains(QStringLiteral("Y +")) == true);
    CHECK(overlay.ReadoutText.endsWith(QStringLiteral(" m")) == true);
    CHECK(overlay.ReadoutAnchorPixel == movedPixel);
    // The plane being dragged is emphasized.
    CHECK(overlay.HandleFills[3].Color != restColour);
    interaction.Cancel();
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.ReadoutText.isEmpty() == true);

    // A released drag leaves no label either.
    REQUIRE(interaction.Press(squarePixel, frame) == true);
    REQUIRE(interaction.Move(movedPixel, false) == true);
    REQUIRE(interaction.Release() == true);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.ReadoutText.isEmpty() == true);

    // An axis drag shows its one distance.
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    REQUIRE(interaction.Move(frame.center() + QPointF(110, 0), true) == true);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.ReadoutText.startsWith(QStringLiteral("X +")) == true);
    CHECK(overlay.ReadoutText.contains(QStringLiteral("Y")) == false);
    interaction.Cancel();
}
TEST_CASE("Move plane handles follow the camera: an oblique view shows all three, each its own plane",
          "[editor][world][object-interaction][gizmo]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    const QRectF frame{0, 0, 800, 600};
    ViewportObjectOverlay overlay;
    // Looking from above and aside, every axis has a length on the screen and every plane faces the camera.
    preview.Orbit(70, 90);
    interaction.BuildOverlay(frame, overlay);
    // Three arrowheads, three planes with their casings, and the pivot marker.
    CHECK(overlay.HandleFills.GetSize() == ViewportObjectOverlay::MAX_TRANSFORM_HANDLE_FILLS);
    ludus::foundation::StaticArray<bool, ViewportObjectInteraction::AXIS_COUNT> hasNormal{};
    ludus::foundation::usize normalCount = 0;
    for (ludus::foundation::usize plane = 0;
         plane < static_cast<ludus::foundation::usize>(ViewportObjectInteraction::AXIS_COUNT);
         ++plane)
    {
        const QPointF centre = GetFillCentre(overlay.HandleFills[3 + 2 * plane + 1]);
        REQUIRE(interaction.Hover(centre, frame) == true);
        const ViewportObjectInteraction::HandleTarget target = interaction.GetHoveredHandle();
        CHECK(target.Kind == ViewportObjectInteraction::HandleKind::Plane);
        REQUIRE(target.Index >= 0);
        REQUIRE(target.Index < ViewportObjectInteraction::AXIS_COUNT);
        normalCount += hasNormal[static_cast<ludus::foundation::usize>(target.Index)] == true ? 0 : 1;
        hasNormal[static_cast<ludus::foundation::usize>(target.Index)] = true;
    }
    // Each square is its own plane: the three normals are all different.
    CHECK(normalCount == 3);
}
TEST_CASE("The scale centre handle scales every axis together and shows one factor",
          "[editor][world][object-interaction][gizmo]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Scale);
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    const QRectF frame{0, 0, 800, 600};
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(frame, overlay);

    // X and Y cubes, then the centre casing and cube, which replace the pivot marker.
    REQUIRE(overlay.HandleFills.GetSize() == 4);
    const QPointF centre = GetFillCentre(overlay.HandleFills[3]);
    CHECK(centre.x() == Catch::Approx(frame.center().x()));
    CHECK(centre.y() == Catch::Approx(frame.center().y()));
    CHECK(overlay.HandleFills[3].Color != overlay.HandleFills[0].Color);

    // The centre wins over everything near the pivot; the axis cubes stay their own handles.
    CHECK(interaction.Hover(centre + QPointF(6, -6), frame) == true);
    CHECK(interaction.GetHoveredHandle().Kind == ViewportObjectInteraction::HandleKind::Uniform);
    CHECK(interaction.GetHoveredAxis() == -1);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.HandleFills[3].Color != GetAxisToken(0));
    CHECK(interaction.Hover(centre + QPointF(80, 0), frame) == true);
    CHECK(interaction.GetHoveredAxis() == 0);
    CHECK(interaction.Hover(centre + QPointF(30, 0), frame) == true);
    CHECK(interaction.GetHoveredAxis() == 0);

    // Up and to the right grows every axis by the same factor; the pointer travels 40 px on each screen axis.
    REQUIRE(interaction.Press(centre, frame) == true);
    CHECK(interaction.GetDraggedHandle().Kind == ViewportObjectInteraction::HandleKind::Uniform);
    REQUIRE(interaction.Move(centre + QPointF(40, -40), false) == true);
    interaction.BuildOverlay(frame, overlay);
    const ludus::foundation::float64 factor = 1 + 80 * ludus::foundation::math::Sqrt(0.5) / 80;
    CHECK(overlay.ReadoutText == QStringLiteral("\u00D7") + QString::number(factor, 'f', 2));
    REQUIRE(interaction.Release() == true);
    REQUIRE(committed.count() == 1);
    const QJsonArray grown = committed[0][1].toJsonObject().value(QStringLiteral("scale")).toArray();
    CHECK(grown[0].toDouble() == Catch::Approx(factor));
    CHECK(grown[1].toDouble() == Catch::Approx(factor));
    CHECK(grown[2].toDouble() == Catch::Approx(factor));
    CHECK(committed[0][1].toJsonObject().value(QStringLiteral("position")) == QJsonArray{2, 3, 4});

    // Shift snaps the factor to a tenth; a drag past zero stops at the smallest scale instead of inverting.
    REQUIRE(interaction.Press(centre, frame) == true);
    REQUIRE(interaction.Move(centre + QPointF(20, 0), true) == true);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.ReadoutText == QStringLiteral("\u00D71.20"));
    REQUIRE(interaction.Move(centre + QPointF(-4000, 4000), false) == true);
    REQUIRE(interaction.Release() == true);
    const QJsonArray floored = committed[1][1].toJsonObject().value(QStringLiteral("scale")).toArray();
    CHECK(floored[0].toDouble() == Catch::Approx(0.01));
    CHECK(floored[2].toDouble() == Catch::Approx(0.01));
}
TEST_CASE("Rotation and axis scale drags show the angle in degrees and the factor of their axis",
          "[editor][world][object-interaction][gizmo]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    const QRectF frame{0, 0, 800, 600};
    ViewportObjectOverlay overlay;

    interaction.SetTool(ViewportObjectInteraction::Tool::Scale);
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    REQUIRE(interaction.Move(frame.center() + QPointF(110, 0), true) == true);
    interaction.BuildOverlay(frame, overlay);
    CHECK(overlay.ReadoutText == QStringLiteral("X \u00D71.50"));
    interaction.Cancel();

    interaction.SetTool(ViewportObjectInteraction::Tool::Rotate);
    const ludus::foundation::float64 diagonal = 80 * ludus::foundation::math::Sqrt(0.5);
    const QPointF ringPixel = frame.center() + QPointF(diagonal, -diagonal);
    REQUIRE(interaction.Press(ringPixel, frame) == true);
    REQUIRE(interaction.Move(ringPixel + QPointF(-20, -20), true) == true);
    interaction.BuildOverlay(frame, overlay);
    // Shift snaps to fifteen degrees, and the angle carries its sign and the degree symbol.
    CHECK(overlay.ReadoutText.startsWith(QStringLiteral("Z ")) == true);
    CHECK(overlay.ReadoutText.endsWith(QStringLiteral("\u00B0")) == true);
    CHECK(overlay.ReadoutText.contains(QStringLiteral(".0")) == true);
    const ludus::foundation::float64 degrees = overlay.ReadoutText.mid(2, overlay.ReadoutText.size() - 3).toDouble();
    CHECK(std::fmod(std::abs(degrees), 15) == Catch::Approx(0).margin(0.0001));
    CHECK(degrees != 0);
    interaction.Cancel();
}
TEST_CASE("Viewport move axes follow a rotated scaled parent and preserve parent-local placement",
          "[editor][world][object-interaction]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    QJsonObject level = ImportedLevel();
    QJsonArray objects = level.value(QStringLiteral("objects")).toArray();
    QJsonObject child = objects[0].toObject();
    const QString parentId = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9701");
    child.insert(QStringLiteral("parent"), parentId);
    QJsonObject parent = objects[0].toObject();
    parent.insert(QStringLiteral("id"), parentId);
    parent.insert(QStringLiteral("transform"), QJsonArray{0, 2, 0, -1, 0, 0, 0, 0, 1, 10, 0, 0});
    objects[0] = child;
    objects.append(parent);
    level.insert(QStringLiteral("objects"), objects);
    REQUIRE(document.ImportScene(level, error) == true);
    RenderScene scene = MakeInteractionScene();
    PreviewObject parentObject;
    parentObject.Id = parentId;
    parentObject.Transform.Columns[0] = {0, 2, 0};
    parentObject.Transform.Columns[1] = {-1, 0, 0};
    parentObject.Transform.Translation = {10, 0, 0};
    scene.Objects[0].Transform = ludus::foundation::math::Compose(parentObject.Transform, scene.Objects[0].Transform);
    scene.Cameras[0].Sample.Pose.Position = {7, 4, 10};
    scene.Objects.append(parentObject);
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    const QRectF frame{0, 0, 800, 600};
    REQUIRE(interaction.Press(frame.center() + QPointF(0, -70), frame) == true);
    REQUIRE(interaction.Move(frame.center() + QPointF(0, -110), true) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(committed.count() == 1);
    const QJsonArray position = committed[0][1].toJsonObject().value(QStringLiteral("position")).toArray();
    CHECK(position[0].toDouble() > 2);
    CHECK(position[1].toDouble() == 3);
    CHECK(position[2].toDouble() == 4);
}

TEST_CASE("Viewport picking reads immutable cooked geometry and matches orthographic letterboxed images",
          "[editor][world][object-interaction]")
{
    using namespace ludus::foundation;
    RenderScene scene = MakeInteractionScene();
    SharedPtr<ludus::resources::SceneDefinition> definition;
    REQUIRE(TryMakeShared<ludus::resources::SceneDefinition>(GetSystemAllocationDomain(), definition) ==
            OwnershipStatus::Ok);
    REQUIRE(definition->Meshes.TryResize(1) == true);
    ludus::resources::SceneMesh& mesh = definition->Meshes[0];
    REQUIRE(mesh.Positions.TryResize(3) == true);
    REQUIRE(mesh.Indices.TryResize(3) == true);
    for (usize index = 0; index < 3; ++index)
    {
        mesh.Positions[index] = scene.Meshes[0].Positions[static_cast<int32>(index)];
        mesh.Indices[index] = scene.Meshes[0].Indices[static_cast<int32>(index)];
    }
    scene.Definition = definition;
    scene.Meshes[0].Positions.clear();
    scene.Meshes[0].Indices.clear();
    scene.Cameras[0].Sample.Lens.Projection = ludus::gameplay::camera::CameraProjection::OrthographicFinite;
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetImageSize({400, 400});
    const QRectF frame{0, 0, 800, 600};
    CHECK(interaction.Pick(frame.center(), frame) == scene.Objects[0].Id);
    CHECK(interaction.Pick({50, 300}, frame).isEmpty() == true);
    QSignalSpy selection(&interaction, &ViewportObjectInteraction::SelectionRequested);
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    REQUIRE(interaction.Press(frame.center(), frame) == true);
    REQUIRE(selection.count() == 1);
    CHECK(selection[0][0].toString() == scene.Objects[0].Id);
    SharedPtr<ludus::resources::SceneDefinition> invalidDefinition;
    REQUIRE(TryMakeShared<ludus::resources::SceneDefinition>(GetSystemAllocationDomain(), invalidDefinition) ==
            OwnershipStatus::Ok);
    scene.Definition = invalidDefinition;
    scene.Identity.append('2');
    REQUIRE(preview.SetScene(scene) == true);
    CHECK(interaction.Pick(frame.center(), frame).isEmpty() == true);
}
TEST_CASE("World viewport interaction shares hierarchy selection inspector validation and undo",
          "[editor][world][object-interaction]")
{
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    const QString external = directory.filePath(QStringLiteral("external"));
    QString error;
    REQUIRE(ludus::editor::tests::CreateFixtureSource(external) == true);
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(directory.path(),
                                                       QStringLiteral("scenes/room.lulevel"),
                                                       external,
                                                       error) == true);
    WorldDocument document = Document();
    const QJsonObject level =
        QJsonDocument::fromJson(Read(directory.filePath(QStringLiteral("scenes/room.lulevel")))).object();
    REQUIRE(document.ImportScene(level, error) == true);
    Write(path, document.Encode());
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    const auto waitUntilSettled = [&workspace]() {
        QElapsedTimer timer;
        timer.start();
        while (workspace.CanSave() == false && timer.elapsed() < 15000)
        {
            QTest::qWait(10);
        }
        REQUIRE(workspace.CanSave() == true);
    };
    waitUntilSettled();
    LevelWorkspace* sceneOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(sceneOrNull != nullptr);
    ViewportObjectInteraction& interaction = sceneOrNull->GetViewport().GetObjectInteraction();
    const QString id =
        level.value(QStringLiteral("objects")).toArray()[0].toObject().value(QStringLiteral("id")).toString();
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    interaction.SelectionRequested(id);
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(entitiesOrNull->currentItem() != nullptr);
    CHECK(entitiesOrNull->currentItem()->data(0, Qt::UserRole).toString() == id);
    QAction* toolOrNull = sceneOrNull->GetViewport().findChild<QAction*>(QStringLiteral("viewportObjectToolAction"));
    REQUIRE(toolOrNull != nullptr);
    CHECK(toolOrNull->isVisible() == true);
    interaction.SetActive(true);
    QTest::keyClick(entitiesOrNull, Qt::Key_Q);
    CHECK(interaction.GetTool() == ViewportObjectInteraction::Tool::Select);
    QTest::keyClick(entitiesOrNull, Qt::Key_W);
    CHECK(interaction.GetTool() == ViewportObjectInteraction::Tool::Move);
    QTest::keyClick(entitiesOrNull, Qt::Key_E);
    CHECK(interaction.GetTool() == ViewportObjectInteraction::Tool::Rotate);
    QTest::keyClick(entitiesOrNull, Qt::Key_R);
    CHECK(interaction.GetTool() == ViewportObjectInteraction::Tool::Scale);
    CHECK(entitiesOrNull->currentItem()->data(0, Qt::UserRole).toString() == id);
    const QJsonObject before = workspace.Document().Draft();
    QJsonObject placement = workspace.Document().ScenePlacement(workspace.Document().Entity(id));
    QJsonArray position = placement.value(QStringLiteral("position")).toArray();
    position[0] = position[0].toDouble() + 2;
    placement.insert(QStringLiteral("position"), position);
    interaction.PlacementCommitted(id, placement);
    CHECK(workspace.Document().ScenePlacement(workspace.Document().Entity(id)).value(QStringLiteral("position")) ==
          position);
    waitUntilSettled();
    workspace.Undo();
    CHECK(workspace.Document().Draft() == before);
    waitUntilSettled();
    workspace.Redo();
    CHECK(workspace.Document().Draft() != before);
    waitUntilSettled();
    const QJsonObject accepted = workspace.Document().Draft();
    placement.insert(QStringLiteral("scale"), QJsonArray{0, 1, 1});
    interaction.PlacementCommitted(id, placement);
    CHECK(workspace.Document().Draft() == accepted);
    REQUIRE(entitiesOrNull->selectedItems().size() == 1);
    QTest::keyClick(&sceneOrNull->GetViewport(), Qt::Key_Escape);
    CHECK(entitiesOrNull->selectedItems().isEmpty() == true);
    CHECK(workspace.Session().value(QStringLiteral("selection")).toString().isEmpty() == true);
    CHECK(workspace.findChild<QLineEdit*>(QStringLiteral("worldEntityName")) == nullptr);
    CHECK(workspace.Document().Draft() == accepted);
}
namespace
{
namespace math = ludus::foundation::math;
namespace camera = ludus::gameplay::camera;
using ludus::foundation::float32;
using ludus::foundation::float64;

constexpr math::Vector3 NESTED_ROTATE_POSITION_PARENT{2, 3, 4};
constexpr math::Vector3 NESTED_ROTATE_PARENT_POSITION_WORLD{-2, 1, 3};
constexpr math::Vector3 NESTED_ROTATE_SCALE{1.4F, 0.8F, 1.1F};
constexpr math::Vector3 NESTED_ROTATE_EULER_DEGREES{19, -24, 31};
constexpr float32 PARENT_ROTATION_DEGREES = 90;
constexpr float32 ROTATE_RING_RADIUS_PIXELS = 80;
constexpr float32 ROTATE_TEST_VERTICAL_SPAN = 6;
constexpr float32 ROTATE_TEST_CAMERA_DISTANCE_WORLD = 10;
constexpr int WORLD_ROTATE_TIMEOUT_MILLISECONDS = 15000;

math::Quaternion MakeAxisRotation(math::Vector3 axis, float32 degrees)
{
    math::Quaternion rotation;
    REQUIRE(math::TryFromAxisAngle(axis, math::DegreesToRadians(degrees), rotation) == math::MathStatus::Success);
    return rotation;
}

math::Vector3 TransformRotateFixturePointToWorld(math::Vector3 pointMesh, math::Vector3 eulerDegrees)
{
    // Apply the documented scale, X, Y, Z, translation and parent transform in sequence.
    // The imported child basis is identity. No gizmo axes or placement-composition code are used here.
    const math::Vector3 scaledMesh{pointMesh.X * NESTED_ROTATE_SCALE.X,
                                   pointMesh.Y * NESTED_ROTATE_SCALE.Y,
                                   pointMesh.Z * NESTED_ROTATE_SCALE.Z};
    const math::Vector3 rotatedX = math::Rotate(MakeAxisRotation({1, 0, 0}, eulerDegrees.X), scaledMesh);
    const math::Vector3 rotatedXY = math::Rotate(MakeAxisRotation({0, 1, 0}, eulerDegrees.Y), rotatedX);
    const math::Vector3 rotatedXYZ = math::Rotate(MakeAxisRotation({0, 0, 1}, eulerDegrees.Z), rotatedXY);
    return math::Rotate(MakeAxisRotation({0, 1, 0}, PARENT_ROTATION_DEGREES),
                        rotatedXYZ + NESTED_ROTATE_POSITION_PARENT) +
           NESTED_ROTATE_PARENT_POSITION_WORLD;
}

QPointF
ProjectRotateFixturePoint(math::Vector3 pointWorld, const camera::CameraSample& cameraSample, const QRectF& frame)
{
    const math::Vector3 relativeWorld = pointWorld - math::Vector3{static_cast<float32>(cameraSample.Pose.Position.X),
                                                                   static_cast<float32>(cameraSample.Pose.Position.Y),
                                                                   static_cast<float32>(cameraSample.Pose.Position.Z)};
    const float64 pixelsPerMetre = frame.height() / static_cast<float64>(cameraSample.Lens.VerticalSpan);
    const math::Vector3 rightWorld = math::Rotate(cameraSample.Pose.Orientation, {1, 0, 0});
    const math::Vector3 upWorld = math::Rotate(cameraSample.Pose.Orientation, {0, 1, 0});
    return frame.center() + QPointF(static_cast<float64>(math::Dot(relativeWorld, rightWorld)),
                                    -static_cast<float64>(math::Dot(relativeWorld, upWorld))) *
                                pixelsPerMetre;
}

PreviewCamera MakeRotateFixtureCamera(float32 yawDegrees, float32 pitchDegrees)
{
    const math::Vector3 pivotWorld = TransformRotateFixturePointToWorld({}, NESTED_ROTATE_EULER_DEGREES);
    const math::Quaternion yawFrameToWorld = MakeAxisRotation({0, 1, 0}, PARENT_ROTATION_DEGREES) *
                                             MakeAxisRotation({0, 0, 1}, NESTED_ROTATE_EULER_DEGREES.Z);
    PreviewCamera view;
    view.Id = QStringLiteral("nested-rotation-camera");
    view.Sample.Lens.VerticalSpan = ROTATE_TEST_VERTICAL_SPAN;
    view.Sample.Pose.Orientation =
        yawFrameToWorld * MakeAxisRotation({0, 1, 0}, yawDegrees) * MakeAxisRotation({1, 0, 0}, pitchDegrees);
    const math::Vector3 eyeWorld =
        pivotWorld + math::Rotate(view.Sample.Pose.Orientation, {0, 0, ROTATE_TEST_CAMERA_DISTANCE_WORLD});
    view.Sample.Pose.Position = {static_cast<float64>(eyeWorld.X),
                                 static_cast<float64>(eyeWorld.Y),
                                 static_cast<float64>(eyeWorld.Z)};
    return view;
}

enum class RotateFixtureAxis : ludus::foundation::uint8
{
    X = 0,
    Z = 2,
};

math::Vector3 MakeRotateFixtureProbe(RotateFixtureAxis axis, float32 arcDegrees, const QRectF& frame)
{
    // Construct a probe fixed in the asymmetric mesh's coordinates. For the X arc, its scaled
    // coordinates are in YZ. For the Z arc, apply inverse Y and X so the point is in XY just before Rz.
    const float32 radiusWorld =
        ROTATE_RING_RADIUS_PIXELS * ROTATE_TEST_VERTICAL_SPAN / static_cast<float32>(frame.height());
    const float32 angle = math::DegreesToRadians(arcDegrees);
    const float32 cosine = math::Cos(angle) * radiusWorld;
    const float32 sine = math::Sin(angle) * radiusWorld;
    math::Vector3 scaledProbeMesh{0, cosine, sine};
    if (axis == RotateFixtureAxis::Z)
    {
        const math::Vector3 undoneY =
            math::Rotate(MakeAxisRotation({0, 1, 0}, -NESTED_ROTATE_EULER_DEGREES.Y), {cosine, sine, 0});
        scaledProbeMesh = math::Rotate(MakeAxisRotation({1, 0, 0}, -NESTED_ROTATE_EULER_DEGREES.X), undoneY);
    }
    return {scaledProbeMesh.X / NESTED_ROTATE_SCALE.X,
            scaledProbeMesh.Y / NESTED_ROTATE_SCALE.Y,
            scaledProbeMesh.Z / NESTED_ROTATE_SCALE.Z};
}

QString WriteNestedRotateWorld(const QTemporaryDir& directory, const QString& path)
{
    const QString external = directory.filePath(QStringLiteral("external"));
    const QString levelPath = QStringLiteral("scenes/nested.lulevel");
    QString error;
    REQUIRE(tests::CreateFixtureSource(external) == true);
    REQUIRE(tests::PrepareImportedLevel(directory.path(), levelPath, external, error) == true);
    QJsonObject level = QJsonDocument::fromJson(Read(directory.filePath(levelPath))).object();
    QJsonArray objects = level.value(QStringLiteral("objects")).toArray();
    REQUIRE(objects.size() == 1);
    QJsonObject child = objects[0].toObject();
    const QString childId = child.value(QStringLiteral("id")).toString();
    const QString parentId = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9701");
    child.insert(QStringLiteral("parent"), parentId);
    child.insert(QStringLiteral("transform"),
                 QJsonArray{1,
                            0,
                            0,
                            0,
                            1,
                            0,
                            0,
                            0,
                            1,
                            static_cast<float64>(NESTED_ROTATE_POSITION_PARENT.X),
                            static_cast<float64>(NESTED_ROTATE_POSITION_PARENT.Y),
                            static_cast<float64>(NESTED_ROTATE_POSITION_PARENT.Z)});
    QJsonObject parent = child;
    parent.insert(QStringLiteral("parent"), QStringLiteral(""));
    parent.insert(QStringLiteral("id"), parentId);
    parent.insert(QStringLiteral("name"), QStringLiteral("Rotate Fixture Parent"));
    parent.insert(QStringLiteral("transform"),
                  QJsonArray{0,
                             0,
                             -1,
                             0,
                             1,
                             0,
                             1,
                             0,
                             0,
                             static_cast<float64>(NESTED_ROTATE_PARENT_POSITION_WORLD.X),
                             static_cast<float64>(NESTED_ROTATE_PARENT_POSITION_WORLD.Y),
                             static_cast<float64>(NESTED_ROTATE_PARENT_POSITION_WORLD.Z)});
    objects[0] = child;
    objects.append(parent);
    level.insert(QStringLiteral("objects"), objects);

    WorldDocument document = Document();
    const bool isImported = document.ImportScene(level, error);
    INFO(error.toStdString());
    REQUIRE(isImported == true);
    const QJsonObject entity = document.Entity(childId);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    transform.insert(QStringLiteral("rx"), static_cast<float64>(NESTED_ROTATE_EULER_DEGREES.X));
    transform.insert(QStringLiteral("ry"), static_cast<float64>(NESTED_ROTATE_EULER_DEGREES.Y));
    transform.insert(QStringLiteral("rz"), static_cast<float64>(NESTED_ROTATE_EULER_DEGREES.Z));
    transform.insert(QStringLiteral("sx"), static_cast<float64>(NESTED_ROTATE_SCALE.X));
    transform.insert(QStringLiteral("sy"), static_cast<float64>(NESTED_ROTATE_SCALE.Y));
    transform.insert(QStringLiteral("sz"), static_cast<float64>(NESTED_ROTATE_SCALE.Z));
    components.insert(QStringLiteral("transform"), transform);
    REQUIRE(document.Edit(childId, components, entity.value(QStringLiteral("name")).toString(), error) == true);
    Write(path, document.Encode());
    return childId;
}

math::Affine3 FindRotateFixtureTransform(const RenderScene& scene, const QString& childId)
{
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id == childId)
        {
            return object.Transform;
        }
    }
    FAIL("Nested child did not reach the viewport scene");
    return {};
}

void CheckRotateFixtureTransforms(const math::Affine3& actual, const math::Affine3& expected)
{
    // The gizmo composes in float32, and the cook composes the hierarchy in float64 before narrowing.
    constexpr float32 COOK_ROUNDING_TOLERANCE = 0.00001F;
    CHECK(math::Length(actual.Translation - expected.Translation) < COOK_ROUNDING_TOLERANCE);
    for (ludus::foundation::int32 axis = 0; axis < ViewportObjectInteraction::AXIS_COUNT; ++axis)
    {
        CHECK(math::Length(actual.Columns[axis] - expected.Columns[axis]) < COOK_ROUNDING_TOLERANCE);
    }
}
} // namespace

TEST_CASE("Nested World rotate rings follow projected mesh motion and preserve placement history",
          "[editor][world][object-interaction][gizmo][nested-rotate]")
{
    // Thanks to Britton, Lipscomb and Pique, Making Nested Rotations Convenient for the User (SIGGRAPH '78),
    // https://doi.org/10.1145/800248.807394, for judging nested rotation by agreement with hand motion.
    // Thanks to Nielson and Olsen, Direct Manipulation Techniques for 3D Objects Using 2D Locator Devices
    // (I3D '86), for exercising the loss of precision when the 3D motion projects to almost no screen motion.
    enum class GestureEnd : ludus::foundation::uint8
    {
        Commit,
        Escape,
        Collapsed,
    };
    struct RotateGesture final
    {
        const char* Name;
        float32 YawDegrees;
        float32 PitchDegrees;
        float32 ArcDegrees;
        RotateFixtureAxis Axis;
        bool IsPositive;
        GestureEnd End;
    };
    const QList<RotateGesture> gestures{
        {"first view X positive", 27, 22, 135, RotateFixtureAxis::X, true, GestureEnd::Commit},
        {"first view Z negative", 27, 22, 39, RotateFixtureAxis::Z, false, GestureEnd::Commit},
        {"second view X negative", -32, -25, 45, RotateFixtureAxis::X, false, GestureEnd::Commit},
        {"second view Z positive", -32, -25, 50, RotateFixtureAxis::Z, true, GestureEnd::Commit},
        {"five degrees before edge-on", 85, 0, 45, RotateFixtureAxis::Z, true, GestureEnd::Commit},
        {"five degrees past edge-on with Escape", 95, 0, 45, RotateFixtureAxis::Z, false, GestureEnd::Escape},
        {"edge-on at the collapsed Z tangent", 90, 0, 90, RotateFixtureAxis::Z, false, GestureEnd::Collapsed},
    };
    for (const RotateGesture& gesture : gestures)
    {
        DYNAMIC_SECTION(gesture.Name)
        {
            QTemporaryDir directory;
            const QString path = Fixture(directory);
            const QString childId = WriteNestedRotateWorld(directory, path);
            WorldWorkspace workspace;
            workspace.SetProject(directory.path(), 1);
            REQUIRE(workspace.Open(path) == true);
            REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.CanSave(); },
                                    WORLD_ROTATE_TIMEOUT_MILLISECONDS) == true);
            LevelWorkspace* levelOrNull = workspace.findChild<LevelWorkspace*>();
            REQUIRE(levelOrNull != nullptr);
            QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldProperties"));
            REQUIRE(propertiesOrNull != nullptr);
            propertiesOrNull->hide();
            REQUIRE(propertiesOrNull->isHidden() == true);

            const QRectF frame{0, 0, 800, 600};
            const PreviewCamera view = MakeRotateFixtureCamera(gesture.YawDegrees, gesture.PitchDegrees);
            RenderPreview& preview = levelOrNull->GetRenderPreview();
            RenderScene scene = preview.Scene();
            const math::Affine3 original = FindRotateFixtureTransform(scene, childId);
            scene.Identity += QByteArrayLiteral("-nested-rotation-camera");
            scene.Cameras = {view};
            ConfigureInteractionPreview(preview, scene);
            REQUIRE(workspace.SelectEntity(childId) == true);

            PreviewViewport& viewport = levelOrNull->GetViewport();
            ViewportObjectInteraction& interaction = viewport.GetObjectInteraction();
            interaction.SetActive(true);
            interaction.ClearPresentedView();
            interaction.SetImageSize({});
            interaction.SetGpuOutlineEnabled(true);
            interaction.SetTool(ViewportObjectInteraction::Tool::Rotate);
            QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);

            const math::Vector3 probeMesh = MakeRotateFixtureProbe(gesture.Axis, gesture.ArcDegrees, frame);
            const auto projectAuthored = [&](float32 deltaDegrees) {
                math::Vector3 eulerDegrees = NESTED_ROTATE_EULER_DEGREES;
                (gesture.Axis == RotateFixtureAxis::X ? eulerDegrees.X : eulerDegrees.Z) += deltaDegrees;
                return ProjectRotateFixturePoint(TransformRotateFixturePointToWorld(probeMesh, eulerDegrees),
                                                 view.Sample,
                                                 frame);
            };
            const QPointF pressPixel = projectAuthored(0);
            const QPointF positiveTangentPixel = projectAuthored(1) - projectAuthored(-1);
            const QJsonObject before = workspace.Document().Draft();
            const WorldDocument::History& history = workspace.Document().GetHistory();
            const ludus::foundation::usize cursorBefore = history.GetCursor();
            const ludus::foundation::usize countBefore = history.GetCount();
            REQUIRE(interaction.Hover(pressPixel, frame) == true);
            const ludus::foundation::int32 axis = static_cast<ludus::foundation::int32>(gesture.Axis);
            REQUIRE(interaction.GetHoveredAxis() == axis);
            REQUIRE(interaction.Press(pressPixel, frame) == true);

            constexpr float64 DRAG_PIXELS = 6;
            const float64 tangentLengthPixels = QLineF(QPointF{}, positiveTangentPixel).length();
            if (gesture.End == GestureEnd::Collapsed)
            {
                constexpr float64 COLLAPSED_TANGENT_TOLERANCE_PIXELS = 0.01;
                REQUIRE(tangentLengthPixels < COLLAPSED_TANGENT_TOLERANCE_PIXELS);
                CHECK(interaction.IsDragging() == false);
                static_cast<void>(interaction.Move(pressPixel + QPointF(0, DRAG_PIXELS), false));
                CHECK(interaction.Release() == false);
                CHECK(committed.count() == 0);
                CHECK(workspace.Document().Draft() == before);
                CHECK(history.GetCursor() == cursorBefore);
                CHECK(history.GetCount() == countBefore);
                return;
            }

            REQUIRE(tangentLengthPixels > 1);
            const float64 dragSign = gesture.IsPositive == true ? 1 : -1;
            const QPointF motionPixel = positiveTangentPixel * (dragSign * DRAG_PIXELS / tangentLengthPixels);
            REQUIRE(interaction.IsDragging() == true);
            REQUIRE(interaction.Move(pressPixel + motionPixel, false) == true);
            CHECK(workspace.Document().Draft() == before);
            CHECK(history.GetCursor() == cursorBefore);
            ViewportObjectOverlay overlay;
            interaction.BuildOverlay(frame, overlay);
            const QString axisName = gesture.Axis == RotateFixtureAxis::X ? QStringLiteral("X") : QStringLiteral("Z");
            const QString sign = gesture.IsPositive == true ? QStringLiteral(" +") : QStringLiteral(" -");
            INFO(overlay.ReadoutText.toStdString());
            REQUIRE(overlay.ReadoutText.startsWith(axisName + sign) == true);
            bool isReadoutNumeric = false;
            const float64 readoutDegrees = overlay.ReadoutText.mid(2).chopped(1).toDouble(&isReadoutNumeric);
            REQUIRE(isReadoutNumeric == true);
            constexpr float64 MAXIMUM_SMALL_DRAG_DEGREES = 6;
            CHECK(math::IsFinite(readoutDegrees) == true);
            CHECK(math::Abs(readoutDegrees) <= MAXIMUM_SMALL_DRAG_DEGREES);

            SelectionOutlineRequest outline;
            REQUIRE(interaction.TryGetGpuOutlineRequest(outline) == true);
            const math::Affine3 candidate = outline.MeshToWorld;
            const QPointF candidatePixel =
                ProjectRotateFixturePoint(math::TransformPoint(candidate, probeMesh), view.Sample, frame);
            CHECK(QPointF::dotProduct(candidatePixel - pressPixel, motionPixel) > 0);
            CHECK(candidate.Translation == original.Translation);

            if (gesture.End == GestureEnd::Escape)
            {
                QTest::keyClick(&viewport, Qt::Key_Escape);
                REQUIRE(interaction.IsDragging() == false);
                CHECK(interaction.Release() == false);
                CHECK(committed.count() == 0);
                CHECK(workspace.Document().Draft() == before);
                CHECK(history.GetCount() == countBefore);
                CHECK(history.GetCursor() == cursorBefore);
                interaction.BuildOverlay(frame, overlay);
                CHECK(overlay.ReadoutText.isEmpty() == true);
                REQUIRE(interaction.TryGetGpuOutlineRequest(outline) == true);
                CheckRotateFixtureTransforms(outline.MeshToWorld, original);
                return;
            }

            const QJsonObject initialPlacement =
                workspace.Document().ScenePlacement(workspace.Document().Entity(childId));
            REQUIRE(interaction.Release() == true);
            REQUIRE(committed.count() == 1);
            CHECK(history.GetCursor() == cursorBefore + 1);
            CHECK(history.GetCount() == countBefore + 1);
            const QJsonObject acceptedPlacement =
                workspace.Document().ScenePlacement(workspace.Document().Entity(childId));
            const QString rotationKey = QStringLiteral("rotation");
            QJsonArray rotation = acceptedPlacement.value(rotationKey).toArray();
            const float64 value = rotation[axis].toDouble();
            const QJsonArray initialRotation = initialPlacement.value(rotationKey).toArray();
            const float64 change = value - initialRotation[axis].toDouble();
            CHECK(change * dragSign > 0);
            CHECK(math::IsFinite(change) == true);
            CHECK(math::Abs(change) <= MAXIMUM_SMALL_DRAG_DEGREES);
            CHECK(change == Catch::Approx(readoutDegrees).margin(0.051));
            constexpr float64 CANDIDATE_PIXEL_TOLERANCE = 0.005;
            const QPointF expectedCandidatePixel = projectAuthored(static_cast<float32>(change));
            CHECK(QLineF(candidatePixel, expectedCandidatePixel).length() < CANDIDATE_PIXEL_TOLERANCE);
            rotation[axis] = initialRotation[axis];
            QJsonObject restoredPlacement = acceptedPlacement;
            restoredPlacement.insert(rotationKey, rotation);
            CHECK(restoredPlacement == initialPlacement);
            REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.CanSave(); },
                                    WORLD_ROTATE_TIMEOUT_MILLISECONDS) == true);
            CheckRotateFixtureTransforms(FindRotateFixtureTransform(levelOrNull->PreviewScene(), childId), candidate);

            REQUIRE(workspace.Save() == true);
            REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.CanSave() && workspace.Dirty() == false; },
                                    WORLD_ROTATE_TIMEOUT_MILLISECONDS) == true);
            WorldWorkspace reopened;
            reopened.SetProject(directory.path(), 2);
            REQUIRE(reopened.Open(path) == true);
            REQUIRE(QTest::qWaitFor([&reopened]() { return reopened.CanSave(); }, WORLD_ROTATE_TIMEOUT_MILLISECONDS) ==
                    true);
            CHECK(reopened.Document().ScenePlacement(reopened.Document().Entity(childId)) == acceptedPlacement);
            const LevelWorkspace* reopenedLevelOrNull = reopened.findChild<LevelWorkspace*>();
            REQUIRE(reopenedLevelOrNull != nullptr);
            CheckRotateFixtureTransforms(FindRotateFixtureTransform(reopenedLevelOrNull->PreviewScene(), childId),
                                         candidate);

            REQUIRE(workspace.CanUndo() == true);
            workspace.Undo();
            CHECK(workspace.Document().Draft() == before);
            CHECK(history.GetCursor() == cursorBefore);
            REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.CanSave(); },
                                    WORLD_ROTATE_TIMEOUT_MILLISECONDS) == true);
            CheckRotateFixtureTransforms(FindRotateFixtureTransform(levelOrNull->PreviewScene(), childId), original);
        }
    }
}

TEST_CASE("World viewport mouse events select objects while Alt keeps camera navigation",
          "[editor][world][object-interaction]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    PreviewViewport viewport(&preview, nullptr, false);
    viewport.resize(1000, 700);
    viewport.show();
    QTest::qWait(10);
    ViewportObjectInteraction& interaction = viewport.GetObjectInteraction();
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    QSignalSpy selection(&interaction, &ViewportObjectInteraction::SelectionRequested);
    REQUIRE(viewport.layout() != nullptr);
    REQUIRE(viewport.layout()->count() >= 2);
    const QPoint centerPixel = viewport.layout()->itemAt(1)->geometry().center();
    if (interaction.IsActive() == false)
    {
        QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, centerPixel);
        CHECK(selection.count() == 0);
        QComboBox* toolOrNull = viewport.findChild<QComboBox*>(QStringLiteral("viewportObjectTool"));
        REQUIRE(toolOrNull != nullptr);
        CHECK(toolOrNull->isEnabled() == false);
    }
    // Inject admission for this input-proxy test; no GPU image is needed to test event coordinates.
    interaction.SetActive(true);
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, centerPixel);
    REQUIRE(selection.count() == 1);
    CHECK(selection[0][0].toString() == scene.Objects[0].Id);
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::AltModifier, centerPixel);
    CHECK(selection.count() == 1);
    QTest::keyClick(&viewport, Qt::Key_E);
    CHECK(interaction.GetTool() == ViewportObjectInteraction::Tool::Rotate);
    QTest::keyClick(&viewport, Qt::Key_R);
    CHECK(interaction.GetTool() == ViewportObjectInteraction::Tool::Scale);
    const QRectF framePixel{0, 0, 800, 600};
    const QPointF handlePixel = framePixel.center() + QPointF(70, 0);
    REQUIRE(interaction.Press(handlePixel, framePixel) == true);
    REQUIRE(interaction.IsDragging() == true);
    QTest::keyClick(&viewport, Qt::Key_Escape);
    CHECK(interaction.IsDragging() == false);
    CHECK(selection.count() == 1);
    QTest::keyClick(&viewport, Qt::Key_Escape);
    REQUIRE(selection.count() == 2);
    CHECK(selection[1][0].toString().isEmpty() == true);
}

TEST_CASE("Destroying a viewport renderer cancels its outstanding object gesture",
          "[editor][world][object-interaction]")
{
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const RenderScene scene = MakeInteractionScene();
    QObject owner;
    RenderPreview* preview = new RenderPreview(&owner);
    ConfigureInteractionPreview(*preview, scene);
    ViewportObjectInteraction interaction(*preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    const QRectF frame{0, 0, 800, 600};
    REQUIRE(interaction.Press(frame.center() + QPointF(70, 0), frame) == true);
    REQUIRE(interaction.Move(frame.center() + QPointF(110, 0), false) == true);
    delete preview;
    CHECK(interaction.IsActive() == false);
    CHECK(interaction.Release() == false);
    CHECK(interaction.Pick(frame.center(), frame).isEmpty() == true);
    CHECK(committed.count() == 0);
}

TEST_CASE("Viewport picking is independent of mesh units and instance scale", "[editor][world][object-interaction]")
{
    RenderScene scene = MakeInteractionScene();
    for (ludus::foundation::math::Vector3& positionMesh : scene.Meshes[0].Positions)
    {
        positionMesh = positionMesh * 0.00001F;
    }
    for (ludus::foundation::math::Vector3& axisWorld : scene.Objects[0].Transform.Columns)
    {
        axisWorld = axisWorld * 100000;
    }
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    const QRectF frame{0, 0, 800, 600};
    CHECK(interaction.Pick(frame.center(), frame) == scene.Objects[0].Id);
}

TEST_CASE("Viewport overlay projection reuses bounded buffers and follows gestures and activation",
          "[editor][world][object-interaction][overlay]")
{
    using namespace ludus::foundation;
    WorldDocument document = UnboundDocument();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    RenderScene scene = MakeInteractionScene();
    // Exceed the outline budget with disjoint triangles, each of which has three open edges.
    constexpr usize TRIANGLE_EDGES = 3;
    constexpr usize DENSE_TRIANGLES = ViewportObjectOverlay::MAX_OUTLINE_LINES / TRIANGLE_EDGES + 1;
    const ludus::foundation::math::Vector3 firstCorner = scene.Meshes[0].Positions[scene.Meshes[0].Indices[0]];
    scene.Meshes[0].Positions.clear();
    scene.Meshes[0].Indices.clear();
    for (usize triangle = 0; triangle < DENSE_TRIANGLES; ++triangle)
    {
        const ludus::foundation::float32 offset = static_cast<ludus::foundation::float32>(triangle) * 0.0001F;
        const ludus::foundation::uint32 base = static_cast<ludus::foundation::uint32>(scene.Meshes[0].Positions.size());
        scene.Meshes[0].Positions.append({firstCorner.X + offset, firstCorner.Y, firstCorner.Z});
        scene.Meshes[0].Positions.append({firstCorner.X + offset + 0.5F, firstCorner.Y, firstCorner.Z});
        scene.Meshes[0].Positions.append({firstCorner.X + offset, firstCorner.Y + 0.5F, firstCorner.Z});
        scene.Meshes[0].Indices.append({base, base + 1, base + 2});
    }
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    ViewportObjectOverlay overlay;
    const QRectF framePixel{0, 0, 800, 600};
    interaction.BuildOverlay(framePixel, overlay);
    REQUIRE(overlay.OutlineLinesPixel.IsEmpty() == false);
    CHECK(overlay.OutlineLinesPixel.GetSize() <= ViewportObjectOverlay::MAX_OUTLINE_LINES);
    REQUIRE(overlay.HandleStrokes.IsEmpty() == false);
    CHECK(overlay.HandleStrokes.GetSize() <= ViewportObjectOverlay::MAX_HANDLE_STROKES);
    const QLineF* wireStorage = overlay.OutlineLinesPixel.GetData();
    const ViewportHandleStroke* handleStorage = overlay.HandleStrokes.GetData();
    const QLineF originalPixel = overlay.OutlineLinesPixel[0];
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel.GetData() == wireStorage);
    CHECK(overlay.HandleStrokes.GetData() == handleStorage);
    CHECK(overlay.OutlineLinesPixel[0] == originalPixel);
    REQUIRE(interaction.Press(framePixel.center() + QPointF(70, 0), framePixel) == true);
    REQUIRE(interaction.Move(framePixel.center() + QPointF(110, 0), false) == true);
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel[0] != originalPixel);
    CHECK(overlay.OutlineLinesPixel.GetData() == wireStorage);
    interaction.Cancel();
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel[0] == originalPixel);
    interaction.SetImageSize({800, 800});
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.FramePixel == QRectF(100, 0, 600, 600));
    interaction.SetActive(false);
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel.IsEmpty() == true);
    CHECK(overlay.HandleStrokes.IsEmpty() == true);
    CHECK(overlay.FramePixel.isEmpty() == true);
    CHECK(overlay.OutlineLinesPixel.GetData() == wireStorage);
    interaction.SetActive(true);
    interaction.SetWorld(document, scene.Objects[0].Id, false);
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel.IsEmpty() == false);
    CHECK(overlay.HandleStrokes.IsEmpty() == true);
}

TEST_CASE("Default World presets create renderable geometry and authored lights in one undo step",
          "[editor][world][creation]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    const QString path = QDir(root).filePath(QStringLiteral("objects.world.json"));
    REQUIRE(workspace.Create(path) == true);
    const QJsonObject empty = workspace.Document().Draft();
    for (const QString& preset : {QStringLiteral("Cube"),
                                  QStringLiteral("Plane"),
                                  QStringLiteral("Sphere"),
                                  QStringLiteral("Point Light"),
                                  QStringLiteral("Spot Light"),
                                  QStringLiteral("Directional Light")})
    {
        QToolButton* button = workspace.findChild<QToolButton*>(QStringLiteral("worldAddObject"));
        REQUIRE(button != nullptr);
        QAction* action = button->menu()->findChild<QAction*>(QStringLiteral("worldCreate") +
                                                              QString(preset).remove(QLatin1Char(' ')));
        REQUIRE(action != nullptr);
        action->trigger();
        const QJsonArray entities = workspace.Document().Draft().value(QStringLiteral("entities")).toArray();
        REQUIRE(entities.size() == 1);
        const QJsonObject entity = entities[0].toObject();
        CHECK(entity.value(QStringLiteral("name")).toString() == preset);
        CHECK(entity.value(QStringLiteral("components")).toObject().contains(QStringLiteral("transform")) == true);
        RenderScene scene;
        QString error;
        INFO(error.toStdString());
        REQUIRE(RenderScene::Load(directory.path(), workspace.Document(), scene, error) == true);
        if (preset.endsWith(QStringLiteral("Light")) == true)
        {
            REQUIRE(scene.AuthoredLights.Count == 1);
            const ludus::graphics::renderer::LightKind expectedKind =
                preset == QStringLiteral("Point Light")  ? ludus::graphics::renderer::LightKind::Point
                : preset == QStringLiteral("Spot Light") ? ludus::graphics::renderer::LightKind::Spot
                                                         : ludus::graphics::renderer::LightKind::Directional;
            CHECK(scene.AuthoredLights.Lights[0].Kind == expectedKind);
            CHECK(scene.Objects.isEmpty() == true);
        }
        else
        {
            REQUIRE(scene.Objects.size() == 1);
            CHECK(scene.Meshes[scene.Objects[0].Mesh].Id == QStringLiteral("mesh.") + preset.toLower());
            CHECK(scene.Meshes[scene.Objects[0].Mesh].Indices.isEmpty() == false);
            CHECK(scene.AuthoredLights.Count == 0);
        }
        workspace.Undo();
        CHECK(workspace.Document().Draft() == empty);
        workspace.Redo();
        CHECK(workspace.Document().Draft().value(QStringLiteral("entities")).toArray() == entities);
        workspace.Undo();
    }
    QString error;
    workspace.findChild<QAction*>(QStringLiteral("worldCreateCube"))->trigger();
    workspace.findChild<QAction*>(QStringLiteral("worldCreatePointLight"))->trigger();
    RenderScene litScene;
    REQUIRE(RenderScene::Load(directory.path(), workspace.Document(), litScene, error) == true);
    CHECK(litScene.Objects.size() == 1);
    CHECK(litScene.AuthoredLights.Count == 1);
    RenderScene authored;
    REQUIRE(RenderScene::Load(directory.path(), workspace.Document(), authored, error, false) == true);
    CHECK(authored.Definition == nullptr);
    REQUIRE(authored.Meshes.isEmpty() == false);
    CHECK(authored.Meshes[0].Positions.isEmpty() == false);
    QJsonObject level = ImportedLevel();
    level.insert(QStringLiteral("objects"), QJsonArray{});
    RenderScene composed;
    REQUIRE(RenderScene::Merge(authored, {}, level, composed, error, {}) == true);
    CHECK(composed.Objects.size() == 1);
    CHECK(composed.AuthoredLights.Count == 1);
    REQUIRE(workspace.Save() == true);
    WorldWorkspace reopened;
    reopened.SetProject(root, 2);
    REQUIRE(reopened.Open(path) == true);
    CHECK(reopened.Document().Draft() == workspace.Document().Draft());
}

TEST_CASE("A dark imported World offers a sun aimed like its disabled light, added as one undoable edit",
          "[editor][world][world-lighting]")
{
    using namespace ludus::foundation;
    namespace math = ludus::foundation::math;
    // An imported floor 20 m square lit only by a small emitter, with a sun authored at zero intensity like the Intel
    // Sponza glTF's.
    SharedPtr<ludus::resources::SceneDefinition> definition;
    REQUIRE(TryMakeShared<ludus::resources::SceneDefinition>(GetSystemAllocationDomain(), definition) ==
            OwnershipStatus::Ok);
    REQUIRE(definition->Meshes.TryResize(2) == true);
    for (const math::Vector3 corner :
         {math::Vector3{-10, 0, -10}, math::Vector3{10, 0, -10}, math::Vector3{10, 0, 10}, math::Vector3{-10, 0, 10}})
    {
        REQUIRE(definition->Meshes[0].Positions.TryAdd(corner) == true);
    }
    for (const uint32 index : {0U, 1U, 2U, 0U, 2U, 3U})
    {
        REQUIRE(definition->Meshes[0].Indices.TryAdd(index) == true);
    }
    // A 0.1 m right triangle 2 m up, of area 0.005 m2.
    for (const math::Vector3 corner : {math::Vector3{0, 2, 0}, math::Vector3{0.1F, 2, 0}, math::Vector3{0, 2, 0.1F}})
    {
        REQUIRE(definition->Meshes[1].Positions.TryAdd(corner) == true);
    }
    for (const uint32 index : {0U, 1U, 2U})
    {
        REQUIRE(definition->Meshes[1].Indices.TryAdd(index) == true);
    }
    ludus::resources::SceneLight sun;
    sun.SourceId = 1;
    sun.Kind = ludus::resources::SceneLightKind::Directional;
    sun.IsEnabled = false;
    sun.DirectionWorld = {1.1011403F, -1.6562406F, 0.2106133F};
    REQUIRE(definition->Lights.TryAdd(sun) == true);
    RenderScene scene;
    scene.Definition = definition;
    scene.Lit = true;
    scene.Meshes.resize(2);
    scene.Surfaces.resize(2);
    scene.Surfaces[1].Emission = {1, 1, 1};
    for (int32 index = 0; index < 2; ++index)
    {
        PreviewObject object;
        object.Id = QStringLiteral("object-%1").arg(index);
        object.Mesh = index;
        object.Surface = index;
        scene.Objects.append(object);
    }
    const WorldLightingSummary summary = SummarizeWorldLighting(scene, true);
    REQUIRE(summary.HasBounds == true);
    CHECK(summary.LightCount == 0);
    CHECK(summary.DisabledLightCount == 1);
    CHECK(summary.HasEnvironment == false);
    CHECK(summary.EmissiveObjectCount == 1);
    // One-sided radiance one over 0.005 m2.
    CHECK(summary.EmittedLumens == Catch::Approx(math::kPi * 0.005).epsilon(1e-4));
    REQUIRE(summary.HasSunDirection == true);
    CHECK(math::Length(summary.SunDirectionWorld) == Catch::Approx(1).epsilon(1e-5));
    const WorldLightingCoverage coverage = DescribeWorldLighting(summary);
    INFO(coverage.Warning.toStdString());
    REQUIRE(coverage.Repair == WorldLightingRepair::AddSun);
    // An unlit preview never reports the World dark.
    scene.Lit = false;
    CHECK(DescribeWorldLighting(SummarizeWorldLighting(scene, true)).Warning.isEmpty() == true);

    // The repair adds a sun that the World extraction presents along the disabled light's direction.
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("sun.world.json"))) == true);
    const QJsonObject before = workspace.Document().Draft();
    REQUIRE(workspace.ApplySunRepair(coverage) == true);
    const QJsonArray entities = workspace.Document().Draft().value(QStringLiteral("entities")).toArray();
    REQUIRE(entities.size() == 1);
    CHECK(entities[0].toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Sun"));
    RenderScene authored;
    QString error;
    REQUIRE(RenderScene::Load(root, workspace.Document(), authored, error) == true);
    REQUIRE(authored.AuthoredLights.Count == 1);
    const ludus::graphics::renderer::DirectLight& light = authored.AuthoredLights.Lights[0];
    CHECK(light.Kind == ludus::graphics::renderer::LightKind::Directional);
    CHECK(light.Intensity == Catch::Approx(DEFAULT_WORLD_SUN_ILLUMINANCE_LUX));
    math::Vector3 direction;
    REQUIRE(math::TryNormalize(light.Direction, direction) == math::MathStatus::Success);
    CHECK(direction.X == Catch::Approx(summary.SunDirectionWorld.X).margin(1e-4));
    CHECK(direction.Y == Catch::Approx(summary.SunDirectionWorld.Y).margin(1e-4));
    CHECK(direction.Z == Catch::Approx(summary.SunDirectionWorld.Z).margin(1e-4));
    workspace.Undo();
    CHECK(workspace.Document().Draft() == before);
    // A repair that offers nothing changes nothing.
    CHECK(workspace.ApplySunRepair(WorldLightingCoverage{}) == false);
    CHECK(workspace.Document().Draft() == before);

    // Add World Preset offers the same sun. This World has no imported light to follow, so it takes the default
    // angle, two metres above the origin.
    QAction* createOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateSun"));
    REQUIRE(createOrNull != nullptr);
    createOrNull->trigger();
    const QJsonArray created = workspace.Document().Draft().value(QStringLiteral("entities")).toArray();
    REQUIRE(created.size() == 1);
    const QJsonObject components = created[0].toObject().value(QStringLiteral("components")).toObject();
    CHECK(components.value(QStringLiteral("light")).toObject().value(QStringLiteral("kind")).toString() ==
          QStringLiteral("directional"));
    const QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    CHECK(transform.value(QStringLiteral("rx")).toDouble() == Catch::Approx(DEFAULT_WORLD_SUN_ROTATION_DEGREES.X));
    CHECK(transform.value(QStringLiteral("ry")).toDouble() == Catch::Approx(DEFAULT_WORLD_SUN_ROTATION_DEGREES.Y));
    CHECK(transform.value(QStringLiteral("y")).toDouble() == Catch::Approx(2));
    workspace.Undo();
    CHECK(workspace.Document().Draft() == before);
    workspace.StopPreview();
}

TEST_CASE("First Person Player preset persists a spawn and uses the imported start view",
          "[editor][world][first-person]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    const QString path = QDir(root).filePath(QStringLiteral("player.world.json"));
    REQUIRE(workspace.Create(path) == true);
    QAction* createOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateFirstPersonPlayer"));
    REQUIRE(createOrNull != nullptr);
    createOrNull->trigger();
    const QJsonArray entities = workspace.Document().Draft().value(QStringLiteral("entities")).toArray();
    REQUIRE(entities.size() == 1);
    const QString id = workspace.Document().GetEntityKey(entities[0].toObject());
    CHECK(HasWorldFirstPersonPlayer(workspace.Document(), id) == true);
    const QPushButton* exploreOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldExploreFirstPerson"));
    REQUIRE(exploreOrNull != nullptr);
    CHECK(exploreOrNull->isHidden() == false);
    CHECK(exploreOrNull->isEnabled() == true);
    workspace.Undo();
    CHECK(HasWorldFirstPersonPlayer(workspace.Document(), id) == false);
    workspace.Redo();
    REQUIRE(workspace.Save() == true);
    REQUIRE(workspace.Open(path) == true);

    namespace camera = ludus::gameplay::camera;
    namespace math = ludus::foundation::math;
    camera::CameraSample spawn;
    ludus::foundation::float32 speed = 0;
    QString error;
    error = QStringLiteral("previous diagnostic");
    REQUIRE(ReadWorldFirstPersonPlayer(workspace.Document(), id, spawn, speed, error) == true);
    CHECK(error.isEmpty() == true);
    CHECK(spawn.Pose.Position.Y == Catch::Approx(1.65));
    CHECK(speed == Catch::Approx(3));
    workspace.StopPreview();

    const QString source = QDir(root).filePath(QStringLiteral("source"));
    REQUIRE(tests::CreateFixtureSource(source) == true);
    REQUIRE(tests::PrepareImportedLevel(root, QStringLiteral("room.lulevel"), source, error) == true);
    QJsonObject level = QJsonDocument::fromJson(Read(QDir(root).filePath(QStringLiteral("room.lulevel")))).object();
    const PreviewStartView start
    {
        .EyeWorld = {1, 2, 3},
        .TargetWorld = {2, 2, 3},
    };
    level.insert(QStringLiteral("camera"), MakeStartViewJson(start));
    WorldDocument imported = workspace.Document();
    REQUIRE(imported.ImportScene(level, error) == true);
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString importedId;
    REQUIRE(CreateWorldObject(imported, entropy, QStringLiteral("First Person Player"), importedId, error) == true);
    REQUIRE(ReadWorldFirstPersonPlayer(imported, importedId, spawn, speed, error) == true);
    CHECK(spawn.Pose.Position == start.EyeWorld);
    const math::Vector3 forwardWorld = math::Rotate(spawn.Pose.Orientation, {0, 0, -1});
    CHECK(forwardWorld.X == Catch::Approx(1));
    CHECK(forwardWorld.Z == Catch::Approx(0).margin(1e-6));
}

TEST_CASE("World Properties attaches Static Collider to selected meshes in one edit", "[editor][world][first-person]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("colliders.world.json"))) == true);
    QAction* cubeOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateCube"));
    QAction* planeOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreatePlane"));
    QAction* playerOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateFirstPersonPlayer"));
    REQUIRE(cubeOrNull != nullptr);
    REQUIRE(planeOrNull != nullptr);
    REQUIRE(playerOrNull != nullptr);
    cubeOrNull->trigger();
    planeOrNull->trigger();
    playerOrNull->trigger();
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    QPushButton* attachOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldAttach"));
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(attachOrNull != nullptr);
    const QString component = GetStaticColliderComponentId();
    CHECK(workspace.findChild<QAction*>(QStringLiteral("worldAttach:") + component) == nullptr);
    const QJsonObject before = workspace.Document().Draft();
    entitiesOrNull->selectAll();
    QAction* colliderOrNull = workspace.findChild<QAction*>(QStringLiteral("worldAttach:") + component);
    REQUIRE(colliderOrNull != nullptr);
    REQUIRE(attachOrNull->isEnabled() == true);
    colliderOrNull->trigger();
    for (const QJsonValue value : workspace.Document().Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject components = value.toObject().value(QStringLiteral("components")).toObject();
        CHECK(components.contains(component) == components.contains(QStringLiteral("renderable")));
    }
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(workspace.Document().Draft() == before);
    workspace.StopPreview();
}

TEST_CASE("A meshless First Person Player shows its collision box and can be framed",
          "[editor][world][first-person][framing][overlay]")
{
    using namespace ludus::foundation;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("player-bounds.world.json"))) == true);
    workspace.resize(1100, 720);
    workspace.show();
    QApplication::processEvents();

    QAction* createOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateFirstPersonPlayer"));
    PreviewViewport* viewportOrNull = workspace.findChild<PreviewViewport*>(QStringLiteral("worldViewport"));
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(createOrNull != nullptr);
    REQUIRE(viewportOrNull != nullptr);
    REQUIRE(entitiesOrNull != nullptr);
    createOrNull->trigger();
    REQUIRE(entitiesOrNull->topLevelItemCount() == 1);
    REQUIRE(QTest::qWaitFor([&workspace]() { return workspace.CanSave() == true; }) == true);
    const QTreeWidgetItem* playerOrNull = entitiesOrNull->topLevelItem(0);
    REQUIRE(playerOrNull != nullptr);
    const QString id = playerOrNull->data(0, Qt::UserRole).toString();
    RenderPreview& preview = workspace.GetRenderPreview();
    math::Aabb3 boundsWorld;
    CHECK(preview.Scene().TryGetBounds(id, boundsWorld) == false);
    const QJsonObject draft = workspace.Document().Draft();

    SECTION("frame the player from the viewport keyboard")
    {
        QTest::keyClick(viewportOrNull, Qt::Key_F);
    }
    SECTION("frame the player from the hierarchy")
    {
        const QPoint rowPixel = entitiesOrNull->visualItemRect(playerOrNull).center();
        QTest::mouseClick(entitiesOrNull->viewport(), Qt::LeftButton, Qt::NoModifier, rowPixel);
        tests::DoubleClick(entitiesOrNull->viewport(), Qt::LeftButton, Qt::NoModifier, rowPixel);
    }

    PreviewStartView view;
    REQUIRE(preview.TryDescribeStartView(view) == true);
    // The default eye is 1.65 m above the World origin and 1.60 m above the box base.
    constexpr float64 TARGET_HEIGHT_METRES = 0.925;
    constexpr float64 FRAME_TOLERANCE_METRES = 0.001;
    CHECK(view.TargetWorld.X == Catch::Approx(0).margin(FRAME_TOLERANCE_METRES));
    CHECK(view.TargetWorld.Y == Catch::Approx(TARGET_HEIGHT_METRES).margin(FRAME_TOLERANCE_METRES));
    CHECK(view.TargetWorld.Z == Catch::Approx(0).margin(FRAME_TOLERANCE_METRES));

    ViewportObjectInteraction& interaction = viewportOrNull->GetObjectInteraction();
    // Offscreen has no presented GPU image. Exercise the production overlay geometry.
    interaction.SetActive(true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    interaction.SetGpuOutlineEnabled(true);
    const QRectF framePixel{0, 0, 800, 600};
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(framePixel, overlay);
    constexpr usize COLLISION_BOX_EDGES = 12;
    CHECK(overlay.OutlineLinesPixel.GetSize() == COLLISION_BOX_EDGES);
    SelectionOutlineRequest meshOutline;
    CHECK(interaction.TryGetGpuOutlineRequest(meshOutline) == false);

    entitiesOrNull->clearSelection();
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel.IsEmpty() == true);
    CHECK(workspace.Document().Draft() == draft);
}

TEST_CASE("Selecting a First Person Player keeps the bottom of the World entity list visible",
          "[editor][world][first-person]")
{
    using ludus::foundation::int32;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("player.world.json"))) == true);
    QAction* addOrNull = workspace.findChild<QAction*>(QStringLiteral("worldAdd"));
    QAction* createPlayerOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateFirstPersonPlayer"));
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    const QPushButton* exploreOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldExploreFirstPerson"));
    REQUIRE(addOrNull != nullptr);
    REQUIRE(createPlayerOrNull != nullptr);
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(exploreOrNull != nullptr);

    // Row height follows the platform font (Linux offscreen rows are shorter than macOS rows), so add
    // entities until the shown list scrolls instead of assuming a count that overflows everywhere.
    constexpr int32 MAXIMUM_ENTITY_COUNT_BEFORE_PLAYER = 200;
    workspace.resize(1100, 720);
    workspace.show();
    QApplication::processEvents();
    int32 entityCountBeforePlayer = 0;
    while (entityCountBeforePlayer < MAXIMUM_ENTITY_COUNT_BEFORE_PLAYER &&
           (entityCountBeforePlayer == 0 || entitiesOrNull->verticalScrollBar()->maximum() == 0))
    {
        addOrNull->trigger();
        ++entityCountBeforePlayer;
        QApplication::processEvents();
    }
    REQUIRE(entitiesOrNull->verticalScrollBar()->maximum() > 0);
    createPlayerOrNull->trigger();
    REQUIRE(entitiesOrNull->topLevelItemCount() == entityCountBeforePlayer + 1);
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(entityCountBeforePlayer - 1));
    QApplication::processEvents();
    entitiesOrNull->scrollToBottom();
    const QTreeWidgetItem* playerOrNull = entitiesOrNull->topLevelItem(entityCountBeforePlayer);
    REQUIRE(playerOrNull != nullptr);
    REQUIRE(playerOrNull->text(0) == QStringLiteral("First Person Player"));
    REQUIRE(exploreOrNull->isHidden() == true);
    REQUIRE(entitiesOrNull->visualItemRect(entitiesOrNull->topLevelItem(0)).bottom() < 0);
    const int32 listHeight = entitiesOrNull->height();
    const QRect playerRow = entitiesOrNull->visualItemRect(playerOrNull);
    REQUIRE(entitiesOrNull->viewport()->rect().contains(playerRow) == true);

    QTest::mouseClick(entitiesOrNull->viewport(), Qt::LeftButton, Qt::NoModifier, playerRow.center());
    QApplication::processEvents();
    CHECK(playerOrNull->isSelected() == true);
    CHECK(exploreOrNull->isVisible() == true);
    CHECK(exploreOrNull->isEnabled() == true);
    CHECK(entitiesOrNull->height() == listHeight);
    const QRect selectedRow = entitiesOrNull->visualItemRect(playerOrNull);
    CHECK(selectedRow.top() == playerRow.top());
    CHECK(entitiesOrNull->viewport()->rect().contains(selectedRow) == true);
    workspace.StopPreview();
}

TEST_CASE("Explore as Player moves the rendered staircase2 World camera", "[.first-person-native][editor][world]")
{
    using namespace ludus::foundation;
    const QString source = qEnvironmentVariable("LUDUS_STAGE5_REFERENCE_SOURCE");
    if (source.isEmpty() == true || PreviewViewport::CanPresent() == false)
    {
        SKIP("Set LUDUS_STAGE5_REFERENCE_SOURCE to staircase2 and run with the native viewport.");
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    QString error;
    REQUIRE(tests::PrepareImportedLevel(root, QStringLiteral("room.lulevel"), source, error, "scene-v4.pbrt") == true);
    const QJsonObject level =
        QJsonDocument::fromJson(Read(QDir(root).filePath(QStringLiteral("room.lulevel")))).object();
    PreviewStartView start;
    REQUIRE(TryParseStartView(level.value(QStringLiteral("camera")).toObject(), start) == true);
    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    REQUIRE(document.ImportScene(level, error) == true);
    QStringList colliderIds;
    for (const QJsonValue value : level.value(QStringLiteral("objects")).toArray())
    {
        colliderIds.append(value.toObject().value(QStringLiteral("id")).toString());
    }
    REQUIRE(document.AttachToEntities(colliderIds, error, GetStaticColliderComponentId()) == true);
    const QString path = QDir(root).filePath(QStringLiteral("staircase2.world.json"));
    Write(path, document.Encode());

    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    WorldWorkspace workspace(tools, nullptr);
    workspace.SetProject(root, 1);
    workspace.resize(1000, 700);
    workspace.show();
    REQUIRE(workspace.Open(path) == true);
    constexpr int32 TIMEOUT_MILLISECONDS = 30000;
    constexpr int32 POLL_MILLISECONDS = 10;
    QElapsedTimer timer;
    timer.start();
    RenderPreview& preview = workspace.GetRenderPreview();
    while ((workspace.CanSave() == false || preview.Frames() == 0) && timer.elapsed() < TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    REQUIRE(workspace.CanSave() == true);
    REQUIRE(preview.Frames() != 0);
    REQUIRE(preview.Scene().Objects.isEmpty() == false);
    QAction* createOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateFirstPersonPlayer"));
    REQUIRE(createOrNull != nullptr);
    createOrNull->trigger();
    timer.restart();
    while (workspace.CanSave() == false && timer.elapsed() < TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    REQUIRE(workspace.CanSave() == true);
    QPushButton* exploreOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldExploreFirstPerson"));
    REQUIRE(exploreOrNull != nullptr);
    REQUIRE(exploreOrNull->isEnabled() == true);
    const QJsonObject editorNavigation = preview.Navigation();
    const QByteArray worldBytes = workspace.Document().Encode();
    exploreOrNull->click();
    REQUIRE(preview.IsExploringFirstPerson() == true);
    CHECK(preview.GetVisibleCameraSample().Pose.Position == start.EyeWorld);
    const uint64 framesBeforeMove = preview.Frames();
    preview.AdvanceFirstPerson({0, 0, -1}, 0.1F, 3, false);
    // Advance without dispatching Qt events so gravity settles before checking the rendered camera.
    for (uint32 frame = 0; frame < 12; ++frame)
    {
        preview.AdvanceFirstPerson({}, 0.1F, 3, false);
    }
    const math::Vector3d movedWorld = preview.GetVisibleCameraSample().Pose.Position;
    CHECK((movedWorld.X != start.EyeWorld.X || movedWorld.Z != start.EyeWorld.Z));
    CHECK(movedWorld.Y < start.EyeWorld.Y);
    const math::Vector3 renderedPositionWorld{static_cast<float32>(movedWorld.X),
                                              static_cast<float32>(movedWorld.Y),
                                              static_cast<float32>(movedWorld.Z)};
    timer.restart();
    while (
        (preview.Frames() <= framesBeforeMove || preview.ViewDescription().CameraPosition != renderedPositionWorld) &&
        timer.elapsed() < TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    CHECK(preview.Frames() > framesBeforeMove);
    CHECK(preview.ViewDescription().CameraPosition == renderedPositionWorld);
    LevelWorkspace* levelOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(levelOrNull != nullptr);
    QTest::keyClick(&levelOrNull->GetViewport(), Qt::Key_Escape);
    CHECK(preview.IsExploringFirstPerson() == false);
    CHECK(preview.Navigation() == editorNavigation);
    CHECK(workspace.Document().Encode() == worldBytes);
    workspace.StopPreview();
}

TEST_CASE("Directional celestial roles round-trip and Earth drivers sample the light transform",
          "[editor][world][celestial]")
{
    using namespace ludus::foundation;
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString path = QDir(root).filePath(QStringLiteral("celestial.world.json"));
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    QAction* sunOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateSun"));
    REQUIRE(sunOrNull != nullptr);
    sunOrNull->trigger();

    const QJsonArray entities = workspace.Document().Draft().value(QStringLiteral("entities")).toArray();
    REQUIRE(entities.size() == 1);
    const QJsonObject entity = entities[0].toObject();
    const QString id = workspace.Document().GetEntityKey(entity);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    QJsonObject light = components.value(QStringLiteral("light")).toObject();
    CHECK(light.value(WORLD_CELESTIAL_ROLE).toString() == QStringLiteral("Static Sun"));

    // A previously saved light has no celestial keys. Its authored direction and bytes remain valid.
    for (const QString& property : light.keys())
    {
        if (IsWorldCelestialProperty(property) == true)
        {
            light.remove(property);
        }
    }
    components.insert(QStringLiteral("light"), light);
    QJsonObject legacyEntity = entity;
    legacyEntity.insert(QStringLiteral("components"), components);
    QJsonObject legacy = workspace.Document().Draft();
    legacy.insert(QStringLiteral("entities"), QJsonArray{legacyEntity});
    Write(path, QJsonDocument(legacy).toJson());
    WorldWorkspace reopened;
    reopened.SetProject(root, 2);
    REQUIRE(reopened.Open(path) == true);
    REQUIRE(reopened.Document().Draft() == legacy);
    REQUIRE(reopened.SelectEntity(id) == true);

    QComboBox* kindOrNull = reopened.findChild<QComboBox*>(QStringLiteral("worldField:light/kind"));
    QComboBox* roleOrNull = reopened.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialRole"));
    QComboBox* driverOrNull = reopened.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialDriver"));
    QWidget* timeOrNull = reopened.findChild<QWidget*>(QStringLiteral("worldField:light/celestialTime"));
    QDateTimeEdit* dateTimeOrNull = reopened.findChild<QDateTimeEdit*>(QStringLiteral("worldEarthDateTime"));
    QCheckBox* overrideOrNull =
        reopened.findChild<QCheckBox*>(QStringLiteral("worldField:light/celestialManualPhotometry"));
    QLabel* calculatedOrNull = reopened.findChild<QLabel*>(QStringLiteral("worldEarthComputedPhotometry"));
    QLineEdit* intensityOrNull = reopened.findChild<QLineEdit*>(QStringLiteral("worldField:light/intensity"));
    REQUIRE(overrideOrNull != nullptr);
    REQUIRE(calculatedOrNull != nullptr);
    REQUIRE(intensityOrNull != nullptr);
    REQUIRE(kindOrNull != nullptr);
    REQUIRE(roleOrNull != nullptr);
    REQUIRE(driverOrNull != nullptr);
    REQUIRE(timeOrNull != nullptr);
    REQUIRE(dateTimeOrNull != nullptr);
    CHECK(roleOrNull->currentText() == QStringLiteral("None"));
    CHECK(driverOrNull->isHidden() == true);
    kindOrNull->setCurrentText(QStringLiteral("point"));
    CHECK(roleOrNull->isHidden() == true);
    kindOrNull->setCurrentText(QStringLiteral("directional"));
    CHECK(roleOrNull->isHidden() == false);

    RenderScene scene;
    QString error;
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    REQUIRE(scene.AuthoredLights.Count == 1);
    const math::Vector3 authoredDirectionWorld = scene.AuthoredLights.Lights[0].Direction;

    roleOrNull->setCurrentText(QStringLiteral("Static Moon"));
    REQUIRE(reopened.Apply() == true);
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    CHECK(scene.AuthoredLights.Lights[0].Direction == authoredDirectionWorld);
    CHECK(scene.AuthoredLights.Lights[0].Intensity > 0);

    roleOrNull->setCurrentText(QStringLiteral("Dynamic Celestial"));
    CHECK(driverOrNull->isHidden() == false);
    REQUIRE(reopened.Apply() == true);
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    CHECK(scene.AuthoredLights.Lights[0].Direction == authoredDirectionWorld);

    driverOrNull->setCurrentText(QStringLiteral("Earth Sun"));
    CHECK(timeOrNull->isHidden() == false);
    // The World's first Earth preset starts at the current time; pin the epoch for a deterministic sample.
    dateTimeOrNull->setDateTime(QDateTime::fromString(QStringLiteral("2000-01-01T12:00:00Z"), Qt::ISODate));
    REQUIRE(reopened.Apply() == true);
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    REQUIRE(scene.AuthoredLights.Count == 1);
    CHECK(scene.AuthoredLights.Lights[0].Direction != authoredDirectionWorld);
    CHECK(scene.AuthoredLights.Lights[0].Direction.Y < 0);
    CHECK(scene.AuthoredLights.Lights[0].Intensity > 100'000);
    CHECK(intensityOrNull->isEnabled() == false);
    CHECK(calculatedOrNull->isHidden() == false);
    const math::Vector3 solarNoonDirectionWorld = scene.AuthoredLights.Lights[0].Direction;

    driverOrNull->setCurrentText(QStringLiteral("Earth Moon"));
    REQUIRE(reopened.Apply() == true);
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    CHECK(scene.AuthoredLights.Lights[0].Direction != solarNoonDirectionWorld);
    CHECK(scene.AuthoredLights.Lights[0].Intensity < 0.4F);
    overrideOrNull->setChecked(true);
    CHECK(intensityOrNull->isEnabled() == true);
    intensityOrNull->setText(QStringLiteral("10"));
    REQUIRE(reopened.Apply() == true);
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    CHECK(scene.AuthoredLights.Lights[0].Intensity == 10);
    overrideOrNull->setChecked(false);
    REQUIRE(reopened.Apply() == true);
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    CHECK(scene.AuthoredLights.Lights[0].Intensity < 0.4F);

    roleOrNull->setCurrentText(QStringLiteral("Static Moon"));
    REQUIRE(reopened.Apply() == true);
    roleOrNull->setCurrentText(QStringLiteral("Dynamic Celestial"));
    driverOrNull->setCurrentText(QStringLiteral("Earth Sun"));
    dateTimeOrNull->setDateTime(QDateTime::fromString(QStringLiteral("2000-01-02T00:00:00Z"), Qt::ISODate));
    REQUIRE(reopened.Apply() == true);
    REQUIRE(RenderScene::Load(root, reopened.Document(), scene, error) == true);
    CHECK(scene.AuthoredLights.Lights[0].Direction.Y > 0);
    CHECK(scene.AuthoredLights.Lights[0].Intensity == 0);
    REQUIRE(reopened.Save() == true);
    WorldDocument saved;
    REQUIRE(saved.LoadSchema(Read(QStringLiteral(":/world/world.schema.json")), error) == true);
    REQUIRE(saved.Load(Read(path), error) == true);
    CHECK(saved.Draft() == reopened.Document().Draft());
    const QJsonObject savedLight =
        saved.Entity(id).value(QStringLiteral("components")).toObject().value(QStringLiteral("light")).toObject();
    const QJsonObject savedObserver = saved.Draft().value(WORLD_EARTH_OBSERVER).toObject();
    CHECK(savedObserver.value(WORLD_CELESTIAL_TIME).toDouble() == 0.5);
    CHECK(savedObserver.value(WORLD_CELESTIAL_LATITUDE).toDouble() == 0);
    CHECK(savedObserver.value(WORLD_CELESTIAL_LONGITUDE).toDouble() == 0);
    // The World stores the Earth date, time and location once, not on the light.
    CHECK(savedLight.contains(WORLD_CELESTIAL_TIME) == false);
    CHECK(savedLight.contains(WORLD_CELESTIAL_LATITUDE) == false);
    CHECK(savedLight.contains(WORLD_CELESTIAL_LONGITUDE) == false);
    workspace.StopPreview();
    reopened.StopPreview();
}

TEST_CASE("Earth lights share one World date, time and location that starts at the current time",
          "[editor][world][celestial]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("shared-earth.world.json"))) == true);
    QAction* sunActionOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateSun"));
    QAction* atmosphereActionOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateAtmosphere"));
    REQUIRE(sunActionOrNull != nullptr);
    REQUIRE(atmosphereActionOrNull != nullptr);
    sunActionOrNull->trigger();
    const QJsonObject sunEntity =
        workspace.Document().Draft().value(QStringLiteral("entities")).toArray().last().toObject();
    const QString sunId = workspace.Document().GetEntityKey(sunEntity);
    const QString sunName = sunEntity.value(QStringLiteral("name")).toString();
    atmosphereActionOrNull->trigger();
    const QString atmosphereId = workspace.Document().GetEntityKey(
        workspace.Document().Draft().value(QStringLiteral("entities")).toArray().last().toObject());

    // A Sun with an authored direction uses the Atmosphere's disk radius.
    REQUIRE(workspace.SelectEntity(atmosphereId) == true);
    QComboBox* atmosphereSunOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:atmosphere/sun"));
    REQUIRE(atmosphereSunOrNull != nullptr);
    atmosphereSunOrNull->setCurrentIndex(atmosphereSunOrNull->findData(sunId));
    REQUIRE(workspace.Apply() == true);
    QWidget* radiusOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldField:atmosphere/solarRadius"));
    QLabel* skyOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldAtmosphereEarthObserver"));
    REQUIRE(radiusOrNull != nullptr);
    REQUIRE(skyOrNull != nullptr);
    CHECK(radiusOrNull->isVisibleTo(&workspace) == true);
    CHECK(skyOrNull->isVisibleTo(&workspace) == false);

    // The World's first Earth preset starts at the current UTC time.
    const ludus::foundation::float64 before = WorldEarthTimeField::GetCurrentDaysSinceEpoch();
    REQUIRE(workspace.SelectEntity(sunId) == true);
    QComboBox* roleOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialRole"));
    QComboBox* driverOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialDriver"));
    REQUIRE(roleOrNull != nullptr);
    REQUIRE(driverOrNull != nullptr);
    roleOrNull->setCurrentText(QStringLiteral("Dynamic Celestial"));
    driverOrNull->setCurrentText(QStringLiteral("Earth Sun"));
    QLabel* sharedOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldEarthSharedObserver"));
    REQUIRE(sharedOrNull != nullptr);
    CHECK(sharedOrNull->isVisibleTo(&workspace) == true);
    CHECK(sharedOrNull->text().startsWith(QStringLiteral("Every Earth Sun and Earth Moon light")) == true);
    REQUIRE(workspace.Apply() == true);
    const ludus::foundation::float64 after = WorldEarthTimeField::GetCurrentDaysSinceEpoch();
    const QJsonObject observer = workspace.Document().Draft().value(WORLD_EARTH_OBSERVER).toObject();
    const ludus::foundation::float64 sharedDays = observer.value(WORLD_CELESTIAL_TIME).toDouble();
    CHECK(sharedDays >= before);
    CHECK(sharedDays <= after);

    // A second Earth light shows and keeps the shared instant instead of starting its own clock.
    sunActionOrNull->trigger();
    const QJsonObject moonEntity =
        workspace.Document().Draft().value(QStringLiteral("entities")).toArray().last().toObject();
    const QString moonId = workspace.Document().GetEntityKey(moonEntity);
    REQUIRE(workspace.SelectEntity(moonId) == true);
    roleOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialRole"));
    driverOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialDriver"));
    WorldEarthTimeField* timeOrNull =
        workspace.findChild<WorldEarthTimeField*>(QStringLiteral("worldField:light/celestialTime"));
    REQUIRE(roleOrNull != nullptr);
    REQUIRE(driverOrNull != nullptr);
    REQUIRE(timeOrNull != nullptr);
    CHECK(timeOrNull->GetDaysSinceEpoch() == sharedDays);
    roleOrNull->setCurrentText(QStringLiteral("Dynamic Celestial"));
    driverOrNull->setCurrentText(QStringLiteral("Earth Moon"));
    sharedOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldEarthSharedObserver"));
    REQUIRE(sharedOrNull != nullptr);
    CHECK(sharedOrNull->text().startsWith(QStringLiteral("Shared with %1.").arg(sunName)) == true);
    REQUIRE(workspace.Apply() == true);
    CHECK(workspace.Document().Draft().value(WORLD_EARTH_OBSERVER).toObject() == observer);

    // Changing the date on the Moon changes it for the Sun and the night sky.
    QDateTimeEdit* dateTimeOrNull = timeOrNull->findChild<QDateTimeEdit*>(QStringLiteral("worldEarthDateTime"));
    REQUIRE(dateTimeOrNull != nullptr);
    dateTimeOrNull->setDateTime(QDateTime::fromString(QStringLiteral("2000-01-02T00:00:00Z"), Qt::ISODate));
    REQUIRE(workspace.Apply() == true);
    CHECK(workspace.Document().Draft().value(WORLD_EARTH_OBSERVER).toObject().value(WORLD_CELESTIAL_TIME).toDouble() ==
          0.5);
    for (const QJsonValue entity : workspace.Document().Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject light =
            entity.toObject().value(QStringLiteral("components")).toObject().value(QStringLiteral("light")).toObject();
        CHECK(light.contains(WORLD_CELESTIAL_TIME) == false);
    }

    // The Earth Sun preset sets its own disk size, and the Atmosphere names the shared instant.
    REQUIRE(workspace.SelectEntity(atmosphereId) == true);
    radiusOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldField:atmosphere/solarRadius"));
    skyOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldAtmosphereEarthObserver"));
    REQUIRE(radiusOrNull != nullptr);
    REQUIRE(skyOrNull != nullptr);
    CHECK(radiusOrNull->isVisibleTo(&workspace) == false);
    CHECK(skyOrNull->isVisibleTo(&workspace) == true);
    CHECK(skyOrNull->text().contains(QStringLiteral("2000-01-02 00:00:00 UTC")) == true);
    workspace.StopPreview();
}

TEST_CASE("The Atmosphere shows enabled layer fields and extracts their settings", "[editor][world][atmosphere]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("clouds.world.json"))) == true);
    QAction* sunActionOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateSun"));
    QAction* atmosphereActionOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateAtmosphere"));
    REQUIRE(sunActionOrNull != nullptr);
    REQUIRE(atmosphereActionOrNull != nullptr);
    sunActionOrNull->trigger();
    const QString sunId = workspace.Document().GetEntityKey(
        workspace.Document().Draft().value(QStringLiteral("entities")).toArray().last().toObject());
    atmosphereActionOrNull->trigger();
    const QString atmosphereId = workspace.Document().GetEntityKey(
        workspace.Document().Draft().value(QStringLiteral("entities")).toArray().last().toObject());
    REQUIRE(workspace.SelectEntity(atmosphereId) == true);
    QComboBox* atmosphereSunOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:atmosphere/sun"));
    REQUIRE(atmosphereSunOrNull != nullptr);
    atmosphereSunOrNull->setCurrentIndex(atmosphereSunOrNull->findData(sunId));
    REQUIRE(workspace.Apply() == true);
    RenderScene scene;
    QString error;
    REQUIRE(RenderScene::Load(root, workspace.Document(), scene, error) == true);
    REQUIRE(scene.Atmosphere.IsEnabled == true);
    CHECK(scene.Atmosphere.Clouds.IsEnabled == false);
    CHECK(scene.Atmosphere.Aurora.IsEnabled == false);
    const QJsonObject beforeLayers = workspace.Document().Draft();

    // The layer's fields stay hidden until the layer is ticked.
    QCheckBox* cloudsOrNull = workspace.findChild<QCheckBox*>(QStringLiteral("worldField:atmosphere/clouds"));
    QLineEdit* baseOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:atmosphere/cloudBase"));
    QLineEdit* extinctionOrNull =
        workspace.findChild<QLineEdit*>(QStringLiteral("worldField:atmosphere/cloudExtinction"));
    REQUIRE(cloudsOrNull != nullptr);
    REQUIRE(baseOrNull != nullptr);
    REQUIRE(extinctionOrNull != nullptr);
    CHECK(cloudsOrNull->isVisibleTo(&workspace) == true);
    CHECK(baseOrNull->isVisibleTo(&workspace) == false);
    cloudsOrNull->setChecked(true);
    CHECK(baseOrNull->isVisibleTo(&workspace) == true);
    CHECK(extinctionOrNull->isVisibleTo(&workspace) == true);
    baseOrNull->setText(QStringLiteral("2500"));
    extinctionOrNull->setText(QStringLiteral("0.002"));
    QCheckBox* auroraOrNull = workspace.findChild<QCheckBox*>(QStringLiteral("worldField:atmosphere/aurora"));
    QLineEdit* auroraTimeOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:atmosphere/auroraTime"));
    REQUIRE(auroraOrNull != nullptr);
    REQUIRE(auroraTimeOrNull != nullptr);
    CHECK(auroraTimeOrNull->isVisibleTo(&workspace) == false);
    auroraOrNull->setChecked(true);
    CHECK(auroraTimeOrNull->isVisibleTo(&workspace) == true);
    auroraTimeOrNull->setText(QStringLiteral("23"));
    REQUIRE(workspace.Apply() == true);
    REQUIRE(RenderScene::Load(root, workspace.Document(), scene, error) == true);
    const ludus::graphics::renderer::CloudLayerDescription& clouds = scene.Atmosphere.Clouds;
    CHECK(clouds.IsEnabled == true);
    CHECK(clouds.BaseHeightMetres == Catch::Approx(2500));
    CHECK(clouds.ExtinctionPerMetre == Catch::Approx(0.002));
    CHECK(clouds.ThicknessMetres == ludus::graphics::renderer::CloudLayerDescription::DEFAULT_THICKNESS_METRES);
    CHECK(clouds.AxisXWorld.X == Catch::Approx(1));
    CHECK(scene.Atmosphere.Aurora.IsEnabled == true);
    CHECK(scene.Atmosphere.Aurora.TimeSeconds == 23);

    // Undo restores the clear sky in one step.
    workspace.Undo();
    CHECK(workspace.Document().Draft() == beforeLayers);
    REQUIRE(RenderScene::Load(root, workspace.Document(), scene, error) == true);
    CHECK(scene.Atmosphere.Clouds.IsEnabled == false);
    CHECK(scene.Atmosphere.Aurora.IsEnabled == false);
    workspace.StopPreview();
}

TEST_CASE("Older Worlds keep per-light Earth observers until an Earth light edit makes them shared",
          "[editor][world][celestial]")
{
    using namespace ludus::foundation;
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    const WorldSunPlacement placement
    {
        .RotationDegrees = {-90, 0, 0},
        .IlluminanceLux = 10,
    };
    QString sunId;
    QString moonId;
    REQUIRE(CreateWorldSun(document, entropy, placement, sunId, error) == true);
    REQUIRE(CreateWorldSun(document, entropy, placement, moonId, error) == true);

    // Write a World as an earlier editor saved it: each Earth light holds its own observer.
    QJsonObject legacy = document.Draft();
    QJsonArray entities = legacy.value(QStringLiteral("entities")).toArray();
    for (qsizetype i = 0; i < entities.size(); ++i)
    {
        QJsonObject entity = entities[i].toObject();
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        QJsonObject light = components.value(QStringLiteral("light")).toObject();
        const bool isSun = document.GetEntityKey(entity) == sunId;
        light.insert(WORLD_CELESTIAL_ROLE, WORLD_CELESTIAL_DYNAMIC);
        light.insert(WORLD_CELESTIAL_DRIVER, isSun == true ? WORLD_CELESTIAL_EARTH_SUN : WORLD_CELESTIAL_EARTH_MOON);
        light.insert(WORLD_CELESTIAL_TIME, isSun == true ? 1.0 : 2.0);
        light.insert(WORLD_CELESTIAL_LATITUDE, isSun == true ? 10.0 : 20.0);
        components.insert(QStringLiteral("light"), light);
        entity.insert(QStringLiteral("components"), components);
        entities[i] = entity;
    }
    legacy.insert(QStringLiteral("entities"), entities);
    WorldDocument reopened;
    REQUIRE(reopened.LoadSchema(schema, error) == true);
    REQUIRE(reopened.Load(QJsonDocument(legacy).toJson(), error) == true);
    CHECK(reopened.Draft() == legacy);
    const auto getLight = [&reopened](const QString& id) {
        return reopened.Entity(id)
            .value(QStringLiteral("components"))
            .toObject()
            .value(QStringLiteral("light"))
            .toObject();
    };
    CHECK(MakeWorldLightValues(getLight(sunId), reopened.Draft()).value(WORLD_CELESTIAL_TIME).toDouble() == 1);
    CHECK(MakeWorldLightValues(getLight(moonId), reopened.Draft()).value(WORLD_CELESTIAL_TIME).toDouble() == 2);

    // An unrelated Moon edit makes the values it showed the World's.
    QJsonObject moonComponents = reopened.Entity(moonId).value(QStringLiteral("components")).toObject();
    QJsonObject moonLight = moonComponents.value(QStringLiteral("light")).toObject();
    moonLight.insert(QStringLiteral("intensity"), 0.5);
    moonComponents.insert(QStringLiteral("light"), moonLight);
    REQUIRE(reopened.Edit(moonId, moonComponents, QStringLiteral("Moon"), error) == true);
    const QJsonObject observer = reopened.Draft().value(WORLD_EARTH_OBSERVER).toObject();
    CHECK(observer.value(WORLD_CELESTIAL_TIME).toDouble() == 2);
    CHECK(observer.value(WORLD_CELESTIAL_LATITUDE).toDouble() == 20);
    CHECK(getLight(sunId).contains(WORLD_CELESTIAL_TIME) == false);
    CHECK(getLight(moonId).contains(WORLD_CELESTIAL_LATITUDE) == false);
    CHECK(MakeWorldLightValues(getLight(sunId), reopened.Draft()).value(WORLD_CELESTIAL_TIME).toDouble() == 2);
    REQUIRE(reopened.Undo() == true);
    CHECK(reopened.Draft() == legacy);

    // The shared observer holds only finite Earth dates and coordinates.
    QJsonObject invalid = reopened.Draft();
    invalid.insert(WORLD_EARTH_OBSERVER, QJsonObject{{WORLD_CELESTIAL_LATITUDE, 91}});
    CHECK(reopened.Validate(invalid, error) == false);
    invalid.insert(WORLD_EARTH_OBSERVER, QJsonObject{{QStringLiteral("spin"), 1}});
    CHECK(reopened.Validate(invalid, error) == false);
    invalid.insert(WORLD_EARTH_OBSERVER, QJsonObject{{WORLD_CELESTIAL_TIME, 3}, {WORLD_CELESTIAL_LONGITUDE, -80}});
    CHECK(reopened.Validate(invalid, error) == true);
    invalid.insert(WORLD_EARTH_OBSERVER, QJsonObject{{WORLD_CELESTIAL_TIME_ZONE, QStringLiteral("Asia/Seoul")}});
    CHECK(reopened.Validate(invalid, error) == true);
    invalid.insert(WORLD_EARTH_OBSERVER, QJsonObject{{WORLD_CELESTIAL_TIME_ZONE, 9}});
    CHECK(reopened.Validate(invalid, error) == false);
    invalid.insert(WORLD_EARTH_OBSERVER, QJsonObject{{WORLD_CELESTIAL_TIME_ZONE, QStringLiteral("../zoneinfo")}});
    CHECK(reopened.Validate(invalid, error) == false);
}

TEST_CASE("Earth preview time preserves the saved offset and copies the UTC clock", "[editor][world][celestial]")
{
    QWidget parent;
    constexpr ludus::foundation::float64 LEGACY_DAYS = -0.5000000001;
    WorldEarthTimeField time(LEGACY_DAYS, QString(WORLD_EARTH_UTC_TIME_ZONE), parent);
    QDateTimeEdit* dateTimeOrNull = time.findChild<QDateTimeEdit*>(QStringLiteral("worldEarthDateTime"));
    QPushButton* nowOrNull = time.findChild<QPushButton*>(QStringLiteral("worldEarthUseNow"));
    REQUIRE(dateTimeOrNull != nullptr);
    REQUIRE(nowOrNull != nullptr);
    CHECK(dateTimeOrNull->dateTime() == QDateTime::fromString(QStringLiteral("2000-01-01T00:00:00Z"), Qt::ISODate));
    CHECK(time.GetDaysSinceEpoch() == LEGACY_DAYS);
    CHECK(dateTimeOrNull->calendarPopup() == true);

    QSignalSpy edited(&time, &WorldEarthTimeField::Edited);
    const QDateTime before = QDateTime::currentDateTimeUtc();
    nowOrNull->click();
    const QDateTime after = QDateTime::currentDateTimeUtc();
    CHECK(edited.count() == 1);
    CHECK(dateTimeOrNull->dateTime().offsetFromUtc() == 0);
    CHECK(dateTimeOrNull->dateTime() >= before);
    CHECK(dateTimeOrNull->dateTime() <= after);
}

TEST_CASE("Earth locations resolve their tzdb time zone offline", "[editor][world][celestial]")
{
    REQUIRE(GetEarthTimeZoneIds().contains(QStringLiteral("Asia/Seoul")) == true);
    REQUIRE(GetEarthTimeZoneIds().contains(QStringLiteral("America/Toronto")) == true);
    // A country's zones win over a closer zone across the border; without a country the nearest zone wins.
    CHECK(FindEarthTimeZone(43.46527, -80.52229, QStringLiteral("CA")) == QStringLiteral("America/Toronto"));
    CHECK(FindEarthTimeZone(37.5665, 126.978, QStringLiteral("kr")) == QStringLiteral("Asia/Seoul"));
    CHECK(FindEarthTimeZone(37.5665, 126.978, {}) == QStringLiteral("Asia/Seoul"));
    CHECK(FindEarthTimeZone(42.3149, -83.0364, QStringLiteral("CA")) == QStringLiteral("America/Toronto"));
    CHECK(FindEarthTimeZone(42.3149, -83.0364, {}) == QStringLiteral("America/Detroit"));
    CHECK(FindEarthTimeZone(-33.8688, 151.2093, QStringLiteral("ZZ")) == QStringLiteral("Australia/Sydney"));

    CHECK(IsEarthTimeZoneIdValid(QStringLiteral("UTC")) == true);
    CHECK(IsEarthTimeZoneIdValid(QStringLiteral("America/Argentina/Buenos_Aires")) == true);
    CHECK(IsEarthTimeZoneIdValid(QStringLiteral("Etc/GMT-14")) == true);
    CHECK(IsEarthTimeZoneIdValid(QString()) == false);
    CHECK(IsEarthTimeZoneIdValid(QStringLiteral("Asia//Seoul")) == false);
    CHECK(IsEarthTimeZoneIdValid(QStringLiteral("../Seoul")) == false);
    CHECK(IsEarthTimeZoneIdValid(QStringLiteral("Asia/Seoul ")) == false);
}

TEST_CASE("Earth preview time shows a time zone's clock and keeps the reading across zones",
          "[editor][world][celestial]")
{
    if (QTimeZone(QByteArrayLiteral("Asia/Seoul")).isValid() == false ||
        QTimeZone(QByteArrayLiteral("America/Toronto")).isValid() == false)
    {
        SKIP("This Qt build has no IANA time zone data.");
    }
    QWidget parent;
    // 2000-01-01 12:00 UTC is 21:00 in Seoul (UTC+9).
    WorldEarthTimeField time(0, QStringLiteral("Asia/Seoul"), parent);
    QDateTimeEdit* dateTimeOrNull = time.findChild<QDateTimeEdit*>(QStringLiteral("worldEarthDateTime"));
    QLabel* captionOrNull = time.findChild<QLabel*>(QStringLiteral("worldEarthDateTimeCaption"));
    REQUIRE(dateTimeOrNull != nullptr);
    REQUIRE(captionOrNull != nullptr);
    CHECK(dateTimeOrNull->dateTime().toString(Qt::ISODate) == QStringLiteral("2000-01-01T21:00:00Z"));
    CHECK(captionOrNull->text() == QStringLiteral("Date && Time (Asia/Seoul, UTC+09:00)"));
    CHECK(time.GetDaysSinceEpoch() == 0);

    // Toronto (UTC-5 in January) keeps 21:00 on its own clock: 2000-01-02 02:00 UTC, 14 hours later.
    QSignalSpy edited(&time, &WorldEarthTimeField::Edited);
    time.SetTimeZoneKeepingClock(QStringLiteral("America/Toronto"));
    CHECK(edited.count() == 1);
    CHECK(dateTimeOrNull->dateTime().toString(Qt::ISODate) == QStringLiteral("2000-01-01T21:00:00Z"));
    CHECK(captionOrNull->text() == QStringLiteral("Date && Time (America/Toronto, UTC-05:00)"));
    constexpr ludus::foundation::float64 HOURS_PER_DAY = 24;
    constexpr ludus::foundation::float64 TORONTO_SHIFT_HOURS = 14;
    CHECK(time.GetDaysSinceEpoch() == TORONTO_SHIFT_HOURS / HOURS_PER_DAY);

    // Editing the clock in the zone stores the matching UTC instant.
    dateTimeOrNull->setDateTime(QDateTime::fromString(QStringLiteral("2000-01-01T07:00:00Z"), Qt::ISODate));
    CHECK(time.GetDaysSinceEpoch() == 0);
    time.SetTimeZoneKeepingClock(QStringLiteral("America/Toronto"));
    CHECK(edited.count() == 2);

    // A zone this Qt build cannot resolve shows UTC and says so.
    WorldEarthTimeField unknown(0, QStringLiteral("Mars/Olympus_Mons"), parent);
    QLabel* unknownCaptionOrNull = unknown.findChild<QLabel*>(QStringLiteral("worldEarthDateTimeCaption"));
    REQUIRE(unknownCaptionOrNull != nullptr);
    CHECK(unknownCaptionOrNull->text().contains(QStringLiteral("Mars/Olympus_Mons time zone data is unavailable")) ==
          true);
    CHECK(unknown.GetDaysSinceEpoch() == 0);
}

TEST_CASE("Earth place results decode longitude first and skip invalid points", "[editor][world][celestial]")
{
    const QByteArray response = QByteArrayLiteral(R"({"features": [
        {"geometry":{"type":"Point","coordinates":[-79.38,43.65]},
         "properties":{"name":"Toronto","city":"Toronto","state":"Ontario","country":"Canada","countrycode":"CA"}},
        {"geometry":{"type":"Point","coordinates":[3,91]},"properties":{"name":"Invalid latitude"}},
        {"geometry":{"type":"Point","coordinates":["1",2]},"properties":{"name":"Invalid number"}}
    ]})");
    QList<EarthPlace> places;
    REQUIRE(TryReadEarthPlaces(response, places) == true);
    REQUIRE(places.size() == 1);
    CHECK(places.first().Label == QStringLiteral("Toronto, Ontario, Canada"));
    CHECK(places.first().LatitudeDegrees == 43.65);
    CHECK(places.first().LongitudeDegrees == -79.38);
    CHECK(places.first().CountryCode == QStringLiteral("CA"));
    CHECK(TryReadEarthPlaces(QByteArrayLiteral("{}"), places) == false);
    CHECK(places.size() == 1);
}

TEST_CASE("Earth location selection fills one World draft and cancel preserves it", "[editor][world][celestial]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("earth-location.world.json"))) == true);
    QAction* sunOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateSun"));
    REQUIRE(sunOrNull != nullptr);
    sunOrNull->trigger();

    QComboBox* roleOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialRole"));
    QComboBox* driverOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialDriver"));
    QPushButton* locationOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldEarthChooseLocation"));
    QLineEdit* latitudeOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:light/celestialLatitude"));
    QLineEdit* longitudeOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:light/celestialLongitude"));
    REQUIRE(roleOrNull != nullptr);
    REQUIRE(driverOrNull != nullptr);
    REQUIRE(locationOrNull != nullptr);
    REQUIRE(latitudeOrNull != nullptr);
    REQUIRE(longitudeOrNull != nullptr);
    roleOrNull->setCurrentText(QStringLiteral("Dynamic Celestial"));
    driverOrNull->setCurrentText(QStringLiteral("Custom Orbit"));
    CHECK(locationOrNull->isHidden() == true);
    driverOrNull->setCurrentText(QStringLiteral("Earth Sun"));
    CHECK(locationOrNull->isHidden() == false);
    REQUIRE(workspace.Apply() == true);

    const QJsonObject original = workspace.Document().Draft();
    const auto openWithResult = [&workspace, locationOrNull]() {
        locationOrNull->click();
        QDialog* dialogOrNull = workspace.findChild<QDialog*>(QStringLiteral("worldEarthLocationDialog"));
        REQUIRE(dialogOrNull != nullptr);
        QListWidget* resultsOrNull = dialogOrNull->findChild<QListWidget*>(QStringLiteral("worldEarthLocationResults"));
        REQUIRE(resultsOrNull != nullptr);
        QListWidgetItem* result = new QListWidgetItem(QStringLiteral("Fixture location"), resultsOrNull);
        result->setData(Qt::UserRole, QPointF(-79.5, 43.5));
        resultsOrNull->setCurrentRow(0);
        return dialogOrNull;
    };

    QDialog* cancelledOrNull = openWithResult();
    REQUIRE(cancelledOrNull != nullptr);
    cancelledOrNull->reject();
    QCoreApplication::sendPostedEvents(cancelledOrNull, QEvent::DeferredDelete);
    CHECK(latitudeOrNull->text() == QStringLiteral("0"));
    CHECK(longitudeOrNull->text() == QStringLiteral("0"));
    CHECK(workspace.Document().Draft() == original);

    QDialog* acceptedOrNull = openWithResult();
    REQUIRE(acceptedOrNull != nullptr);
    QPushButton* chooseOrNull = acceptedOrNull->findChild<QPushButton*>(QStringLiteral("worldEarthUseCoordinates"));
    REQUIRE(chooseOrNull != nullptr);
    QComboBox* timeZoneOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialTimeZone"));
    QDateTimeEdit* dateTimeOrNull = workspace.findChild<QDateTimeEdit*>(QStringLiteral("worldEarthDateTime"));
    REQUIRE(timeZoneOrNull != nullptr);
    REQUIRE(dateTimeOrNull != nullptr);
    CHECK(timeZoneOrNull->currentData().toString() == QStringLiteral("UTC"));
    const QDateTime clock = dateTimeOrNull->dateTime();
    chooseOrNull->click();
    CHECK(latitudeOrNull->text() == QStringLiteral("43.5"));
    CHECK(longitudeOrNull->text() == QStringLiteral("-79.5"));
    CHECK(timeZoneOrNull->currentData().toString() == QStringLiteral("America/Toronto"));
    CHECK(workspace.Document().Draft() == original);
    REQUIRE(workspace.Apply() == true);
    const QJsonObject observer = workspace.Document().Draft().value(WORLD_EARTH_OBSERVER).toObject();
    CHECK(observer.value(WORLD_CELESTIAL_LATITUDE).toDouble() == 43.5);
    CHECK(observer.value(WORLD_CELESTIAL_LONGITUDE).toDouble() == -79.5);
    CHECK(observer.value(WORLD_CELESTIAL_TIME_ZONE).toString() == QStringLiteral("America/Toronto"));
    const QTimeZone toronto(QByteArrayLiteral("America/Toronto"));
    if (toronto.isValid() == true)
    {
        // The UTC clock reading the light showed becomes the same reading on Toronto's clock.
        constexpr ludus::foundation::float64 MILLISECONDS_PER_DAY = 86'400'000;
        const QDateTime epoch = QDateTime::fromString(QStringLiteral("2000-01-01T12:00:00Z"), Qt::ISODate);
        const QDateTime instant =
            epoch.addMSecs(qRound64(observer.value(WORLD_CELESTIAL_TIME).toDouble() * MILLISECONDS_PER_DAY));
        const QDateTime local = instant.toTimeZone(toronto);
        CHECK(QDateTime(local.date(), local.time(), QTimeZone::utc()) == clock);
    }
    workspace.StopPreview();
}

TEST_CASE("Custom celestial orbits use an authored world spin and survive a World reload", "[editor][world][celestial]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString path = QDir(root).filePath(QStringLiteral("custom-orbit.world.json"));
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    QAction* sunOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateSun"));
    REQUIRE(sunOrNull != nullptr);
    sunOrNull->trigger();

    QComboBox* roleOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialRole"));
    QComboBox* driverOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:light/celestialDriver"));
    QWidget* earthTimeOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldField:light/celestialTime"));
    QLineEdit* orbitTimeOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:light/celestialOrbitTime"));
    QLineEdit* spinOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:light/celestialObserverSpin"));
    QLineEdit* rateOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:light/celestialOrbitRate"));
    REQUIRE(roleOrNull != nullptr);
    REQUIRE(driverOrNull != nullptr);
    REQUIRE(earthTimeOrNull != nullptr);
    REQUIRE(orbitTimeOrNull != nullptr);
    REQUIRE(spinOrNull != nullptr);
    REQUIRE(rateOrNull != nullptr);
    roleOrNull->setCurrentText(QStringLiteral("Dynamic Celestial"));
    driverOrNull->setCurrentText(QStringLiteral("Custom Orbit"));
    // Bounded number fields sit inside a slider row, and the form hides that row, not the field.
    CHECK(orbitTimeOrNull->isVisibleTo(&workspace) == true);
    CHECK(earthTimeOrNull->isVisibleTo(&workspace) == false);

    const auto enterNumber = [](QLineEdit& inoutField, const QString& number) {
        inoutField.selectAll();
        QTest::keyClicks(&inoutField, number);
    };
    enterNumber(*spinOrNull, QStringLiteral("30"));
    enterNumber(*orbitTimeOrNull, QStringLiteral("6"));
    REQUIRE(workspace.Apply() == true);
    RenderScene scene;
    QString error;
    REQUIRE(RenderScene::Load(root, workspace.Document(), scene, error) == true);
    REQUIRE(scene.AuthoredLights.Count == 1);
    CHECK(scene.AuthoredLights.Lights[0].Direction.Y > 0);
    CHECK(scene.AuthoredLights.Lights[0].Intensity == 0);

    enterNumber(*rateOrNull, QStringLiteral("30"));
    REQUIRE(workspace.Apply() == true);
    driverOrNull->setCurrentText(QStringLiteral("Earth Sun"));
    CHECK(orbitTimeOrNull->isVisibleTo(&workspace) == false);
    CHECK(earthTimeOrNull->isVisibleTo(&workspace) == true);
    REQUIRE(workspace.Apply() == true);
    driverOrNull->setCurrentText(QStringLiteral("Custom Orbit"));
    REQUIRE(workspace.Save() == true);

    WorldDocument saved;
    REQUIRE(saved.LoadSchema(Read(QStringLiteral(":/world/world.schema.json")), error) == true);
    REQUIRE(saved.Load(Read(path), error) == true);
    REQUIRE(RenderScene::Load(root, saved, scene, error) == true);
    REQUIRE(scene.AuthoredLights.Count == 1);
    CHECK(scene.AuthoredLights.Lights[0].Direction.Y < 0);
    CHECK(scene.AuthoredLights.Lights[0].Intensity > 0);
    workspace.StopPreview();
}

TEST_CASE("A default World luminaire is scene data with an outline and a working gizmo",
          "[editor][world][creation][luminaire]")
{
    using namespace ludus::foundation;
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(QDir(root).filePath(QStringLiteral("luminaire.world.json"))) == true);
    workspace.findChild<QAction*>(QStringLiteral("worldCreateCube"))->trigger();
    QAction* createOrNull = workspace.findChild<QAction*>(QStringLiteral("worldCreateLuminaire"));
    REQUIRE(createOrNull != nullptr);
    createOrNull->trigger();
    const QJsonArray entities = workspace.Document().Draft().value(QStringLiteral("entities")).toArray();
    REQUIRE(entities.size() == 2);
    const QJsonObject entity = entities[1].toObject();
    const QString id = workspace.Document().GetEntityKey(entity);
    CHECK(entity.value(QStringLiteral("name")).toString() == QStringLiteral("Luminaire"));
    const QJsonObject values =
        entity.value(QStringLiteral("components")).toObject().value(QStringLiteral("luminaire")).toObject();
    CHECK(values.value(QStringLiteral("halfExtent")).toDouble() == Catch::Approx(0.35));

    // The panel faces down from its Transform, with the schema's beam profile.
    RenderScene scene;
    QString error;
    REQUIRE(RenderScene::Load(root, workspace.Document(), scene, error) == true);
    REQUIRE(scene.Luminaires.Count == 1);
    const ludus::graphics::renderer::PanelLuminaire& luminaire = scene.Luminaires.Luminaires[0];
    constexpr float32 FRAME_TOLERANCE = 1e-5F;
    CHECK(luminaire.SourceId == ComputeRenderSceneSourceId(id));
    CHECK(luminaire.CenterWorld.Y == Catch::Approx(2));
    CHECK(luminaire.TangentWorld.X == Catch::Approx(1).margin(FRAME_TOLERANCE));
    CHECK(luminaire.BitangentWorld.Z == Catch::Approx(1).margin(FRAME_TOLERANCE));
    const math::Vector3 normalWorld = math::Cross(luminaire.TangentWorld, luminaire.BitangentWorld);
    CHECK(normalWorld.Y == Catch::Approx(-1).margin(FRAME_TOLERANCE));
    CHECK(luminaire.HalfExtentWorld == Catch::Approx(0.35));
    CHECK(luminaire.RadianceScale == Catch::Approx(6));
    CHECK(luminaire.BeamWidth == Catch::Approx(0.02));
    CHECK(luminaire.BeamSteering == Catch::Approx(0.12));
    CHECK(luminaire.BeamSpread == Catch::Approx(0.08));
    CHECK(luminaire.BeamTwist == Catch::Approx(0.04));
    // Raster lighting is unchanged: the luminaire is not an analytic light.
    CHECK(scene.AuthoredLights.Count == 0);
    CHECK(scene.LuminaireBinding.isEmpty() == false);

    // An imported-scene composition keeps the authored luminaire.
    RenderScene authored;
    REQUIRE(RenderScene::Load(root, workspace.Document(), authored, error, false) == true);
    QJsonObject level = ImportedLevel();
    level.insert(QStringLiteral("objects"), QJsonArray{});
    RenderScene composed;
    REQUIRE(RenderScene::Merge(authored, {}, level, composed, error, {}) == true);
    REQUIRE(composed.Luminaires.Count == 1);
    CHECK(composed.Luminaires.Luminaires[0].SourceId == luminaire.SourceId);

    // Selecting it outlines the panel and its emission direction, and the transform handles work.
    PreviewCamera camera;
    camera.Id = QStringLiteral("luminaire-camera");
    camera.Sample.Pose.Position = {0, 4, 10};
    camera.Sample.Lens.Projection = ludus::gameplay::camera::CameraProjection::PerspectiveInfinite;
    camera.Sample.Lens.NearPlane = 0.01F;
    scene.Cameras.append(camera);
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(workspace.Document(), id, true);
    ViewportObjectOverlay overlay;
    const QRectF framePixel{0, 0, 800, 600};
    interaction.BuildOverlay(framePixel, overlay);
    // Four edges, two diagonals and the emission direction; the cube is not selected.
    CHECK(overlay.OutlineLinesPixel.GetSize() == 7);
    REQUIRE(overlay.HandleStrokes.IsEmpty() == false);

    // A committed viewport move writes the authored transform, which the preview descriptor binds, as one undo step.
    QSignalSpy committed(&interaction, &ViewportObjectInteraction::PlacementCommitted);
    const QPointF pivotPixel = overlay.HandleStrokes[0].LinePixel.p1();
    const QPointF tipPixel = overlay.HandleStrokes[0].LinePixel.p2();
    REQUIRE(interaction.Press(tipPixel, framePixel) == true);
    REQUIRE(interaction.Move(tipPixel + (tipPixel - pivotPixel), false) == true);
    REQUIRE(interaction.Release() == true);
    REQUIRE(committed.size() == 1);
    const QJsonObject placement = committed[0][1].toJsonObject();
    const QJsonArray position = placement.value(QStringLiteral("position")).toArray();
    REQUIRE(position.size() == 3);
    CHECK(position[1].toDouble() == Catch::Approx(2));
    CHECK(position[0].toDouble() != Catch::Approx(0));

    // The workspace writes that placement into the bound transform properties.
    RenderPanel* panelOrNull = workspace.findChild<RenderPanel*>();
    REQUIRE(panelOrNull != nullptr);
    constexpr int64 MAX_SCENE_WAIT_MILLISECONDS = 15000;
    constexpr int32 POLL_MILLISECONDS = 10;
    QElapsedTimer timer;
    timer.start();
    while (panelOrNull->Preview().Scene().Luminaires.Count == 0 && timer.elapsed() < MAX_SCENE_WAIT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    REQUIRE(panelOrNull->Preview().Scene().Luminaires.Count == 1);
    REQUIRE(workspace.SelectEntity(id) == true);
    const QJsonObject before = workspace.Document().Draft();
    panelOrNull->GetViewport().GetObjectInteraction().PlacementCommitted(id, placement);
    const QJsonObject transform = workspace.Document()
                                      .Entity(id)
                                      .value(QStringLiteral("components"))
                                      .toObject()
                                      .value(QStringLiteral("transform"))
                                      .toObject();
    CHECK(transform.value(QStringLiteral("x")).toDouble() == Catch::Approx(position[0].toDouble()));
    CHECK(transform.value(QStringLiteral("rx")).toDouble() == Catch::Approx(WORLD_LUMINAIRE_DOWNWARD_DEGREES));
    workspace.Undo();
    CHECK(workspace.Document().Draft() == before);
    workspace.StopPreview();
}

TEST_CASE("Default World creation preserves drafts on entropy and validation failures", "[editor][world][creation]")
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const QJsonObject before = document.Draft();
    QString id = QStringLiteral("unchanged");
    CHECK(CreateWorldObject(document, tests::MakeUnavailableEntropySource(), QStringLiteral("Cube"), id, error) ==
          false);
    CHECK(document.Draft() == before);
    CHECK(document.CanUndo() == false);
    CHECK(id == QStringLiteral("unchanged"));
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    CHECK(document.CreateEntity(entropy,
                                error,
                                QStringLiteral("Bad"),
                                id,
                                QJsonObject{{QStringLiteral("unknown"), QJsonObject{}}}) == false);
    CHECK(document.Draft() == before);
    CHECK(document.CanUndo() == false);
    CHECK(id == QStringLiteral("unchanged"));
    CHECK(CreateWorldObject(document, entropy, QStringLiteral("Unknown"), id, error) == false);
    WorldDocument project = Document();
    const QJsonObject projectBefore = project.Draft();
    CHECK(CreateWorldObject(project, entropy, QStringLiteral("Cube"), id, error) == false);
    CHECK(project.Draft() == projectBefore);
    // Worlds authored before bundled components existed can still open unchanged.
    QJsonObject legacy = before;
    legacy.insert(QStringLiteral("entities"),
                  QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("old.entity")},
                                         {QStringLiteral("name"), QStringLiteral("Old entity")},
                                         {QStringLiteral("components"), QJsonObject{}}}});
    REQUIRE(document.Load(QJsonDocument(legacy).toJson(), error) == true);
    CHECK(document.Draft() == legacy);
}

TEST_CASE("The World inspector saves raster shadow eligibility separately from light visibility",
          "[editor][world][lighting][raster-shadows]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString cube;
    QString light;
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Cube"), cube, error) == true);
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Point Light"), light, error) == true);

    const QJsonObject entity = document.Entity(light);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    QJsonObject values = components.value(QStringLiteral("light")).toObject();
    values.remove(WORLD_RASTER_SHADOWS);
    components.insert(QStringLiteral("light"), values);
    REQUIRE(document.Edit(light, components, entity.value(QStringLiteral("name")).toString(), error) == true);
    const QString path = QDir(root).filePath(QStringLiteral("shadows.world.json"));
    Write(path, document.Encode());

    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    REQUIRE(workspace.SelectEntity(light) == true);
    QCheckBox* shadowsOrNull = workspace.findChild<QCheckBox*>(QStringLiteral("worldField:light/rasterShadows"));
    REQUIRE(shadowsOrNull != nullptr);
    CHECK(shadowsOrNull->isChecked() == true);

    RenderScene before;
    REQUIRE(RenderScene::Load(root, workspace.Document(), before, error) == true);
    const QByteArray lightingKey = preview_lighting::MakeKey(before);
    preview_lighting::Inputs shadowed;
    REQUIRE(preview_lighting::BuildInputs(before, lightingKey, shadowed, preview_lighting::InputsTarget::Raster) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    CHECK(shadowed.ShadowCount == ludus::graphics::renderer::POINT_SHADOW_FACES);

    shadowsOrNull->setChecked(false);
    REQUIRE(workspace.Apply() == true);
    REQUIRE(workspace.Save() == true);

    WorldWorkspace reopened;
    reopened.SetProject(root, 1);
    REQUIRE(reopened.Open(path) == true);
    REQUIRE(reopened.SelectEntity(light) == true);
    QCheckBox* savedShadowsOrNull = reopened.findChild<QCheckBox*>(QStringLiteral("worldField:light/rasterShadows"));
    REQUIRE(savedShadowsOrNull != nullptr);
    CHECK(savedShadowsOrNull->isChecked() == false);

    RenderScene after;
    REQUIRE(RenderScene::Load(root, reopened.Document(), after, error) == true);
    REQUIRE(after.Definition != nullptr);
    LightSwitchReport switchReport;
    REQUIRE(ApplyLightSwitches({}, after, switchReport) == true);
    CHECK(preview_lighting::MakeKey(after) == lightingKey);
    preview_lighting::Inputs unshadowed;
    REQUIRE(preview_lighting::BuildInputs(after, lightingKey, unshadowed, preview_lighting::InputsTarget::Raster) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    CHECK(unshadowed.ShadowCount == 0);
    REQUIRE(unshadowed.Lights.Count == 1);
    CHECK(unshadowed.Lights.Lights[0].SourceId == ComputeEntitySourceId(light));

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    RenderGraphExtraction extraction;
    REQUIRE(MakeRenderGraphExtraction(after, {}, 1, extraction, error) == true);
    REQUIRE(extraction.ShaderInputs.Lights.GetSize() == 1);
    CHECK(extraction.ShaderInputs.Lights[0].ShouldCastRasterShadows == false);
#endif

    preview_lighting::Inputs reference;
    REQUIRE(preview_lighting::BuildInputs(after, lightingKey, reference, preview_lighting::InputsTarget::Reference) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    REQUIRE(reference.Lights.Count == 1);
    CHECK(reference.Lights.Lights[0].SourceId == ComputeEntitySourceId(light));
    CHECK(reference.Lights.Lights[0].Intensity == unshadowed.Lights.Lights[0].Intensity);
}

TEST_CASE("The World inspector offers an explicit always-on bake for a static light", "[editor][world][lighting]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString cube;
    QString light;
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Cube"), cube, error) == true);
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Point Light"), light, error) == true);

    // A previously saved light has neither of the new fields.
    const QJsonObject entity = document.Entity(light);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    QJsonObject values = components.value(QStringLiteral("light")).toObject();
    values.remove(QStringLiteral("staticTransform"));
    values.remove(QStringLiteral("bakeAlwaysOn"));
    components.insert(QStringLiteral("light"), values);
    REQUIRE(document.Edit(light, components, entity.value(QStringLiteral("name")).toString(), error) == true);
    QJsonObject dynamicLight = values;
    dynamicLight.insert(QStringLiteral("kind"), QStringLiteral("directional"));
    dynamicLight.insert(QString(WORLD_CELESTIAL_ROLE), QString(WORLD_CELESTIAL_DYNAMIC));
    dynamicLight.insert(QStringLiteral("staticTransform"), true);
    dynamicLight.insert(QStringLiteral("bakeAlwaysOn"), true);
    QJsonObject dynamicComponents = components;
    dynamicComponents.insert(QStringLiteral("light"), dynamicLight);
    CHECK(document.Edit(light, dynamicComponents, entity.value(QStringLiteral("name")).toString(), error) == false);
    CHECK(error.contains(QStringLiteral("Clear Bake always-on lighting")) == true);
    CHECK(document.Entity(light).value(QStringLiteral("components")).toObject() == components);

    const QString path = QDir(root).filePath(QStringLiteral("bake.world.json"));
    Write(path, document.Encode());

    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    REQUIRE(workspace.SelectEntity(light) == true);
    const auto findBake = [&workspace]() {
        return workspace.findChild<QCheckBox*>(QStringLiteral("worldField:light/bakeAlwaysOn"));
    };
    const auto findStatic = [&workspace]() {
        return workspace.findChild<QCheckBox*>(QStringLiteral("worldField:light/staticTransform"));
    };
    REQUIRE(findBake() != nullptr);
    REQUIRE(findStatic() != nullptr);
    CHECK(findBake()->isHidden() == true);
    findStatic()->setChecked(true);
    CHECK(findBake()->isChecked() == false);
    CHECK(findBake()->isHidden() == false);
    REQUIRE(workspace.Apply() == true);
    RenderScene realtime;
    REQUIRE(RenderScene::Load(root, workspace.Document(), realtime, error) == true);
    CHECK(realtime.AuthoredLights.Count == 1);
    CHECK(realtime.AlwaysOnLightIds.isEmpty() == true);

    REQUIRE(findBake() != nullptr);
    findBake()->setChecked(true);
    REQUIRE(workspace.Apply() == true);
    RenderScene baked;
    REQUIRE(RenderScene::Load(root, workspace.Document(), baked, error) == true);
    CHECK(baked.AlwaysOnLightIds == QVector<ludus::foundation::uint64>{ComputeEntitySourceId(light)});
    CHECK(baked.AuthoredLights.Count == realtime.AuthoredLights.Count);
    const QVector<WorldLightRow> rows = CollectWorldLightRows(workspace.Document(), baked);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].IsBakedAlwaysOn == true);
    CHECK(MakeWorldLightToggle(rows, {}, 0, false).WorldLights.isEmpty() == true);

    WorldSurfaceBakeState writer;
    writer.SetWorldPath(path);
    writer.SetScene(baked);
    QElapsedTimer bakeWait;
    bakeWait.start();
    while (writer.GetReady().isNull() == true && writer.GetFailure().isEmpty() == true && bakeWait.elapsed() < 30000)
    {
        writer.Poll(baked);
        QTest::qWait(1);
    }
    INFO(writer.GetFailure().toStdString());
    REQUIRE(writer.GetReady().isNull() == false);
    REQUIRE(writer.Save() == true);
    WorldSurfaceBakeState reader;
    reader.SetWorldPath(path);
    reader.SetScene(baked);
    REQUIRE(reader.GetReady().isNull() == false);
    REQUIRE(reader.GetReady()->Charts.size() == 1);
    CHECK(reader.GetReady()->Charts.front().Pixels == writer.GetReady()->Charts.front().Pixels);
    RenderScene moved = baked;
    moved.Objects[0].Transform.Translation.X += 1;
    reader.SetScene(moved);
    CHECK(reader.GetReady().isNull() == true);

    // Start one bake and move its grid twice before polling for a result.
    RenderScene probes = baked;
    probes.ProbeGrid.Counts = {2, 2, 2};
    reader.SetScene(probes);
    reader.Poll(probes);
    probes.ProbeGrid.OriginWorld.X = 1;
    reader.SetProbeGrid(probes.ProbeGrid);
    reader.Poll(probes);
    probes.ProbeGrid.OriginWorld.X = 2;
    reader.SetProbeGrid(probes.ProbeGrid);
    CHECK(reader.GetReady().isNull() == true);
    REQUIRE(QTest::qWaitFor([&reader, &probes]() {
                reader.Poll(probes);
                return reader.GetReady().isNull() == false || reader.GetFailure().isEmpty() == false;
            }) == true);
    INFO(reader.GetFailure().toStdString());
    REQUIRE(reader.GetReady().isNull() == false);
    CHECK(reader.GetReady()->LightingKey == preview_lighting::MakeKey(probes));
    CHECK(reader.GetReady()->ProbeAtlas.Pixels.isEmpty() == false);
    REQUIRE(reader.Save() == true);
    WorldSurfaceBakeState reopened;
    reopened.SetWorldPath(path);
    reopened.SetScene(probes);
    const QSharedPointer<const WorldSurfaceBake> restored = reopened.GetReady();
    REQUIRE(restored.isNull() == false);
    CHECK(restored->LightingKey == reader.GetReady()->LightingKey);
    REQUIRE(restored->Charts.size() == 1);
    CHECK(restored->Charts.front().Pixels == reader.GetReady()->Charts.front().Pixels);
    CHECK(restored->ProbeAtlas.Width == reader.GetReady()->ProbeAtlas.Width);
    CHECK(restored->ProbeAtlas.Height == reader.GetReady()->ProbeAtlas.Height);
    CHECK(restored->ProbeAtlas.IrradianceScale == reader.GetReady()->ProbeAtlas.IrradianceScale);
    CHECK(restored->ProbeAtlas.Pixels == reader.GetReady()->ProbeAtlas.Pixels);
    reopened.Poll(probes);
    CHECK(reopened.GetReady() == restored);
    probes.ProbeGrid.OriginWorld.X += 1;
    reopened.SetScene(probes);
    CHECK(reopened.GetReady().isNull() == true);
    probes.ProbeGrid.OriginWorld.X -= 1;

    const QString cachePath = path + QStringLiteral(".ludus-lighting");
    const QByteArray savedBytes = Read(cachePath);
    Write(cachePath, savedBytes.chopped(1));
    WorldSurfaceBakeState truncated;
    truncated.SetWorldPath(path);
    truncated.SetScene(probes);
    CHECK(truncated.GetReady().isNull() == true);
    REQUIRE(QTest::qWaitFor([&truncated, &probes]() {
                truncated.Poll(probes);
                return truncated.GetReady().isNull() == false || truncated.GetFailure().isEmpty() == false;
            }) == true);
    INFO(truncated.GetFailure().toStdString());
    REQUIRE(truncated.GetReady().isNull() == false);
    CHECK(truncated.GetReady()->ProbeAtlas.Pixels == restored->ProbeAtlas.Pixels);

    REQUIRE(findStatic() != nullptr);
    findStatic()->setChecked(false);
    REQUIRE(workspace.Apply() == true);
    RenderScene optedOut;
    REQUIRE(RenderScene::Load(root, workspace.Document(), optedOut, error) == true);
    CHECK(optedOut.AlwaysOnLightIds.isEmpty() == true);
    CHECK(optedOut.AuthoredLights.Count == 1);
    workspace.StopPreview();
}

TEST_CASE("The World inspector bakes one authored GI volume for marked receivers", "[editor][world][probe-gi]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString receiver;
    QString volume;
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Cube"), receiver, error) == true);
    REQUIRE(document.Attach(receiver, error, GetGiProbeReceiverComponentId()) == true);
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("GI Probe Volume"), volume, error) == true);
    const QJsonObject original = document.Draft();
    QString duplicate;
    CHECK(CreateWorldObject(document, entropy, QStringLiteral("GI Probe Volume"), duplicate, error) == false);
    CHECK(document.Draft() == original);
    CHECK(error.contains(QStringLiteral("one GI Probe Volume")) == true);

    const QString path = QDir(root).filePath(QStringLiteral("probe-gi.world.json"));
    Write(path, document.Encode());
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    REQUIRE(workspace.SelectEntity(volume) == true);
    RenderScene before;
    REQUIRE(RenderScene::Load(root, workspace.Document(), before, error) == true);
    CHECK(before.ProbeGrid.Counts[0] == 0);
    CHECK(before.GiProbeReceiverIds == QVector<ludus::foundation::uint64>{ComputeEntitySourceId(receiver)});
    QPushButton* bakeOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldBakeGi"));
    REQUIRE(bakeOrNull != nullptr);
    REQUIRE(bakeOrNull->isEnabled() == true);
    bakeOrNull->click();

    RenderScene after;
    REQUIRE(RenderScene::Load(root, workspace.Document(), after, error) == true);
    CHECK(after.ProbeGrid.Counts[0] > 0);
    CHECK(after.ProbeGrid.Spacing == 1);
    CHECK(after.GiProbeReceiverIds == before.GiProbeReceiverIds);
    CHECK(preview_lighting::MakeKey(after) != preview_lighting::MakeKey(before));
    workspace.Undo();
    RenderScene undone;
    REQUIRE(RenderScene::Load(root, workspace.Document(), undone, error) == true);
    CHECK(undone.ProbeGrid.Counts[0] == 0);
    workspace.StopPreview();
}

namespace
{
/// The volume's Transform scale on one axis, as the World stores it.
ludus::foundation::float64 ReadGiProbeScale(const WorldDocument& document, const QString& volume, const QString& key)
{
    return document.Entity(volume)
        .value(QStringLiteral("components"))
        .toObject()
        .value(QStringLiteral("transform"))
        .toObject()
        .value(key)
        .toDouble();
}
} // namespace

TEST_CASE("GI probe volumes lay probes out from the Transform scale and keep them across layout changes",
          "[editor][world][probe-gi]")
{
    namespace math = ludus::foundation::math;
    namespace renderer = ludus::graphics::renderer;
    using ludus::foundation::float64;
    SECTION("Counts come from whole spacings inside the edge, at least two, without a limit")
    {
        CHECK(ComputeGiProbeCountForSize(3, 1) == 4);
        CHECK(ComputeGiProbeCountForSize(0.3, 0.1) == 4);
        CHECK(ComputeGiProbeCountForSize(4.9999, 1) == 6);
        CHECK(ComputeGiProbeCountForSize(10.4, 1) == 11);
        CHECK(ComputeGiProbeCountForSize(10.6, 1) == 12);
        CHECK(ComputeGiProbeCountForSize(0.2, 1) == 2);
        CHECK(ComputeGiProbeCountForSize(43, 1) == 44);
        GiProbeCounts required{};
        const renderer::GiProbeGrid grid = ComputeGiProbeGridForVolume({0, 0, 0}, {43, 3, 10.4F}, 1, required);
        CHECK(grid.Counts == GiProbeCounts{renderer::MAX_GI_PROBE_AXIS_COUNT, 4, 11});
        CHECK(required == GiProbeCounts{44, 4, 11});
        const float64 covering = ComputeGiProbeSpacingCoveringSize({43, 3, 10.4F});
        CHECK(covering == 3.91);
        CHECK(ComputeGiProbeCountForSize(43, covering) == renderer::MAX_GI_PROBE_AXIS_COUNT);
        // A tiny box keeps the exact spacing when whole centimetres would drop a probe.
        const float64 tinyCovering = ComputeGiProbeSpacingCoveringSize({1.15F, 1, 1});
        CHECK(ComputeGiProbeCountForSize(1.15, tinyCovering) == renderer::MAX_GI_PROBE_AXIS_COUNT);
    }
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString volume;
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("GI Probe Volume"), volume, error) == true);
    const auto readVolume = [&document, &volume, &error]() {
        WorldGiProbeVolume read;
        REQUIRE(ReadWorldGiProbeVolume(document.Entity(volume).value(QStringLiteral("components")).toObject(),
                                       read,
                                       error) == true);
        return read;
    };
    SECTION("A new volume is a three-metre box holding four probes per axis at one-metre spacing")
    {
        const WorldGiProbeVolume created = readVolume();
        CHECK(created.Layout == GiProbeLayout::Volume);
        CHECK(created.SizeWorld == math::Vector3{3, 3, 3});
        CHECK(created.Grid.Counts == GiProbeCounts{4, 4, 4});
        CHECK(created.Grid.Spacing == 1);
        CHECK(created.IsCoverageLimited() == false);
        CHECK(DescribeWorldGiProbeCoverage(created).isEmpty() == true);
    }
    SECTION("Scaling the Transform adds probes at the same spacing and reports the limit")
    {
        const QJsonObject entity = document.Entity(volume);
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        WriteWorldGiProbeSize(components, 0, 43);
        WriteWorldGiProbeSize(components, 2, 10.4);
        REQUIRE(document.Edit(volume, components, entity.value(QStringLiteral("name")).toString(), error) == true);
        const WorldGiProbeVolume scaled = readVolume();
        CHECK(scaled.Grid.Counts == GiProbeCounts{renderer::MAX_GI_PROBE_AXIS_COUNT, 4, 11});
        CHECK(scaled.IsCoverageLimited() == true);
        const QString coverage = DescribeWorldGiProbeCoverage(scaled);
        CHECK(coverage.contains(QStringLiteral("44 along X")) == true);
        CHECK(coverage.contains(QStringLiteral("3.91 m")) == true);
        CHECK(ComputeWorldGiProbeVolumeBounds(scaled).Max.X == scaled.Grid.OriginWorld.X + 43);
        CHECK(ComputeWorldGiProbeBounds(scaled.Grid).Max.X == scaled.Grid.OriginWorld.X + 11);
    }
    SECTION("A volume saved without the layout keeps its counts and ignores the Transform scale")
    {
        const QJsonObject entity = document.Entity(volume);
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        QJsonObject values = components.value(GetGiProbeVolumeComponentId()).toObject();
        values.remove(GetGiProbeLayoutKey());
        values.insert(QStringLiteral("countX"), static_cast<int>(renderer::MAX_GI_PROBE_AXIS_COUNT));
        components.insert(GetGiProbeVolumeComponentId(), values);
        REQUIRE(document.Edit(volume, components, entity.value(QStringLiteral("name")).toString(), error) == true);
        const WorldGiProbeVolume legacy = readVolume();
        CHECK(legacy.Layout == GiProbeLayout::Counts);
        CHECK(legacy.Grid.Counts == GiProbeCounts{renderer::MAX_GI_PROBE_AXIS_COUNT, 4, 4});
        CHECK(legacy.SizeWorld == math::Vector3{11, 3, 3});
        // Choosing the volume layout writes that extent into the Transform scale, so nothing moves.
        QJsonObject chosen = components;
        values.insert(GetGiProbeLayoutKey(), GetGiProbeLayoutWord(GiProbeLayout::Volume));
        chosen.insert(GetGiProbeVolumeComponentId(), values);
        SynchronizeWorldGiProbeLayout(components, chosen);
        CHECK(chosen.value(QStringLiteral("transform")).toObject().value(QStringLiteral("sx")).toDouble() == 11);
        WorldGiProbeVolume synchronized;
        REQUIRE(ReadWorldGiProbeVolume(chosen, synchronized, error) == true);
        CHECK(synchronized.Layout == GiProbeLayout::Volume);
        CHECK(synchronized.Grid.Counts == legacy.Grid.Counts);
    }
    SECTION("Choosing the counts layout writes the counts the Transform scale produced")
    {
        QJsonObject components = document.Entity(volume).value(QStringLiteral("components")).toObject();
        WriteWorldGiProbeSize(components, 1, 7);
        QJsonObject chosen = components;
        QJsonObject values = chosen.value(GetGiProbeVolumeComponentId()).toObject();
        values.insert(GetGiProbeLayoutKey(), GetGiProbeLayoutWord(GiProbeLayout::Counts));
        chosen.insert(GetGiProbeVolumeComponentId(), values);
        SynchronizeWorldGiProbeLayout(components, chosen);
        CHECK(chosen.value(GetGiProbeVolumeComponentId()).toObject().value(QStringLiteral("countY")).toInt() == 8);
        CHECK(chosen.value(GetGiProbeVolumeComponentId()).toObject().value(QStringLiteral("countX")).toInt() == 4);
    }
}

TEST_CASE("World viewport resizes a volume-layout GI probe box and its probes follow with the inspector hidden",
          "[editor][world][object-interaction][probe-gi]")
{
    namespace math = ludus::foundation::math;
    namespace renderer = ludus::graphics::renderer;
    using ludus::foundation::float32;
    using ludus::foundation::float64;
    using ludus::foundation::uint32;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString mesh;
    QString volume;
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Cube"), mesh, error) == true);
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("GI Probe Volume"), volume, error) == true);
    const QString path = QDir(root).filePath(QStringLiteral("probe-box.world.json"));
    Write(path, document.Encode());
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    REQUIRE(workspace.SelectEntity(volume) == true);
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    RenderPanel* panelOrNull = workspace.findChild<RenderPanel*>();
    REQUIRE(panelOrNull != nullptr);

    const auto readVolume = [&workspace, &volume, &error]() {
        const QJsonObject components =
            workspace.Document().Entity(volume).value(QStringLiteral("components")).toObject();
        WorldGiProbeVolume read;
        REQUIRE(ReadWorldGiProbeVolume(components, read, error) == true);
        return read;
    };
    const WorldGiProbeVolume original = readVolume();
    REQUIRE(original.Layout == GiProbeLayout::Volume);
    const math::Aabb3 boundsWorld = ComputeWorldGiProbeVolumeBounds(original);
    const math::Vector3 centerWorld = (boundsWorld.Min + boundsWorld.Max) * 0.5F;
    RenderScene scene = MakeInteractionScene();
    scene.Cameras[0].Sample.Pose.Position = {static_cast<float64>(centerWorld.X),
                                             static_cast<float64>(centerWorld.Y),
                                             10};
    RenderPreview& preview = workspace.GetRenderPreview();
    ConfigureInteractionPreview(preview, scene);

    ViewportObjectInteraction& interaction = panelOrNull->GetViewport().GetObjectInteraction();
    interaction.SetActive(true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Move);
    const QRectF framePixel{0, 0, 800, 600};
    renderer::ViewDescription view;
    REQUIRE(preview.DescribeView(static_cast<uint32>(framePixel.width()),
                                 static_cast<uint32>(framePixel.height()),
                                 view,
                                 error) == true);
    const auto project = [&view, &framePixel](math::Vector3 positionWorld) {
        const math::Vector4 clip =
            view.WorldToClip * math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
        return QPointF((static_cast<float64>(clip.X / clip.W) + 1) * framePixel.width() * 0.5,
                       (1 - static_cast<float64>(clip.Y / clip.W)) * framePixel.height() * 0.5);
    };
    const QJsonObject before = workspace.Document().Draft();

    SECTION("The inspector shows the derived grid and hides the counts until the counts layout is chosen")
    {
        QLabel* summaryOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldProbeSummary"));
        REQUIRE(summaryOrNull != nullptr);
        CHECK(summaryOrNull->text().contains(QStringLiteral("4 × 4 × 4 probes")) == true);
        QLineEdit* countOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:giProbeVolume/countX"));
        REQUIRE(countOrNull != nullptr);
        CHECK(countOrNull->isVisibleTo(propertiesOrNull) == false);
        QPushButton* coverOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldProbeCoverSpacing"));
        REQUIRE(coverOrNull != nullptr);
        CHECK(coverOrNull->isVisibleTo(propertiesOrNull) == false);
        QComboBox* layoutOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:giProbeVolume/layout"));
        REQUIRE(layoutOrNull != nullptr);
        layoutOrNull->setCurrentIndex(layoutOrNull->findData(GetGiProbeLayoutWord(GiProbeLayout::Counts)));
        CHECK(countOrNull->isVisibleTo(propertiesOrNull) == true);
        REQUIRE(workspace.Apply() == true);
        const WorldGiProbeVolume counted = readVolume();
        CHECK(counted.Layout == GiProbeLayout::Counts);
        CHECK(counted.Grid.Counts == original.Grid.Counts);
        CHECK(ReadGiProbeScale(workspace.Document(), volume, QStringLiteral("sx")) == 3);
    }

    SECTION("The inspector offers the spacing that reaches every edge of a box past the probe limit")
    {
        // Stretch the box past the probe limit through the Transform's Scale X field.
        QLineEdit* scaleOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:transform/sx"));
        REQUIRE(scaleOrNull != nullptr);
        Type(scaleOrNull, QStringLiteral("43"));
        REQUIRE(workspace.Apply() == true);
        CHECK(ReadGiProbeScale(workspace.Document(), volume, QStringLiteral("sx")) == 43);
        propertiesOrNull->hide();
        REQUIRE(workspace.SelectEntity(mesh) == true);
        REQUIRE(workspace.SelectEntity(volume) == true);
        QPushButton* coverOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldProbeCoverSpacing"));
        REQUIRE(coverOrNull != nullptr);
        CHECK(coverOrNull->isVisibleTo(propertiesOrNull) == true);
        CHECK(coverOrNull->text() == QStringLiteral("Use 3.91 m spacing"));
        QTimer::singleShot(0, []() {
            QMessageBox* boxOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            REQUIRE(boxOrNull != nullptr);
            QPushButton* applyOrNull = boxOrNull->findChild<QPushButton*>(QStringLiteral("worldConfirmProbeSpacing"));
            REQUIRE(applyOrNull != nullptr);
            applyOrNull->click();
        });
        coverOrNull->click();
        const WorldGiProbeVolume covered = readVolume();
        CHECK(covered.Grid.Spacing == 3.91F);
        CHECK(covered.Grid.Counts[0] == renderer::MAX_GI_PROBE_AXIS_COUNT);
        CHECK(covered.IsCoverageLimited() == false);
        CHECK(ComputeWorldGiProbeBounds(covered.Grid).Max.X ==
              covered.Grid.OriginWorld.X + 3.91F * static_cast<float32>(renderer::MAX_GI_PROBE_AXIS_COUNT - 1));
        workspace.Undo();
        CHECK(readVolume().Grid.Spacing == 1);
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
    }

    propertiesOrNull->hide();
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel.GetSize() == 12);

    SECTION("Dragging the maximum face lengthens the box and the probes follow at the spacing")
    {
        const math::Vector3 faceWorld{boundsWorld.Max.X, centerWorld.Y, centerWorld.Z};
        const QPointF handlePixel = project(faceWorld);
        REQUIRE(interaction.Hover(handlePixel, framePixel) == true);
        CHECK(interaction.GetHoveredHandle().Kind == ViewportObjectInteraction::HandleKind::ProbeFace);
        REQUIRE(interaction.Press(handlePixel, framePixel) == true);
        REQUIRE(interaction.Move(project(faceWorld + math::Vector3{2, 0, 0}), false) == true);
        interaction.BuildOverlay(framePixel, overlay);
        CHECK(overlay.ReadoutText.contains(QStringLiteral("6 probes")) == true);
        CHECK(workspace.Document().Draft() == before);
        REQUIRE(interaction.Release() == true);
        const WorldGiProbeVolume resized = readVolume();
        CHECK(resized.Grid.OriginWorld == original.Grid.OriginWorld);
        CHECK(resized.Grid.Counts == GiProbeCounts{6, 4, 4});
        CHECK(resized.Grid.Spacing == original.Grid.Spacing);
        CHECK(ReadGiProbeScale(workspace.Document(), volume, QStringLiteral("sx")) == 5);
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
        workspace.Redo();
        CHECK(readVolume().Grid.Counts[0] == 6);
    }

    SECTION("The minimum face grows past the probe limit, keeping the far corner fixed")
    {
        const math::Vector3 faceWorld{boundsWorld.Min.X, centerWorld.Y, centerWorld.Z};
        REQUIRE(interaction.Press(project(faceWorld), framePixel) == true);
        CHECK(interaction.GetDraggedHandle().Kind == ViewportObjectInteraction::HandleKind::ProbeFace);
        const float32 dragMetres = 2 * static_cast<float32>(renderer::MAX_GI_PROBE_AXIS_COUNT);
        REQUIRE(interaction.Move(project(faceWorld - math::Vector3{dragMetres, 0, 0}), false) == true);
        interaction.BuildOverlay(framePixel, overlay);
        CHECK(overlay.ReadoutText.contains(QStringLiteral("(probe limit)")) == true);
        REQUIRE(interaction.Release() == true);
        const WorldGiProbeVolume resized = readVolume();
        CHECK(resized.Grid.Counts[0] == renderer::MAX_GI_PROBE_AXIS_COUNT);
        CHECK(resized.IsCoverageLimited() == true);
        CHECK(resized.Grid.OriginWorld.X < original.Grid.OriginWorld.X);
        CHECK(ComputeWorldGiProbeVolumeBounds(resized).Max == boundsWorld.Max);
        CHECK(ReadGiProbeScale(workspace.Document(), volume, QStringLiteral("sx")) ==
              static_cast<float64>(3 + dragMetres));
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
    }

    SECTION("The Scale tool's centre handle scales the box uniformly about its minimum corner")
    {
        interaction.SetTool(ViewportObjectInteraction::Tool::Scale);
        const QPointF handlePixel = project(boundsWorld.Min);
        REQUIRE(interaction.Hover(handlePixel, framePixel) == true);
        CHECK(interaction.GetHoveredHandle().Kind == ViewportObjectInteraction::HandleKind::Uniform);
        REQUIRE(interaction.Press(handlePixel, framePixel) == true);
        REQUIRE(interaction.Move(handlePixel + QPointF(56, -56), true) == true);
        interaction.BuildOverlay(framePixel, overlay);
        CHECK(workspace.Document().Draft() == before);
        REQUIRE(interaction.Release() == true);
        const WorldGiProbeVolume scaled = readVolume();
        CHECK(scaled.Grid.OriginWorld == original.Grid.OriginWorld);
        CHECK(scaled.SizeWorld == math::Vector3{6, 6, 6});
        CHECK(scaled.Grid.Counts == GiProbeCounts{7, 7, 7});
        CHECK(scaled.Grid.Spacing == original.Grid.Spacing);
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
    }
    workspace.StopPreview();
}

TEST_CASE("World viewport moves, resizes and scales an unbaked counts-layout GI probe volume with the inspector hidden",
          "[editor][world][object-interaction][probe-gi]")
{
    namespace math = ludus::foundation::math;
    namespace renderer = ludus::graphics::renderer;
    using ludus::foundation::float32;
    using ludus::foundation::float64;
    using ludus::foundation::uint32;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString mesh;
    QString volume;
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Cube"), mesh, error) == true);
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("GI Probe Volume"), volume, error) == true);
    const QJsonObject entity = document.Entity(volume);
    QJsonObject legacyComponents = entity.value(QStringLiteral("components")).toObject();
    QJsonObject legacyProbe = legacyComponents.value(GetGiProbeVolumeComponentId()).toObject();
    legacyProbe.remove(GetGiProbeLayoutKey());
    legacyComponents.insert(GetGiProbeVolumeComponentId(), legacyProbe);
    WriteWorldGiProbeSize(legacyComponents, 1, 1);
    REQUIRE(document.Edit(volume, legacyComponents, entity.value(QStringLiteral("name")).toString(), error) == true);
    const QString path = QDir(root).filePath(QStringLiteral("probe-gizmo.world.json"));
    Write(path, document.Encode());
    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    REQUIRE(workspace.SelectEntity(volume) == true);
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    RenderPanel* panelOrNull = workspace.findChild<RenderPanel*>();
    REQUIRE(panelOrNull != nullptr);

    const auto readGrid = [&workspace, &volume, &error]() {
        const QJsonObject components =
            workspace.Document().Entity(volume).value(QStringLiteral("components")).toObject();
        renderer::GiProbeGrid grid;
        REQUIRE(ReadWorldGiProbeGrid(components, grid, error, true) == true);
        return grid;
    };
    const renderer::GiProbeGrid original = readGrid();
    const math::Aabb3 boundsWorld = ComputeWorldGiProbeBounds(original);
    const math::Vector3 centerWorld = (boundsWorld.Min + boundsWorld.Max) * 0.5F;
    RenderScene scene = MakeInteractionScene();
    scene.Cameras[0].Sample.Pose.Position = {static_cast<float64>(centerWorld.X),
                                             static_cast<float64>(centerWorld.Y),
                                             10};
    RenderPreview& preview = workspace.GetRenderPreview();
    ConfigureInteractionPreview(preview, scene);

    ViewportObjectInteraction& interaction = panelOrNull->GetViewport().GetObjectInteraction();
    interaction.SetActive(true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Move);
    const QRectF framePixel{0, 0, 800, 600};
    renderer::ViewDescription view;
    REQUIRE(preview.DescribeView(static_cast<uint32>(framePixel.width()),
                                 static_cast<uint32>(framePixel.height()),
                                 view,
                                 error) == true);
    const auto project = [&view, &framePixel](math::Vector3 positionWorld) {
        const math::Vector4 clip =
            view.WorldToClip * math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
        return QPointF((static_cast<float64>(clip.X / clip.W) + 1) * framePixel.width() * 0.5,
                       (1 - static_cast<float64>(clip.Y / clip.W)) * framePixel.height() * 0.5);
    };
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.OutlineLinesPixel.GetSize() == 12);
    REQUIRE(overlay.HandleStrokes.IsEmpty() == false);
    const QJsonObject before = workspace.Document().Draft();

    SECTION("In Scale mode the maximum face still grows the count without moving the origin")
    {
        interaction.SetTool(ViewportObjectInteraction::Tool::Scale);
        const math::Vector3 faceWorld{boundsWorld.Max.X, centerWorld.Y, centerWorld.Z};
        const QPointF handlePixel = project(faceWorld);
        REQUIRE(interaction.Hover(handlePixel, framePixel) == true);
        CHECK(interaction.GetHoveredHandle().Kind == ViewportObjectInteraction::HandleKind::ProbeFace);
        REQUIRE(interaction.Press(handlePixel, framePixel) == true);
        REQUIRE(interaction.Move(project(faceWorld + math::Vector3{2 * original.Spacing, 0, 0}), false) == true);
        interaction.BuildOverlay(framePixel, overlay);
        CHECK(overlay.ReadoutText.contains(QStringLiteral("6 probes")) == true);
        CHECK(workspace.Document().Draft() == before);
        REQUIRE(interaction.Release() == true);
        const renderer::GiProbeGrid resized = readGrid();
        CHECK(resized.OriginWorld == original.OriginWorld);
        CHECK(resized.Counts[0] == original.Counts[0] + 2);
        CHECK(resized.Counts[1] == original.Counts[1]);
        CHECK(resized.Counts[2] == original.Counts[2]);
        CHECK(resized.Spacing == original.Spacing);
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
        workspace.Redo();
        CHECK(readGrid().Counts[0] == resized.Counts[0]);
    }

    SECTION("Dragging the minimum face past the probe limit offers a layout change and resize")
    {
        const math::Vector3 faceWorld{boundsWorld.Min.X, centerWorld.Y, centerWorld.Z};
        const float32 dragMetres = 2 * static_cast<float32>(renderer::MAX_GI_PROBE_AXIS_COUNT) * original.Spacing;
        REQUIRE(interaction.Press(project(faceWorld), framePixel) == true);
        REQUIRE(interaction.GetDraggedHandle().Kind == ViewportObjectInteraction::HandleKind::ProbeFace);
        REQUIRE(interaction.Move(project(faceWorld - math::Vector3{dragMetres, 0, 0}), false) == true);
        interaction.BuildOverlay(framePixel, overlay);
        CHECK(overlay.ReadoutText.contains(QStringLiteral("Release to switch layout")) == true);
        CHECK(workspace.Document().Draft() == before);
        const auto closeConfirmation = [](bool shouldResize) {
            QTimer::singleShot(0, [shouldResize]() {
                QMessageBox* boxOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                REQUIRE(boxOrNull != nullptr);
                CHECK(boxOrNull->defaultButton() == boxOrNull->button(QMessageBox::Cancel));
                if (shouldResize == true)
                {
                    QPushButton* resizeOrNull =
                        boxOrNull->findChild<QPushButton*>(QStringLiteral("worldConfirmProbeVolumeResize"));
                    REQUIRE(resizeOrNull != nullptr);
                    resizeOrNull->click();
                }
                else
                {
                    boxOrNull->defaultButton()->click();
                }
            });
        };
        SECTION("Cancel keeps the legacy layout and bounds")
        {
            closeConfirmation(false);
            REQUIRE(interaction.Release() == true);
            CHECK(workspace.Document().Draft() == before);
        }
        SECTION("Confirm switches layouts and resizes in one undoable edit")
        {
            closeConfirmation(true);
            REQUIRE(interaction.Release() == true);
            const QJsonObject components =
                workspace.Document().Entity(volume).value(QStringLiteral("components")).toObject();
            WorldGiProbeVolume resized;
            REQUIRE(ReadWorldGiProbeVolume(components, resized, error) == true);
            CHECK(resized.Layout == GiProbeLayout::Volume);
            CHECK(resized.Grid.Counts[0] == renderer::MAX_GI_PROBE_AXIS_COUNT);
            CHECK(resized.Grid.Spacing == original.Spacing);
            CHECK(resized.IsCoverageLimited() == true);
            CHECK(resized.SizeWorld.X == boundsWorld.Max.X - boundsWorld.Min.X + dragMetres);
            CHECK(ComputeWorldGiProbeVolumeBounds(resized).Max == boundsWorld.Max);
            workspace.Undo();
            CHECK(workspace.Document().Draft() == before);
            workspace.Redo();
            CHECK(readGrid().OriginWorld == resized.Grid.OriginWorld);
        }
    }

    SECTION("The inspector applies a legacy volume's new layout and explicit edge length together")
    {
        QComboBox* layoutOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldField:giProbeVolume/layout"));
        REQUIRE(layoutOrNull != nullptr);
        CHECK(layoutOrNull->currentData().toString() == GetGiProbeLayoutWord(GiProbeLayout::Counts));
        layoutOrNull->setCurrentIndex(layoutOrNull->findData(GetGiProbeLayoutWord(GiProbeLayout::Volume)));
        QLineEdit* scaleOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:transform/sx"));
        REQUIRE(scaleOrNull != nullptr);
        Type(scaleOrNull, QStringLiteral("43"));
        REQUIRE(workspace.Apply() == true);
        CHECK(ReadGiProbeScale(workspace.Document(), volume, QStringLiteral("sx")) == 43);
        CHECK(ReadGiProbeScale(workspace.Document(), volume, QStringLiteral("sy")) ==
              static_cast<float64>(boundsWorld.Max.Y - boundsWorld.Min.Y));
        CHECK(readGrid().Counts[0] == renderer::MAX_GI_PROBE_AXIS_COUNT);
        QLabel* summaryOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldProbeSummary"));
        REQUIRE(summaryOrNull != nullptr);
        CHECK(summaryOrNull->text().contains(QStringLiteral("Volume size 43 × 3 × 3 m")) == true);
        QLineEdit* derivedScaleOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:transform/sy"));
        REQUIRE(derivedScaleOrNull != nullptr);
        CHECK(derivedScaleOrNull->text() == QStringLiteral("3"));
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
    }

    SECTION("Cancelling a face drag leaves the World and bake unchanged")
    {
        const math::Vector3 faceWorld{boundsWorld.Max.X, centerWorld.Y, centerWorld.Z};
        REQUIRE(interaction.Press(project(faceWorld), framePixel) == true);
        REQUIRE(interaction.Move(project(faceWorld + math::Vector3{original.Spacing, 0, 0}), false) == true);
        interaction.Cancel();
        CHECK(interaction.Release() == false);
        CHECK(workspace.Document().Draft() == before);
        CHECK(interaction.FrameProbeVolume() == true);
    }

    SECTION("The Scale handle previews uniform spacing and commits one undoable edit")
    {
        interaction.SetTool(ViewportObjectInteraction::Tool::Scale);
        interaction.BuildOverlay(framePixel, overlay);
        REQUIRE(overlay.HandleFills.GetSize() >= 2);
        const QPointF handlePixel = project(centerWorld);
        const ViewportHandleFill& scaleFill = overlay.HandleFills[overlay.HandleFills.GetSize() - 1];
        CHECK(QLineF((scaleFill.CornersPixel[0] + scaleFill.CornersPixel[2]) * 0.5, handlePixel).length() < 0.01);
        REQUIRE(interaction.Hover(handlePixel, framePixel) == true);
        CHECK(interaction.GetHoveredHandle().Kind == ViewportObjectInteraction::HandleKind::ProbeSpacing);
        REQUIRE(interaction.Press(handlePixel, framePixel) == true);
        const QPointF enlargedPixel = handlePixel + QPointF(56, -56);
        REQUIRE(interaction.Move(enlargedPixel, true) == true);
        interaction.BuildOverlay(framePixel, overlay);
        CHECK(overlay.ReadoutText.contains(QStringLiteral("2.00 m spacing")) == true);
        CHECK(workspace.Document().Draft() == before);
        interaction.Cancel();
        CHECK(interaction.Release() == false);
        CHECK(workspace.Document().Draft() == before);

        REQUIRE(interaction.Press(handlePixel, framePixel) == true);
        REQUIRE(interaction.Move(enlargedPixel, true) == true);
        REQUIRE(interaction.Release() == true);
        const renderer::GiProbeGrid scaled = readGrid();
        CHECK(scaled.Spacing == 2 * original.Spacing);
        CHECK(scaled.OriginWorld == original.OriginWorld);
        CHECK(scaled.Counts == original.Counts);
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
        workspace.Redo();
        CHECK(readGrid().Spacing == scaled.Spacing);
    }

    SECTION("Move handles translate the volume without resizing it")
    {
        const QPointF handlePixel = overlay.HandleStrokes[0].LinePixel.pointAt(0.5);
        REQUIRE(interaction.Press(handlePixel, framePixel) == true);
        CHECK(interaction.GetDraggedHandle().Kind == ViewportObjectInteraction::HandleKind::Axis);
        REQUIRE(interaction.Move(handlePixel + QPointF(40, 0), true) == true);
        CHECK(workspace.Document().Draft() == before);
        REQUIRE(interaction.Release() == true);
        const renderer::GiProbeGrid moved = readGrid();
        CHECK(moved.OriginWorld.X > original.OriginWorld.X);
        CHECK(moved.Counts == original.Counts);
        CHECK(moved.Spacing == original.Spacing);
        workspace.Undo();
        CHECK(workspace.Document().Draft() == before);
    }
    workspace.StopPreview();
}

TEST_CASE("Default World objects and lights survive imported scene composition", "[editor][world][creation][framing]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString source = QDir(root).filePath(QStringLiteral("source"));
    QString error;
    REQUIRE(tests::CreateFixtureSource(source) == true);
    REQUIRE(tests::PrepareImportedLevel(root, QStringLiteral("room.lulevel"), source, error) == true);
    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    QString id;
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Cube"), id, error) == true);
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Point Light"), id, error) == true);
    const QJsonObject level =
        QJsonDocument::fromJson(Read(QDir(root).filePath(QStringLiteral("room.lulevel")))).object();
    REQUIRE(document.ImportScene(level, error) == true);
    const QString importedId =
        level.value(QStringLiteral("objects")).toArray()[0].toObject().value(QStringLiteral("id")).toString();
    REQUIRE(document.Attach(importedId, error, QStringLiteral("firstPersonPlayer")) == true);
    const QString path = QDir(root).filePath(QStringLiteral("mixed.world.json"));
    Write(path, document.Encode());
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    WorldWorkspace workspace(tools, nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    constexpr ludus::foundation::int64 MAX_SCENE_WAIT_MILLISECONDS = 15000;
    constexpr ludus::foundation::int32 POLL_MILLISECONDS = 10;
    QElapsedTimer timer;
    timer.start();
    while (workspace.CanSave() == false && timer.elapsed() < MAX_SCENE_WAIT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    REQUIRE(workspace.CanSave() == true);
    LevelWorkspace* scene = workspace.findChild<LevelWorkspace*>();
    REQUIRE(scene != nullptr);
    CHECK(scene->PreviewScene().Objects.size() == 2);
    CHECK(scene->PreviewScene().AuthoredLights.Count == 1);
    CHECK(workspace.Document().Draft() == document.Draft());

    // Imported placement supplies the mesh bounds even when the player lacks a Transform.
    REQUIRE(HasWorldFirstPersonPlayer(workspace.Document(), importedId) == true);
    REQUIRE(workspace.Document()
                .Entity(importedId)
                .value(QStringLiteral("components"))
                .toObject()
                .contains(QStringLiteral("transform")) == false);
    RenderPreview& preview = workspace.GetRenderPreview();
    preview.FrameAll();
    preview.SetFocusDistance(preview.GetFocusDistance() * 3);
    const ludus::foundation::float32 distanceBefore = preview.GetFocusDistance();
    scene->GetViewport().GetObjectInteraction().FrameRequested(importedId);
    CHECK(preview.GetFocusDistance() < distanceBefore);
    workspace.StopPreview();
}

TEST_CASE("An imported World's only authored luminaire survives scene composition", "[editor][world][luminaire]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString source = QDir(root).filePath(QStringLiteral("source"));
    QString error;
    REQUIRE(tests::CreateFixtureSource(source) == true);
    REQUIRE(tests::PrepareImportedLevel(root, QStringLiteral("room.lulevel"), source, error) == true);
    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    QString id;
    const ludus::foundation::identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    // No authored geometry or light: composition used to keep only the imported scene and drop the luminaire.
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Luminaire"), id, error) == true);
    const QJsonObject level =
        QJsonDocument::fromJson(Read(QDir(root).filePath(QStringLiteral("room.lulevel")))).object();
    REQUIRE(document.ImportScene(level, error) == true);
    const QString path = QDir(root).filePath(QStringLiteral("luminaire.world.json"));
    Write(path, document.Encode());
    ToolingPaths tools;
    tools.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    WorldWorkspace workspace(tools, nullptr);
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    constexpr ludus::foundation::int64 MAX_SCENE_WAIT_MILLISECONDS = 15000;
    constexpr ludus::foundation::int32 POLL_MILLISECONDS = 10;
    LevelWorkspace* scene = workspace.findChild<LevelWorkspace*>();
    REQUIRE(scene != nullptr);
    QElapsedTimer timer;
    timer.start();
    while ((workspace.CanSave() == false || scene->PreviewScene().Objects.isEmpty() == true) &&
           timer.elapsed() < MAX_SCENE_WAIT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    REQUIRE(workspace.CanSave() == true);
    CHECK(scene->PreviewScene().Objects.size() == 1);
    REQUIRE(scene->PreviewScene().Luminaires.Count == 1);
    CHECK(scene->PreviewScene().Luminaires.Luminaires[0].SourceId == ComputeRenderSceneSourceId(id));
    // The bindings that the luminaire check and the viewport gizmo read come with it.
    CHECK(scene->PreviewScene().LuminaireBinding.isEmpty() == false);
    CHECK(scene->PreviewScene().TransformBinding.isEmpty() == false);
    workspace.StopPreview();
}

TEST_CASE("Closing a World with created geometry and lighting retires the delegated preview safely",
          "[editor][world][creation][layout]")
{
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString descriptor = QDir(root).filePath(QStringLiteral("ludus.project.json"));
    Write(descriptor, QByteArrayLiteral(R"json({"version":1,"name":"objects","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    EditorController controller(ToolingPaths{}, nullptr, QDir(root).filePath(QStringLiteral("recent.json")));
    {
        MainWindow window(&controller, nullptr, QDir(root).filePath(QStringLiteral("workspace.json")));
        window.show();
        controller.OpenProject(descriptor);
        WorldWorkspace* world = window.findChild<WorldWorkspace*>();
        REQUIRE(world != nullptr);
        REQUIRE(world->Create(QDir(root).filePath(QStringLiteral("objects.world.json"))) == true);
        window.findChild<QAction*>(QStringLiteral("worldCreateCube"))->trigger();
        window.findChild<QAction*>(QStringLiteral("worldCreatePointLight"))->trigger();
        constexpr ludus::foundation::int32 MAX_PREVIEW_WAIT_MILLISECONDS = 15000;
        REQUIRE(QTest::qWaitFor(
                    [world]() {
                        const RenderScene& scene = world->GetRenderPreview().Scene();
                        return scene.Objects.size() == 1 && scene.AuthoredLights.Count == 1;
                    },
                    MAX_PREVIEW_WAIT_MILLISECONDS) == true);
    }
    QApplication::processEvents();
}

namespace
{
/// Longest wait for a fixture scene operation or placement cook on a loaded machine.
constexpr ludus::foundation::int64 MAX_SCENE_WAIT_MILLISECONDS = 30000;

/// Open a World whose one scene instance comes from the fixture cook, and wait for its first assembly.
/// @return The instance's stable ID.
QString OpenPlacementWorld(QTemporaryDir& inoutDirectory, WorldWorkspace& inoutWorkspace)
{
    const QString path = Fixture(inoutDirectory);
    const QString external = inoutDirectory.filePath(QStringLiteral("external"));
    QString error;
    REQUIRE(ludus::editor::tests::CreateFixtureSource(external) == true);
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(inoutDirectory.path(),
                                                       QStringLiteral("scenes/room.lulevel"),
                                                       external,
                                                       error) == true);
    WorldDocument document = Document();
    const QJsonObject level =
        QJsonDocument::fromJson(Read(inoutDirectory.filePath(QStringLiteral("scenes/room.lulevel")))).object();
    REQUIRE(document.ImportScene(level, error) == true);
    Write(path, document.Encode());
    inoutWorkspace.SetProject(inoutDirectory.path(), 1);
    REQUIRE(inoutWorkspace.Open(path) == true);
    return level.value(QStringLiteral("objects")).toArray()[0].toObject().value(QStringLiteral("id")).toString();
}
LevelWorkspace& GetLevel(const WorldWorkspace& workspace)
{
    LevelWorkspace* sceneOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(sceneOrNull != nullptr);
    return *sceneOrNull;
}
/// Wait until no scene operation or placement cook runs and the presented scene is validated.
void WaitForValidatedScene(const WorldWorkspace& workspace)
{
    LevelWorkspace& scene = GetLevel(workspace);
    QElapsedTimer timer;
    timer.start();
    const auto isSettled = [&workspace, &scene]() {
        return workspace.CanSave() == true && scene.CanStartOperation() == true &&
               scene.GetPlacementValidation() == PlacementValidation::Ready;
    };
    while (isSettled() == false && timer.elapsed() < MAX_SCENE_WAIT_MILLISECONDS)
    {
        QTest::qWait(10);
    }
    REQUIRE(isSettled() == true);
}
/// The selected instance's placement moved along x by deltaMetres from the World's current one.
QJsonObject MovePlacement(const WorldWorkspace& workspace, const QString& id, ludus::foundation::float64 deltaMetres)
{
    QJsonObject placement = workspace.Document().ScenePlacement(workspace.Document().Entity(id));
    QJsonArray position = placement.value(QStringLiteral("position")).toArray();
    position[0] = position[0].toDouble() + deltaMetres;
    placement.insert(QStringLiteral("position"), position);
    return placement;
}
const PreviewObject& GetPresentedObject(const WorldWorkspace& workspace, const QString& id)
{
    const RenderScene& scene = GetLevel(workspace).PreviewScene();
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id == id)
        {
            return object;
        }
    }
    FAIL("The presented scene lacks the instance");
    return scene.Objects[0];
}
ludus::foundation::float32 GetPresentedX(const WorldWorkspace& workspace, const QString& id)
{
    return GetPresentedObject(workspace, id).Transform.Translation.X;
}
} // namespace

TEST_CASE("An open World publishes newly imported diffuse conductor Mix geometry", "[editor][world][mix]")
{
    namespace format = ludus::content::scene_format;
    QTemporaryDir directory;
    WorldWorkspace workspace;
    if (PreviewViewport::CanPresent() == true)
    {
        workspace.show();
    }
    const QString existing = OpenPlacementWorld(directory, workspace);
    WaitForValidatedScene(workspace);
    const QString worldPath = workspace.Path();
    const QString source = directory.filePath(QStringLiteral("panel.luscene.json"));
    Write(source, QByteArrayLiteral(R"({
        "format":"ludus.scene","version":1,"name":"Mix scene",
        "materials":[{"name":"Mixed panel","model":"mix","base_color":[0.2,0.4,0.6],
                      "weight":0.375,"conductor":{"alpha":0.125,"eta":[0.5,0.75,1],"k":[2,3,4]}}],
        "objects":[{"name":"Panel","material":"Mixed panel",
                    "positions":[[2,0,0],[3,0,0],[2,1,0]],"polygons":[[0,1,2]]}]
    })"));
    REQUIRE(workspace.ImportScene(source) == true);
    CHECK(GetLevel(workspace).PreviewScene().Objects.size() == 1);
    WaitForValidatedScene(workspace);
    CHECK(workspace.Path() == worldPath);
    CHECK(workspace.Document().Entity(existing).isEmpty() == false);
    const RenderScene& scene = GetLevel(workspace).PreviewScene();
    REQUIRE(scene.Objects.size() == 2);
    bool hasMix = false;
    for (const PreviewObject& object : scene.Objects)
    {
        const ludus::resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(scene, object.Id);
        REQUIRE(materialOrNull != nullptr);
        if (materialOrNull->Scattering.Model == format::MaterialModel::DiffuseConductorMix)
        {
            hasMix = true;
            CHECK(materialOrNull->Metallic == 0.375F);
            CHECK(materialOrNull->Scattering.AlphaU == 0.125F);
            CHECK(materialOrNull->Scattering.AlphaV == 0.125F);
            CHECK(materialOrNull->Scattering.Eta[1] == 0.75F);
            CHECK(materialOrNull->Scattering.K[2] == 4);
        }
    }
    CHECK(hasMix == true);
    workspace.StopPreview();
}

TEST_CASE("World assigns a project BRDF and saves a slot roughness edit", "[editor][world][project-brdf]")
{
    namespace format = ludus::content::scene_format;
    using ludus::foundation::float32;
    QTemporaryDir directory;
    WorldWorkspace workspace;
    if (PreviewViewport::CanPresent() == true)
    {
        workspace.show();
    }
    const QString recipient = OpenPlacementWorld(directory, workspace);
    WaitForValidatedScene(workspace);
    const QString path = workspace.Path();
    const QString source = directory.filePath(QStringLiteral("glaze.luscene.json"));
    Write(source, QByteArrayLiteral(R"({
        "format":"ludus.scene","version":1,"name":"Glaze scene",
        "materials":[{"name":"demo/glaze","model":"project_brdf","base_color":[0.3,0.5,0.7],"roughness":0.35,
                      "brdf":{"version":1,"weights":[0.25,0.5,0.25],"powers":[0,4,12],
                              "fixed_dimension_reflection":true}}],
        "objects":[{"name":"Donor","material":"demo/glaze",
                    "positions":[[2,0,0],[3,0,0],[2,1,0]],"polygons":[[0,1,2]]}]
    })"));
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForValidatedScene(workspace);
    QString donor;
    const RenderScene& scene = GetLevel(workspace).PreviewScene();
    REQUIRE(scene.Objects.size() == 2);
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id != recipient)
        {
            donor = object.Id;
        }
    }
    REQUIRE(donor.isEmpty() == false);
    const auto checkMaterial = [](const WorldWorkspace& view, const QString& id, float32 roughness) {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(view).PreviewScene(), id);
        REQUIRE(materialOrNull != nullptr);
        CHECK(materialOrNull->Scattering.Model == format::MaterialModel::ProjectBrdf);
        CHECK(materialOrNull->Scattering.Eta[1] == 0.5F);
        CHECK(materialOrNull->Scattering.K[2] == 12);
        CHECK(format::SupportsFixedDimensionReflection(materialOrNull->Scattering) == true);
        CHECK(materialOrNull->Roughness == roughness);
    };
    checkMaterial(workspace, donor, 0.35F);
    QTreeWidget* materialsOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldMaterials"));
    REQUIRE(materialsOrNull != nullptr);
    REQUIRE(materialsOrNull->topLevelItemCount() == 2);
    const WorldDocument& document = workspace.Document();
    const QString donorMaterial = document.GetMaterialResourceId(document.ScenePlacement(document.Entity(donor)));
    REQUIRE(donorMaterial.startsWith(QStringLiteral("material/")) == true);
    REQUIRE(workspace.SelectEntity(recipient) == true);
    QPushButton* assignOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldAssignProjectBrdf"));
    REQUIRE(assignOrNull != nullptr);
    REQUIRE(assignOrNull->isEnabled() == true);
    assignOrNull->click();
    WaitForValidatedScene(workspace);
    checkMaterial(workspace, recipient, 0.35F);
    CHECK(GetLevel(workspace).PreviewScene().Definition->Materials.GetSize() == 1);
    REQUIRE(materialsOrNull->topLevelItemCount() == 1);
    QTreeWidgetItem* resource = materialsOrNull->topLevelItem(0);
    REQUIRE(resource->childCount() == 2);
    CHECK(resource->text(0) == QStringLiteral("demo/glaze"));
    CHECK(resource->data(0, Qt::UserRole).toString() == donorMaterial);
    QPushButton* revealOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldRevealMaterial"));
    REQUIRE(revealOrNull != nullptr);
    revealOrNull->click();
    QTabWidget* listsOrNull = workspace.findChild<QTabWidget*>(QStringLiteral("worldSidebarLists"));
    REQUIRE(listsOrNull != nullptr);
    CHECK(listsOrNull->currentWidget()->isAncestorOf(materialsOrNull) == true);
    CHECK(materialsOrNull->currentItem() == resource);
    CHECK(resource->isExpanded() == true);
    QPushButton* copyOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldCopyMaterialId"));
    REQUIRE(copyOrNull != nullptr);
    copyOrNull->click();
    CHECK(QApplication::clipboard()->text() == donorMaterial);

    workspace.show();
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    REQUIRE(workspace.SelectEntity(donor) == true);
    QTreeWidgetItem* recipientRow =
        resource->child(0)->data(0, Qt::UserRole).toString() == recipient ? resource->child(0) : resource->child(1);
    REQUIRE(recipientRow->data(0, Qt::UserRole).toString() == recipient);
    REQUIRE(materialsOrNull->isEnabled() == true);
    materialsOrNull->setCurrentItem(recipientRow);
    materialsOrNull->setFocus();
    QTest::keyClick(materialsOrNull, Qt::Key_Return);
    const QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(entitiesOrNull->currentItem() != nullptr);
    REQUIRE(entitiesOrNull->currentItem()->data(0, Qt::UserRole).toString() == recipient);
    propertiesOrNull->show();

    const QString roughnessField = QStringLiteral("scene/project_brdf/roughness");
    Type(Field(workspace, roughnessField), QStringLiteral("0"));
    CHECK(workspace.Apply() == false);
    checkMaterial(workspace, recipient, 0.35F);
    Type(Field(workspace, roughnessField), QStringLiteral("0.75"));
    REQUIRE(workspace.Apply() == true);
    WaitForValidatedScene(workspace);
    checkMaterial(workspace, recipient, 0.75F);
    checkMaterial(workspace, donor, 0.35F);
    CHECK(GetLevel(workspace).PreviewScene().Definition->Materials.GetSize() == 2);
    REQUIRE(materialsOrNull->topLevelItemCount() == 1);
    CHECK(materialsOrNull->topLevelItem(0)->childCount() == 2);

    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkMaterial(workspace, recipient, 0.35F);
    REQUIRE(workspace.CanRedo() == true);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkMaterial(workspace, recipient, 0.75F);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    workspace.StopPreview();
    workspace.hide();

    WorldWorkspace reopened;
    if (PreviewViewport::CanPresent() == true)
    {
        reopened.show();
    }
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    REQUIRE(reopened.SelectEntity(recipient) == true);
    checkMaterial(reopened, recipient, 0.75F);
    checkMaterial(reopened, donor, 0.35F);
    CHECK(Field(reopened, roughnessField)->text() == QStringLiteral("0.75"));
    reopened.StopPreview();
}

TEST_CASE("World document validates hair-card slots and writes them to the level", "[editor][world][hair-card]")
{
    QTemporaryDir directory;
    static_cast<void>(Fixture(directory));
    const QString external = directory.filePath(QStringLiteral("external"));
    QString error;
    REQUIRE(ludus::editor::tests::CreateFixtureSource(external) == true);
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(directory.path(),
                                                       QStringLiteral("scenes/room.lulevel"),
                                                       external,
                                                       error) == true);
    const QJsonObject level =
        QJsonDocument::fromJson(Read(directory.filePath(QStringLiteral("scenes/room.lulevel")))).object();
    WorldDocument document = Document();
    REQUIRE(document.ImportScene(level, error) == true);
    const QString id =
        level.value(QStringLiteral("objects")).toArray()[0].toObject().value(QStringLiteral("id")).toString();
    const QJsonObject entity = document.Entity(id);
    const QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    const QString name = entity.value(QStringLiteral("name")).toString();
    const QJsonObject placement = entity.value(QStringLiteral("scene")).toObject();
    const auto edit = [&](const QJsonObject& hair, QString& outError, const QJsonObject* coatingOrNull = nullptr) {
        QJsonObject scene = placement;
        scene.insert(QStringLiteral("hair_card"), hair);
        if (coatingOrNull != nullptr)
        {
            scene.insert(QStringLiteral("coating"), *coatingOrNull);
        }
        return document.Edit(id, components, name, outError, &scene, "Edit Hair Card");
    };
    QJsonObject invalid = MakeDefaultHairCard();
    invalid.insert(QStringLiteral("strand_direction_degrees"), 200);
    CHECK(edit(invalid, error) == false);
    CHECK(error.contains(QStringLiteral("Strand direction")) == true);
    invalid = MakeDefaultHairCard();
    invalid.insert(QString::fromLatin1(HAIR_CARD_CUTOFF_KEY), 2);
    CHECK(edit(invalid, error) == false);
    CHECK(error.contains(QStringLiteral("Mask cutoff")) == true);
    invalid = MakeDefaultHairCard();
    invalid.remove(QStringLiteral("secondary_weight"));
    CHECK(edit(invalid, error) == false);
    CHECK(error.contains(QStringLiteral("strand direction")) == true);
    const QJsonObject coating{{QStringLiteral("alpha"), 0.5},
                              {QStringLiteral("eta"), 1.5},
                              {QStringLiteral("thickness"), 0.01}};
    CHECK(edit(MakeDefaultHairCard(), error, &coating) == false);
    CHECK(error.contains(QStringLiteral("cannot also hold a coating")) == true);
    CHECK(document.ScenePlacement(document.Entity(id)).contains(QStringLiteral("hair_card")) == false);

    QJsonObject hair = MakeDefaultHairCard();
    hair.insert(QStringLiteral("strand_direction_degrees"), 90);
    REQUIRE(edit(hair, error) == true);
    bool isWritten = false;
    for (const QJsonValue value : document.SceneLevel().value(QStringLiteral("objects")).toArray())
    {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("id")).toString() == id)
        {
            CHECK(object.value(QStringLiteral("hair_card")).toObject() == hair);
            isWritten = true;
        }
    }
    CHECK(isWritten == true);
}

TEST_CASE(
    "World dresses the character bust with hair cards and keeps its strand edits across save, reopen and reimport",
    "[editor][world][hair-card]")
{
    namespace format = ludus::content::scene_format;
    using ludus::foundation::float32;
    const QString destination = qEnvironmentVariable("LUDUS_HAIR_BUST_PROJECTS");
    QTemporaryDir directory(destination.isEmpty() == true
                                ? QDir::temp().filePath(QStringLiteral("hair-bust-XXXXXX"))
                                : QDir(destination).filePath(QStringLiteral("hair-bust-XXXXXX")));
    const QString path = Fixture(directory);
    QJsonObject world = QJsonDocument::fromJson(Read(path)).object();
    world.insert(QStringLiteral("entities"), QJsonArray());
    Write(path, QJsonDocument(world).toJson());
    WorldWorkspace workspace;
    if (PreviewViewport::CanPresent() == true)
    {
        workspace.show();
    }
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    WaitForValidatedScene(workspace);
    const QDir bust(QDir(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT))
                        .filePath(QStringLiteral("tests/scenes/characters/infinite-head")));
    const QDir sourceDirectory(directory.filePath(QStringLiteral("infinite-head")));
    REQUIRE(QDir().mkpath(sourceDirectory.path()) == true);
    for (const QString& file : bust.entryList(QDir::Files))
    {
        REQUIRE(QFile::copy(bust.filePath(file), sourceDirectory.filePath(file)) == true);
    }
    const QString source = sourceDirectory.filePath(QStringLiteral("infinite-head-hair.gltf"));
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForValidatedScene(workspace);
    QString card;
    QString head;
    for (const PreviewObject& object : GetLevel(workspace).PreviewScene().Objects)
    {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(workspace).PreviewScene(), object.Id);
        REQUIRE(materialOrNull != nullptr);
        const std::string_view name = materialOrNull->Name.View();
        if (name == "Hair")
        {
            card = object.Id;
        }
        else if (name == "Skin")
        {
            head = object.Id;
        }
    }
    REQUIRE(card.isEmpty() == false);
    REQUIRE(head.isEmpty() == false);
    REQUIRE(GetLevel(workspace).PreviewScene().Definition->Lights.GetSize() == 2);
    const auto getSlot = [&card](const WorldWorkspace& view) {
        return view.Document().GetMaterialResourceId(view.Document().ScenePlacement(view.Document().Entity(card)));
    };
    const QString slot = getSlot(workspace);
    REQUIRE(slot.isEmpty() == false);
    const auto getMaterial = [](const WorldWorkspace& view,
                                const QString& id) -> const ludus::resources::SceneMaterial& {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(view).PreviewScene(), id);
        REQUIRE(materialOrNull != nullptr);
        return *materialOrNull;
    };
    const auto checkHairCard = [&](const WorldWorkspace& view, float32 strandDegrees, float32 cutoff) {
        const ludus::resources::SceneMaterial& material = getMaterial(view, card);
        REQUIRE(material.Scattering.Model == format::MaterialModel::HairCard);
        CHECK(material.Metallic == 0);
        CHECK(material.IsAlphaMasked == true);
        CHECK(material.Texture != format::NO_TEXTURE);
        CHECK(getSlot(view) == slot);
        CHECK(material.AlphaCutoff == cutoff);
        const format::HairCardParameters hair = format::GetHairCardParameters(material.Scattering);
        CHECK(hair.StrandDirectionDegrees == strandDegrees);
        CHECK(hair.PrimaryShiftDegrees == format::DEFAULT_HAIR_CARD_PRIMARY_SHIFT_DEGREES);
        CHECK(hair.SecondaryWeight == format::DEFAULT_HAIR_CARD_SECONDARY_WEIGHT);
    };
    CHECK(getMaterial(workspace, card).Scattering.Model == format::MaterialModel::MetallicRoughness);
    CHECK(getMaterial(workspace, card).AlphaCutoff == 0.5F);

    // Assign the hair card from the inspector; the viewport scene follows with the default strand controls.
    REQUIRE(workspace.SelectEntity(card) == true);
    QPushButton* assignOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldAssignHairCard"));
    REQUIRE(assignOrNull != nullptr);
    REQUIRE(assignOrNull->isEnabled() == true);
    assignOrNull->click();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 0, 0.5F);
    CHECK(workspace.findChild<QPushButton*>(QStringLiteral("worldAssignHairCard")) == nullptr);

    // Edit the strand direction and the mask cutoff, which the masked material authors.
    const QString strandField = QStringLiteral("scene/hair_card/strand_direction_degrees");
    const QString cutoffField = QStringLiteral("scene/hair_card/alpha_cutoff");
    Type(Field(workspace, strandField), QStringLiteral("90"));
    Type(Field(workspace, cutoffField), QStringLiteral("0.625"));
    REQUIRE(workspace.Apply() == true);
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 90, 0.625F);

    // An out-of-range strand is rejected beside the field, and the viewport keeps the last applied card.
    Type(Field(workspace, strandField), QStringLiteral("200"));
    CHECK(workspace.Apply() == false);
    QLabel* errorOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldHairCardError"));
    REQUIRE(errorOrNull != nullptr);
    CHECK(errorOrNull->isHidden() == false);
    CHECK(errorOrNull->text().contains(QStringLiteral("Strand direction")) == true);
    CHECK(errorOrNull->text().contains(QStringLiteral("-180 to 180")) == true);
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 90, 0.625F);
    Type(Field(workspace, strandField), QStringLiteral("45"));
    REQUIRE(workspace.Apply() == true);
    CHECK(errorOrNull->isHidden() == true);
    CHECK(errorOrNull->text().isEmpty() == true);
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 45, 0.625F);

    // Undo and redo the strand edit, then the assignment.
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 90, 0.625F);
    REQUIRE(workspace.CanRedo() == true);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 45, 0.625F);

    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 90, 0.625F);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 0, 0.5F);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    CHECK(getMaterial(workspace, card).Scattering.Model == format::MaterialModel::MetallicRoughness);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 0, 0.5F);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 90, 0.625F);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkHairCard(workspace, 45, 0.625F);

    // The skin fixture's original slot has no opacity mask.
    REQUIRE(workspace.SelectEntity(head) == true);
    QPushButton* headAssignOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldAssignHairCard"));
    REQUIRE(headAssignOrNull != nullptr);
    headAssignOrNull->click();
    WaitForValidatedScene(workspace);
    CHECK(getMaterial(workspace, head).Scattering.Model == format::MaterialModel::HairCard);
    CHECK(workspace.findChild<QLineEdit*>(QStringLiteral("worldField:") + cutoffField) == nullptr);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    CHECK(getMaterial(workspace, head).Scattering.Model != format::MaterialModel::HairCard);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    workspace.StopPreview();
    workspace.hide();

    // Save, reopen and reimport keep the slot's hair card and its edits.
    WorldWorkspace reopened;
    if (PreviewViewport::CanPresent() == true)
    {
        reopened.show();
    }
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    checkHairCard(reopened, 45, 0.625F);
    REQUIRE(reopened.ImportScene(source) == true);
    WaitForValidatedScene(reopened);
    checkHairCard(reopened, 45, 0.625F);
    REQUIRE(reopened.SelectEntity(card) == true);
    CHECK(Field(reopened, strandField)->text() == QStringLiteral("45"));
    CHECK(Field(reopened, cutoffField)->text() == QStringLiteral("0.625"));

    // Use Inherited Material restores the imported material in one undoable edit.
    QPushButton* restoreOrNull = reopened.findChild<QPushButton*>(QStringLiteral("worldRestoreHairCard"));
    REQUIRE(restoreOrNull != nullptr);
    restoreOrNull->click();
    WaitForValidatedScene(reopened);
    CHECK(getMaterial(reopened, card).Scattering.Model == format::MaterialModel::MetallicRoughness);
    CHECK(getMaterial(reopened, card).AlphaCutoff == 0.5F);
    reopened.Undo();
    WaitForValidatedScene(reopened);
    checkHairCard(reopened, 45, 0.625F);
    reopened.StopPreview();
    if (destination.isEmpty() == false)
    {
        // Leave a saved World with the strand along the ribbons for the timing comparison.
        REQUIRE(reopened.SelectEntity(card) == true);
        Type(Field(reopened, strandField), QStringLiteral("90"));
        REQUIRE(reopened.Apply() == true);
        WaitForValidatedScene(reopened);
        REQUIRE(reopened.Save() == true);
        directory.setAutoRemove(false);
        WARN(QStringLiteral("hair bust project: %1").arg(directory.path()).toStdString());
    }
}

TEST_CASE("World document validates skin slots and writes them to the level", "[editor][world][skin]")
{
    QTemporaryDir directory;
    static_cast<void>(Fixture(directory));
    const QString external = directory.filePath(QStringLiteral("external"));
    QString error;
    REQUIRE(ludus::editor::tests::CreateFixtureSource(external) == true);
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(directory.path(),
                                                       QStringLiteral("scenes/room.lulevel"),
                                                       external,
                                                       error) == true);
    const QJsonObject level =
        QJsonDocument::fromJson(Read(directory.filePath(QStringLiteral("scenes/room.lulevel")))).object();
    WorldDocument document = Document();
    REQUIRE(document.ImportScene(level, error) == true);
    const QString id =
        level.value(QStringLiteral("objects")).toArray()[0].toObject().value(QStringLiteral("id")).toString();
    const QJsonObject entity = document.Entity(id);
    const QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    const QString name = entity.value(QStringLiteral("name")).toString();
    const QJsonObject placement = entity.value(QStringLiteral("scene")).toObject();
    const auto edit = [&](const QJsonObject& skin, QString& outError, const QJsonObject* hairOrNull = nullptr) {
        QJsonObject scene = placement;
        scene.insert(QStringLiteral("skin"), skin);
        if (hairOrNull != nullptr)
        {
            scene.insert(QStringLiteral("hair_card"), *hairOrNull);
        }
        return document.Edit(id, components, name, outError, &scene, "Edit Skin");
    };
    const auto makeSkin = [](const QJsonArray& distance) {
        return QJsonObject{{QString::fromLatin1(SKIN_DISTANCE_KEY), distance}};
    };
    CHECK(edit(makeSkin({0.2, 0, 0}), error) == false);
    CHECK(error.contains(QStringLiteral("Scattering red (mm)")) == true);
    CHECK(error.contains(QStringLiteral("0 to 100")) == true);
    CHECK(edit(makeSkin({0, 0, -0.001}), error) == false);
    CHECK(error.contains(QStringLiteral("Scattering blue (mm)")) == true);
    CHECK(edit(makeSkin({0.001, 0.001}), error) == false);
    CHECK(error.contains(QStringLiteral("red, green and blue scattering distance")) == true);
    const QJsonObject hair = MakeDefaultHairCard();
    CHECK(edit(MakeDefaultSkin(nullptr), error, &hair) == false);
    CHECK(error.contains(QStringLiteral("cannot also hold")) == true);
    CHECK(document.ScenePlacement(document.Entity(id)).contains(QStringLiteral("skin")) == false);

    const QJsonObject skin = makeSkin({0.004, 0.0015, 0.0007});
    REQUIRE(edit(skin, error) == true);
    bool isWritten = false;
    for (const QJsonValue value : document.SceneLevel().value(QStringLiteral("objects")).toArray())
    {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("id")).toString() == id)
        {
            CHECK(object.value(QStringLiteral("skin")).toObject() == skin);
            isWritten = true;
        }
    }
    CHECK(isWritten == true);
}

TEST_CASE("Engine bust World for the skin benchmark", "[.skin-bust-world]")
{
    // Write a project under LUDUS_SKIN_BUST_PROJECTS for the raster benchmark
    // (docs/development/benchmark-scenes.md, "Measure raster skin on the character bust"): the lit Infinite head
    // imported into an empty World, with its slot made skin through the inspector. The benchmark's skin control
    // draws the same World as metallic-roughness.
    namespace format = ludus::content::scene_format;
    const QString destination = qEnvironmentVariable("LUDUS_SKIN_BUST_PROJECTS");
    if (destination.isEmpty() == true)
    {
        SKIP("Set LUDUS_SKIN_BUST_PROJECTS to a directory that receives the skin bust project.");
    }
    REQUIRE(QDir().mkpath(destination) == true);
    const QDir bust(QDir(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT))
                        .filePath(QStringLiteral("tests/scenes/characters/infinite-head")));
    QTemporaryDir directory(QDir(destination).filePath(QStringLiteral("skin-bust-XXXXXX")));
    directory.setAutoRemove(false);
    const QString path = Fixture(directory);
    // Only the bust: the starter World's cube would enclose it.
    QJsonObject world = QJsonDocument::fromJson(Read(path)).object();
    world.insert(QStringLiteral("entities"), QJsonArray());
    Write(path, QJsonDocument(world).toJson());
    const QDir copy(directory.filePath(QStringLiteral("infinite-head")));
    REQUIRE(QDir().mkpath(copy.path()) == true);
    for (const QString& file : bust.entryList(QDir::Files))
    {
        REQUIRE(QFile::copy(bust.filePath(file), copy.filePath(file)) == true);
    }
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    WaitForValidatedScene(workspace);
    REQUIRE(workspace.ImportScene(copy.filePath(QStringLiteral("infinite-head-lit.gltf"))) == true);
    WaitForValidatedScene(workspace);
    QString head;
    for (const PreviewObject& object : GetLevel(workspace).PreviewScene().Objects)
    {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(workspace).PreviewScene(), object.Id);
        if (materialOrNull != nullptr && materialOrNull->Name.View() == "Skin")
        {
            head = object.Id;
        }
    }
    REQUIRE(head.isEmpty() == false);
    CHECK(GetLevel(workspace).PreviewScene().Definition->Lights.GetSize() == 2);
    REQUIRE(workspace.SelectEntity(head) == true);
    QPushButton* assignOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldAssignSkin"));
    REQUIRE(assignOrNull != nullptr);
    assignOrNull->click();
    WaitForValidatedScene(workspace);
    const ludus::resources::SceneMaterial* materialOrNull =
        GetSceneObjectMaterialOrNull(GetLevel(workspace).PreviewScene(), head);
    REQUIRE(materialOrNull != nullptr);
    CHECK(materialOrNull->Scattering.Model == format::MaterialModel::Skin);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    workspace.StopPreview();
    // The benchmark reads Worlds under assets/, so it names this one ../authoring/starter.world.json.
    WARN(QStringLiteral("skin bust project: %1").arg(directory.path()).toStdString());
}

TEST_CASE("World makes the engine's bust skin and keeps its edits across save, reopen and reimport",
          "[editor][world][skin]")
{
    namespace format = ludus::content::scene_format;
    using ludus::foundation::float32;
    QTemporaryDir directory;
    WorldWorkspace workspace;
    if (PreviewViewport::CanPresent() == true)
    {
        workspace.show();
    }
    static_cast<void>(OpenPlacementWorld(directory, workspace));
    WaitForValidatedScene(workspace);
    const QString path = workspace.Path();
    // The engine's Infinite head scan: one "Skin" slot that imports as metallic-roughness.
    const QDir bust(QDir(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT))
                        .filePath(QStringLiteral("tests/scenes/characters/infinite-head")));
    const QDir copy(directory.filePath(QStringLiteral("infinite-head")));
    REQUIRE(QDir().mkpath(copy.path()) == true);
    for (const QString& file : bust.entryList(QDir::Files))
    {
        REQUIRE(QFile::copy(bust.filePath(file), copy.filePath(file)) == true);
    }
    const QString source = copy.filePath(QStringLiteral("infinite-head.gltf"));
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForValidatedScene(workspace);
    QString head;
    for (const PreviewObject& object : GetLevel(workspace).PreviewScene().Objects)
    {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(workspace).PreviewScene(), object.Id);
        REQUIRE(materialOrNull != nullptr);
        if (materialOrNull->Name.View() == "Skin")
        {
            head = object.Id;
        }
    }
    REQUIRE(head.isEmpty() == false);
    const auto getMaterial = [&head](const WorldWorkspace& view) -> const ludus::resources::SceneMaterial& {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(view).PreviewScene(), head);
        REQUIRE(materialOrNull != nullptr);
        return *materialOrNull;
    };
    const auto checkSkin = [&](const WorldWorkspace& view, float32 red, float32 green, float32 blue) {
        const ludus::resources::SceneMaterial& material = getMaterial(view);
        REQUIRE(material.Scattering.Model == format::MaterialModel::Skin);
        CHECK(material.Metallic == 0);
        CHECK(material.Scattering.K[0] == red);
        CHECK(material.Scattering.K[1] == green);
        CHECK(material.Scattering.K[2] == blue);
        // The scan's colour and normal maps are kept.
        CHECK(material.Texture != format::NO_TEXTURE);
        CHECK(material.SurfaceTexture != format::NO_TEXTURE);
    };
    CHECK(getMaterial(workspace).Scattering.Model == format::MaterialModel::MetallicRoughness);
    using ludus::foundation::math::Vector3;
    const Vector3 inheritedReflectance = getMaterial(workspace).Reflectance;
    const float32 inheritedRoughness = getMaterial(workspace).Roughness;
    const auto checkSurface = [&](const WorldWorkspace& view, const Vector3& reflectance, float32 roughness) {
        CHECK(getMaterial(view).Reflectance == reflectance);
        CHECK(getMaterial(view).Roughness == roughness);
    };

    // Make Skin starts from human skin's normalized-diffusion distance, in metres.
    REQUIRE(workspace.SelectEntity(head) == true);
    QPushButton* assignOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldAssignSkin"));
    REQUIRE(assignOrNull != nullptr);
    REQUIRE(assignOrNull->isEnabled() == true);
    assignOrNull->click();
    WaitForValidatedScene(workspace);
    checkSkin(workspace, 0.0015F, 0.00094F, 0.00057F);
    // The base colour and roughness stay the inherited material's.
    checkSurface(workspace, inheritedReflectance, inheritedRoughness);
    CHECK(workspace.findChild<QPushButton*>(QStringLiteral("worldAssignSkin")) == nullptr);
    // A skin slot is not offered as a hair card.
    CHECK(workspace.findChild<QPushButton*>(QStringLiteral("worldAssignHairCard")) == nullptr);

    // The fields edit millimetres; an unchanged field keeps its stored metres.
    const QString redField = QStringLiteral("scene/skin/scattering_red_mm");
    CHECK(Field(workspace, redField)->text() == QStringLiteral("1.5"));
    Type(Field(workspace, redField), QStringLiteral("8"));
    REQUIRE(workspace.Apply() == true);
    WaitForValidatedScene(workspace);
    checkSkin(workspace, 0.008F, 0.00094F, 0.00057F);

    // An out-of-range distance is rejected beside the field, and the viewport keeps the last applied skin.
    Type(Field(workspace, redField), QStringLiteral("200"));
    CHECK(workspace.Apply() == false);
    QLabel* errorOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldSkinError"));
    REQUIRE(errorOrNull != nullptr);
    CHECK(errorOrNull->isHidden() == false);
    CHECK(errorOrNull->text().contains(QStringLiteral("Scattering red (mm)")) == true);
    CHECK(errorOrNull->text().contains(QStringLiteral("0 to 100")) == true);
    WaitForValidatedScene(workspace);
    checkSkin(workspace, 0.008F, 0.00094F, 0.00057F);
    Type(Field(workspace, redField), QStringLiteral("5"));
    REQUIRE(workspace.Apply() == true);
    CHECK(errorOrNull->isHidden() == true);
    CHECK(errorOrNull->text().isEmpty() == true);
    WaitForValidatedScene(workspace);
    checkSkin(workspace, 0.005F, 0.00094F, 0.00057F);

    // Undo and redo the distance edit.
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkSkin(workspace, 0.008F, 0.00094F, 0.00057F);
    REQUIRE(workspace.CanRedo() == true);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkSkin(workspace, 0.005F, 0.00094F, 0.00057F);

    // The base colour and roughness fields start from the inherited material, shown as authored.
    REQUIRE(workspace.SelectEntity(head) == true);
    const QString baseRedField = QStringLiteral("scene/skin/base_color_red");
    const QString roughnessField = QStringLiteral("scene/skin/roughness");
    constexpr ludus::foundation::int32 FLOAT32_ROUND_TRIP_DIGITS = 9;
    CHECK(Field(workspace, roughnessField)->text() ==
          QString::number(GetShortestFloat32Decimal(inheritedRoughness), 'g', FLOAT32_ROUND_TRIP_DIGITS));
    Type(Field(workspace, baseRedField), QStringLiteral("0.7"));
    Type(Field(workspace, roughnessField), QStringLiteral("0.55"));
    REQUIRE(workspace.Apply() == true);
    WaitForValidatedScene(workspace);
    const Vector3 editedReflectance{0.7F, inheritedReflectance.Y, inheritedReflectance.Z};
    checkSurface(workspace, editedReflectance, 0.55F);
    checkSkin(workspace, 0.005F, 0.00094F, 0.00057F);

    // An out-of-range roughness is rejected beside the skin fields; the last applied surface stays.
    Type(Field(workspace, roughnessField), QStringLiteral("0.01"));
    CHECK(workspace.Apply() == false);
    QLabel* surfaceErrorOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldSkinError"));
    REQUIRE(surfaceErrorOrNull != nullptr);
    CHECK(surfaceErrorOrNull->isHidden() == false);
    CHECK(surfaceErrorOrNull->text().contains(QStringLiteral("Roughness")) == true);
    CHECK(surfaceErrorOrNull->text().contains(QStringLiteral("0.045 to 1")) == true);
    WaitForValidatedScene(workspace);
    checkSurface(workspace, editedReflectance, 0.55F);
    Type(Field(workspace, roughnessField), QStringLiteral("0.6"));
    REQUIRE(workspace.Apply() == true);
    CHECK(surfaceErrorOrNull->isHidden() == true);
    WaitForValidatedScene(workspace);
    checkSurface(workspace, editedReflectance, 0.6F);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkSurface(workspace, editedReflectance, 0.55F);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkSurface(workspace, editedReflectance, 0.6F);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    workspace.StopPreview();
    workspace.hide();

    // Save, reopen and reimport keep the slot's skin and its edits.
    WorldWorkspace reopened;
    if (PreviewViewport::CanPresent() == true)
    {
        reopened.show();
    }
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    checkSkin(reopened, 0.005F, 0.00094F, 0.00057F);
    checkSurface(reopened, editedReflectance, 0.6F);
    REQUIRE(reopened.ImportScene(source) == true);
    WaitForValidatedScene(reopened);
    checkSkin(reopened, 0.005F, 0.00094F, 0.00057F);
    checkSurface(reopened, editedReflectance, 0.6F);
    REQUIRE(reopened.SelectEntity(head) == true);
    CHECK(Field(reopened, redField)->text() == QStringLiteral("5"));
    CHECK(Field(reopened, baseRedField)->text() == QStringLiteral("0.7"));
    CHECK(Field(reopened, roughnessField)->text() == QStringLiteral("0.6"));

    // Use Inherited Material restores the imported material in one undoable edit.
    QPushButton* restoreOrNull = reopened.findChild<QPushButton*>(QStringLiteral("worldRestoreSkin"));
    REQUIRE(restoreOrNull != nullptr);
    restoreOrNull->click();
    WaitForValidatedScene(reopened);
    CHECK(getMaterial(reopened).Scattering.Model == format::MaterialModel::MetallicRoughness);
    checkSurface(reopened, inheritedReflectance, inheritedRoughness);
    reopened.Undo();
    WaitForValidatedScene(reopened);
    checkSkin(reopened, 0.005F, 0.00094F, 0.00057F);
    checkSurface(reopened, editedReflectance, 0.6F);
    reopened.StopPreview();
}

TEST_CASE("World imports, inspects and maps closure programs across save, reopen and reimport",
          "[editor][world][program]")
{
    namespace format = ludus::content::scene_format;
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    const QString block = directory.filePath(QStringLiteral("closure-program-block.luscene.json"));
    REQUIRE(QFile::copy(QDir(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT))
                            .filePath(QStringLiteral("tests/scenes/materials/closure-program-block.luscene.json")),
                        block) == true);
    const QString fibers = directory.filePath(QStringLiteral("fibers.pbrt"));
    Write(fibers, QByteArrayLiteral(R"(LookAt 0 0 4 0 0 0 0 1 0
Camera "perspective" "float fov" 55
WorldBegin
MakeNamedMaterial "Fiber" "string type" "hair" "float eta" 1.55
NamedMaterial "Fiber"
Shape "trianglemesh" "point3 P" [-1.7 -0.7 0 -0.5 -0.7 0 -1.1 0.7 0] "integer indices" [0 1 2]
)"));
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    REQUIRE(workspace.ImportScene(block) == true);
    WaitForValidatedScene(workspace);
    REQUIRE(workspace.ImportScene(fibers) == true);
    WaitForValidatedScene(workspace);

    const auto findObjects = [](const WorldWorkspace& view, format::MaterialModel model) {
        QStringList ids;
        const RenderScene& scene = GetLevel(view).PreviewScene();
        for (const PreviewObject& object : scene.Objects)
        {
            const ludus::resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(scene, object.Id);
            if (materialOrNull != nullptr && materialOrNull->Scattering.Model == model)
            {
                ids.append(object.Id);
            }
        }
        return ids;
    };
    const auto checkProgram = [](const WorldWorkspace& view, const QString& id) {
        const RenderScene& scene = GetLevel(view).PreviewScene();
        const ludus::resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(scene, id);
        REQUIRE(materialOrNull != nullptr);
        CHECK(materialOrNull->Scattering.Model == format::MaterialModel::Program);
        CHECK(format::IsMaterialProgramValid(materialOrNull->Program) == true);
        CHECK(materialOrNull->Program.LobeCount == 3);
        CHECK(materialOrNull->Program.Lobes[1].Operation == format::MaterialLobeOperation::Conductor);
        CHECK(format::SupportsFixedDimensionReflection(materialOrNull->Scattering, materialOrNull->Program) == true);
        // The World's renderer surfaces carry the program to every preview profile.
        const PreviewSurface& surface = scene.Surfaces[GetPresentedObject(view, id).Surface];
        CHECK(surface.Scattering.Model == format::MaterialModel::Program);
        CHECK(surface.Program.LobeCount == 3);
        CHECK(surface.HasUnresolvedMaterial == false);
    };
    const QStringList programs = findObjects(workspace, format::MaterialModel::Program);
    REQUIRE(programs.size() == 1);
    const QString& program = programs[0];
    checkProgram(workspace, program);

    PreviewViewport& viewport = GetLevel(workspace).GetViewport();
    viewport.GetObjectInteraction().SelectionRequested(program);
    const auto label = [&workspace](const QString& name) {
        const QLabel* labelOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldMaterial:") + name);
        REQUIRE(labelOrNull != nullptr);
        return labelOrNull->text();
    };
    CHECK(label(QStringLiteral("Model")) == QStringLiteral("Closure program"));
    CHECK(label(QStringLiteral("Lobe 1")) == QStringLiteral("Diffuse"));
    CHECK(label(QStringLiteral("Lobe 2")).startsWith(QStringLiteral("Conductor")) == true);
    CHECK(label(QStringLiteral("Lobe 3")) == QStringLiteral("Cosine power 12"));
    CHECK(label(QStringLiteral("Capabilities")) == QStringLiteral("Continuous reflection, fixed three-value sampler"));
    CHECK(label(QStringLiteral("Metallic")) == QStringLiteral("0.7"));
    // The short label leaves its qualifier to the tooltip.
    const QLabel* metallicOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldMaterial:Metallic"));
    REQUIRE(metallicOrNull != nullptr);
    CHECK(metallicOrNull->toolTip().contains(QStringLiteral("an input of the closure program")) == true);

    // Map the unsupported PBRT material to the imported program.
    const QStringList unsupported = findObjects(workspace, format::MaterialModel::Unresolved);
    REQUIRE(unsupported.size() == 1);
    viewport.GetObjectInteraction().SelectionRequested(unsupported[0]);
    QComboBox* replacementsOrNull = workspace.findChild<QComboBox*>(QStringLiteral("worldMaterialReplacement"));
    REQUIRE(replacementsOrNull != nullptr);
    const ludus::foundation::int32 choice =
        replacementsOrNull->findText(QStringLiteral("Weathered gold"), Qt::MatchStartsWith);
    REQUIRE(choice >= 0);
    replacementsOrNull->setCurrentIndex(choice);
    QPushButton* mapOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldMapMaterial"));
    REQUIRE(mapOrNull != nullptr);
    REQUIRE(mapOrNull->isEnabled() == true);
    QTimer::singleShot(0, []() {
        QMessageBox* dialogOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QPushButton* applyOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("worldConfirmMaterialMapping"));
        REQUIRE(applyOrNull != nullptr);
        applyOrNull->click();
    });
    mapOrNull->click();
    WaitForValidatedScene(workspace);
    checkProgram(workspace, unsupported[0]);
    CHECK(findObjects(workspace, format::MaterialModel::Unresolved).isEmpty() == true);
    const QTreeWidget* materialsOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldMaterials"));
    REQUIRE(materialsOrNull != nullptr);
    REQUIRE(materialsOrNull->topLevelItemCount() == 3);
    const QList<QTreeWidgetItem*> goldRows =
        materialsOrNull->findItems(QStringLiteral("Weathered gold"), Qt::MatchExactly);
    REQUIRE(goldRows.size() == 1);
    CHECK(goldRows.front()->childCount() == 2);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    workspace.StopPreview();

    WorldWorkspace reopened;
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    checkProgram(reopened, program);
    checkProgram(reopened, unsupported[0]);
    // Reimporting the scene keeps the program and the mapping.
    REQUIRE(reopened.ImportScene(block) == true);
    WaitForValidatedScene(reopened);
    const QStringList reimported = findObjects(reopened, format::MaterialModel::Program);
    CHECK(reimported.size() >= 2);
    for (const QString& id : reimported)
    {
        checkProgram(reopened, id);
    }
    reopened.StopPreview();
}

TEST_CASE("World maps a shared unsupported PBRT material and retains the repair on reimport",
          "[editor][world][material-repair]")
{
    namespace format = ludus::content::scene_format;
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    const QString source = directory.filePath(QStringLiteral("fibers.pbrt"));
    Write(source, QByteArrayLiteral(R"(LookAt 0 0 4 0 0 0 0 1 0
Camera "perspective" "float fov" 55
WorldBegin
MakeNamedMaterial "Fiber" "string type" "hair" "float eta" 1.55
MakeNamedMaterial "Matte" "string type" "diffuse" "rgb reflectance" [0.2 0.5 0.2]
NamedMaterial "Fiber"
Shape "trianglemesh" "point3 P" [-1.7 -0.7 0 -0.5 -0.7 0 -1.1 0.7 0] "integer indices" [0 1 2]
NamedMaterial "Matte"
Shape "trianglemesh" "point3 P" [-0.4 -0.7 0 0.4 -0.7 0 0 0.7 0] "integer indices" [0 1 2]
)"));

    WorldWorkspace workspace;
    const bool canPresent =
        PreviewViewport::CanPresent() == true && QApplication::platformName() == QStringLiteral("cocoa");
    if (canPresent == true)
    {
        workspace.resize(1024, 720);
        workspace.show();
    }
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForValidatedScene(workspace);
    REQUIRE(GetLevel(workspace).PreviewScene().Objects.size() == 2);
    // The cooker groups PBRT geometry by source material. Import it again to place
    // the same named material on a second distinct World slot.
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForValidatedScene(workspace);
    const RenderScene& imported = GetLevel(workspace).PreviewScene();
    REQUIRE(imported.Objects.size() == 4);
    const auto getBinding = [](const WorldWorkspace& view, const QString& id) {
        return view.Document().ScenePlacement(view.Document().Entity(id)).value(QStringLiteral("material")).toString();
    };
    QStringList unsupported;
    QString replacement;
    for (const PreviewObject& object : imported.Objects)
    {
        if (object.Name == QStringLiteral("Fiber"))
        {
            unsupported.append(object.Id);
        }
        else if (object.Name == QStringLiteral("Matte"))
        {
            replacement = getBinding(workspace, object.Id);
        }
    }
    REQUIRE(unsupported.size() == 2);
    REQUIRE(replacement.isEmpty() == false);
    const QString first = unsupported[0];
    const QString second = unsupported[1];
    const QString original = getBinding(workspace, first);
    CHECK(getBinding(workspace, second) == original);
    REQUIRE(original != replacement);

    const auto checkMaterial = [](const WorldWorkspace& view, const QString& id, format::MaterialModel model) {
        const RenderScene& scene = GetLevel(view).PreviewScene();
        const ludus::resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(scene, id);
        REQUIRE(materialOrNull != nullptr);
        CHECK(materialOrNull->Scattering.Model == model);
        const PreviewSurface& surface = scene.Surfaces[GetPresentedObject(view, id).Surface];
        CHECK(surface.HasUnresolvedMaterial == (model == format::MaterialModel::Unresolved));
        if (model == format::MaterialModel::Unresolved)
        {
            CHECK(surface.FlatTint == UNRESOLVED_MATERIAL_TINT);
            CHECK(surface.Emission == ludus::foundation::math::Vector3{});
            CHECK(materialOrNull->Name.View() == "Fiber");
        }
    };
    checkMaterial(workspace, first, format::MaterialModel::Unresolved);
    checkMaterial(workspace, second, format::MaterialModel::Unresolved);

    PreviewViewport& viewport = GetLevel(workspace).GetViewport();
    if (canPresent == true)
    {
        GetLevel(workspace).GetRenderPreview().SetShading(PreviewShading::Lit);
        REQUIRE(QTest::qWaitFor([&viewport]() { return viewport.PresentedFrames() > 0; },
                                MAX_SCENE_WAIT_MILLISECONDS) == true);
        QQuickWindow* frameWindowOrNull = nullptr;
        for (QWindow* window : QGuiApplication::allWindows())
        {
            if (window->objectName() == QStringLiteral("LudusGpuViewport") && window->isVisible() == true)
            {
                frameWindowOrNull = qobject_cast<QQuickWindow*>(window);
                break;
            }
        }
        REQUIRE(frameWindowOrNull != nullptr);
        REQUIRE(QTest::qWaitForWindowExposed(frameWindowOrNull, MAX_SCENE_WAIT_MILLISECONDS) == true);
        const QImage pixels = frameWindowOrNull->grabWindow();
        REQUIRE(pixels.isNull() == false);
        ludus::foundation::usize purplePixels = 0;
        for (ludus::foundation::int32 y = 0; y < pixels.height(); ++y)
        {
            for (ludus::foundation::int32 x = 0; x < pixels.width(); ++x)
            {
                const QRgb pixel = pixels.pixel(x, y);
                if (qRed(pixel) > 100 && qBlue(pixel) > 100 && qGreen(pixel) < 40)
                {
                    ++purplePixels;
                }
            }
        }
        CHECK(purplePixels > 100);
    }

    viewport.GetObjectInteraction().SelectionRequested(first);
    const QLabel* nameOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldMaterial:Name"));
    REQUIRE(nameOrNull != nullptr);
    CHECK(nameOrNull->text() == QStringLiteral("Fiber"));
    const QLabel* modelOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldMaterial:Model"));
    REQUIRE(modelOrNull != nullptr);
    CHECK(modelOrNull->text() == QStringLiteral("Unsupported imported material"));
    const QString catalogPath = directory.filePath(QStringLiteral("assets/catalog.json"));
    const QByteArray catalog = Read(catalogPath);
    QPushButton* mapOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldMapMaterial"));
    REQUIRE(mapOrNull != nullptr);
    REQUIRE(mapOrNull->isEnabled() == true);
    const QJsonObject before = workspace.Document().Draft();
    QTimer::singleShot(0, []() {
        QMessageBox* dialogOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        CHECK(dialogOrNull->defaultButton() == dialogOrNull->button(QMessageBox::Cancel));
        dialogOrNull->button(QMessageBox::Cancel)->click();
    });
    mapOrNull->click();
    CHECK(workspace.Document().Draft() == before);

    QTimer::singleShot(0, [&first, &second]() {
        QMessageBox* dialogOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        CHECK(dialogOrNull->informativeText().contains(first) == true);
        CHECK(dialogOrNull->informativeText().contains(second) == true);
        QPushButton* applyOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("worldConfirmMaterialMapping"));
        REQUIRE(applyOrNull != nullptr);
        applyOrNull->click();
    });
    mapOrNull->click();
    WaitForValidatedScene(workspace);
    checkMaterial(workspace, first, format::MaterialModel::Diffuse);
    checkMaterial(workspace, second, format::MaterialModel::Diffuse);
    CHECK(getBinding(workspace, first) == original);
    CHECK(getBinding(workspace, second) == original);

    workspace.Undo();
    WaitForValidatedScene(workspace);
    CHECK(workspace.Document().Draft() == before);
    checkMaterial(workspace, first, format::MaterialModel::Unresolved);
    checkMaterial(workspace, second, format::MaterialModel::Unresolved);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkMaterial(workspace, first, format::MaterialModel::Diffuse);
    checkMaterial(workspace, second, format::MaterialModel::Diffuse);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    CHECK(workspace.Dirty() == false);
    CHECK(Read(catalogPath) == catalog);
    workspace.StopPreview();
    workspace.hide();

    WorldWorkspace reopened;
    if (canPresent == true)
    {
        reopened.resize(1024, 720);
        reopened.show();
    }
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    checkMaterial(reopened, first, format::MaterialModel::Diffuse);
    checkMaterial(reopened, second, format::MaterialModel::Diffuse);
    REQUIRE(reopened.ImportScene(source) == true);
    WaitForValidatedScene(reopened);
    const RenderScene& reimported = GetLevel(reopened).PreviewScene();
    REQUIRE(reimported.Objects.size() == 6);
    ludus::foundation::usize sharedSlots = 0;
    for (const PreviewObject& object : reimported.Objects)
    {
        if (getBinding(reopened, object.Id) == original)
        {
            ++sharedSlots;
            checkMaterial(reopened, object.Id, format::MaterialModel::Diffuse);
        }
    }
    CHECK(sharedSlots == 3);
    checkMaterial(reopened, first, format::MaterialModel::Diffuse);
    checkMaterial(reopened, second, format::MaterialModel::Diffuse);
    reopened.StopPreview();
}

TEST_CASE("World creates a material from a shared unsupported PBRT import across undo, failure, save and reimport",
          "[editor][world][material-repair]")
{
    namespace format = ludus::content::scene_format;
    using ludus::foundation::math::Vector3;
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    const QString source = directory.filePath(QStringLiteral("fibers.pbrt"));
    Write(source, QByteArrayLiteral(R"(LookAt 0 0 4 0 0 0 0 1 0
Camera "perspective" "float fov" 55
WorldBegin
MakeNamedMaterial "Fiber" "string type" "hair" "rgb reflectance" [0.5 0.25 0.125] "float eta" 1.55
NamedMaterial "Fiber"
Shape "trianglemesh" "point3 P" [-1.7 -0.7 0 -0.5 -0.7 0 -1.1 0.7 0] "integer indices" [0 1 2]
)"));
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    // Import twice to place the same named material on two distinct World slots.
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForValidatedScene(workspace);
    REQUIRE(workspace.ImportScene(source) == true);
    WaitForValidatedScene(workspace);
    const auto getBinding = [](const WorldWorkspace& view, const QString& id) {
        return view.Document().ScenePlacement(view.Document().Entity(id)).value(QStringLiteral("material")).toString();
    };
    QStringList fibers;
    for (const PreviewObject& object : GetLevel(workspace).PreviewScene().Objects)
    {
        fibers.append(object.Id);
    }
    REQUIRE(fibers.size() == 2);
    const QString original = getBinding(workspace, fibers[0]);
    REQUIRE(getBinding(workspace, fibers[1]) == original);
    const auto getMaterial = [](const RenderScene& scene, const QString& id) -> const ludus::resources::SceneMaterial& {
        const ludus::resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(scene, id);
        REQUIRE(materialOrNull != nullptr);
        return *materialOrNull;
    };
    const auto checkUnresolved = [&](const RenderScene& scene, const QString& id) {
        CHECK(getMaterial(scene, id).Scattering.Model == format::MaterialModel::Unresolved);
    };
    const auto checkCreated = [&](const RenderScene& scene,
                                  const QString& id,
                                  format::MaterialModel model,
                                  const Vector3& baseColor,
                                  ludus::foundation::float32 roughness) {
        const ludus::resources::SceneMaterial& material = getMaterial(scene, id);
        CHECK(material.Scattering.Model == model);
        CHECK(material.Reflectance == baseColor);
        CHECK(material.Roughness == roughness);
        CHECK(material.Metallic == 0);
        CHECK(material.Name.View() == "Fiber");
    };
    const Vector3 imported{0.5F, 0.25F, 0.125F};
    // The unsupported import keeps its recoverable reflectance for the draft.
    checkUnresolved(GetLevel(workspace).PreviewScene(), fibers[0]);
    CHECK(getMaterial(GetLevel(workspace).PreviewScene(), fibers[0]).Reflectance == imported);

    REQUIRE(workspace.SelectEntity(fibers[0]) == true);
    QPushButton* createOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldCreateMaterial"));
    REQUIRE(createOrNull != nullptr);
    REQUIRE(createOrNull->isEnabled() == true);
    const QString catalogPath = directory.filePath(QStringLiteral("assets/catalog.json"));
    const QByteArray catalog = Read(catalogPath);
    const QJsonObject before = workspace.Document().Draft();
    const auto getDialog = []() -> WorldMaterialCreationDialog& {
        WorldMaterialCreationDialog* dialogOrNull =
            qobject_cast<WorldMaterialCreationDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        return *dialogOrNull;
    };
    const auto getInput = [](const WorldMaterialCreationDialog& dialog, const QString& name) -> QLineEdit& {
        QLineEdit* inputOrNull = dialog.findChild<QLineEdit*>(name);
        REQUIRE(inputOrNull != nullptr);
        return *inputOrNull;
    };

    // The draft names the source identity and both affected slots, starts from the imported values, and Cancel,
    // the default, changes nothing.
    QTimer::singleShot(0, [&]() {
        WorldMaterialCreationDialog& dialog = getDialog();
        const QLabel* identityOrNull = dialog.findChild<QLabel*>(QStringLiteral("worldMaterialCreationSource"));
        REQUIRE(identityOrNull != nullptr);
        CHECK(identityOrNull->text().contains(QStringLiteral("Fiber · ") + original) == true);
        const QLabel* slotsOrNull = dialog.findChild<QLabel*>(QStringLiteral("worldMaterialCreationSlots"));
        REQUIRE(slotsOrNull != nullptr);
        CHECK(slotsOrNull->text().contains(fibers[0]) == true);
        CHECK(slotsOrNull->text().contains(fibers[1]) == true);
        // The untextured import has no maps to keep; its colour is not shown as a one-texel swatch.
        const QLabel* keptOrNull = dialog.findChild<QLabel*>(QStringLiteral("worldMaterialCreationKept"));
        REQUIRE(keptOrNull != nullptr);
        CHECK(keptOrNull->text().contains(QStringLiteral("no maps")) == true);
        CHECK(getInput(dialog, QStringLiteral("worldMaterialCreationBaseColor0")).text().toFloat() == imported.X);
        CHECK(getInput(dialog, QStringLiteral("worldMaterialCreationBaseColor1")).text().toFloat() == imported.Y);
        CHECK(getInput(dialog, QStringLiteral("worldMaterialCreationBaseColor2")).text().toFloat() == imported.Z);
        QDialogButtonBox* buttonsOrNull = dialog.findChild<QDialogButtonBox*>();
        REQUIRE(buttonsOrNull != nullptr);
        QPushButton* cancelOrNull = buttonsOrNull->button(QDialogButtonBox::Cancel);
        REQUIRE(cancelOrNull != nullptr);
        CHECK(cancelOrNull->isDefault() == true);
        cancelOrNull->click();
    });
    createOrNull->click();
    CHECK(workspace.Document().Draft() == before);

    // A rejected field is reported in the dialog, which keeps the typed text; the corrected draft applies.
    const Vector3 baseColor{0.25F, 0.5F, 0.75F};
    QTimer::singleShot(0, [&]() {
        WorldMaterialCreationDialog& dialog = getDialog();
        QPushButton* applyOrNull = dialog.findChild<QPushButton*>(QStringLiteral("worldConfirmMaterialCreation"));
        REQUIRE(applyOrNull != nullptr);
        QLineEdit& roughness = getInput(dialog, QStringLiteral("worldMaterialCreationRoughness"));
        roughness.setText(QStringLiteral("0.01"));
        applyOrNull->click();
        const QLabel* errorOrNull = dialog.findChild<QLabel*>(QStringLiteral("worldMaterialCreationError"));
        REQUIRE(errorOrNull != nullptr);
        CHECK(errorOrNull->isHidden() == false);
        CHECK(errorOrNull->text().contains(QStringLiteral("Roughness")) == true);
        CHECK(roughness.text() == QStringLiteral("0.01"));
        CHECK(dialog.isVisible() == true);
        CHECK(workspace.Document().Draft() == before);
        roughness.setText(QStringLiteral("0.6"));
        getInput(dialog, QStringLiteral("worldMaterialCreationMetallic")).setText(QStringLiteral("0"));
        getInput(dialog, QStringLiteral("worldMaterialCreationBaseColor0")).setText(QStringLiteral("0.25"));
        getInput(dialog, QStringLiteral("worldMaterialCreationBaseColor1")).setText(QStringLiteral("0.5"));
        getInput(dialog, QStringLiteral("worldMaterialCreationBaseColor2")).setText(QStringLiteral("0.75"));
        applyOrNull->click();
    });
    createOrNull->click();
    WaitForValidatedScene(workspace);
    for (const QString& id : fibers)
    {
        checkCreated(GetLevel(workspace).PreviewScene(), id, format::MaterialModel::MetallicRoughness, baseColor, 0.6F);
        CHECK(getBinding(workspace, id) == original);
    }
    // The World holds the repair; the project catalog is unchanged.
    CHECK(Read(catalogPath) == catalog);

    // The creation is one undoable edit.
    workspace.Undo();
    WaitForValidatedScene(workspace);
    CHECK(workspace.Document().Draft() == before);
    checkUnresolved(GetLevel(workspace).PreviewScene(), fibers[0]);
    checkUnresolved(GetLevel(workspace).PreviewScene(), fibers[1]);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkCreated(GetLevel(workspace).PreviewScene(),
                 fibers[1],
                 format::MaterialModel::MetallicRoughness,
                 baseColor,
                 0.6F);

    // A replacement whose preparation fails keeps the last published material visible and reports the failure.
    REQUIRE(workspace.SelectEntity(fibers[1]) == true);
    QPushButton* editOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldEditCreatedMaterial"));
    REQUIRE(editOrNull != nullptr);
    const QJsonObject resources =
        QJsonDocument::fromJson(catalog).object().value(QStringLiteral("resources")).toObject();
    const QString artifactPath =
        directory.filePath(resources.value(original).toObject().value(QStringLiteral("path")).toString());
    const QByteArray artifact = Read(artifactPath);
    REQUIRE(artifact.isEmpty() == false);
    Write(artifactPath, QByteArrayLiteral("{\"damaged\":true}"));
    QTimer::singleShot(0, [&]() {
        WorldMaterialCreationDialog& dialog = getDialog();
        QComboBox* modelOrNull = dialog.findChild<QComboBox*>(QStringLiteral("worldMaterialCreationModel"));
        REQUIRE(modelOrNull != nullptr);
        modelOrNull->setCurrentIndex(modelOrNull->findData(QStringLiteral("diffuse")));
        CHECK(getInput(dialog, QStringLiteral("worldMaterialCreationRoughness")).isEnabled() == false);
        QPushButton* applyOrNull = dialog.findChild<QPushButton*>(QStringLiteral("worldConfirmMaterialCreation"));
        REQUIRE(applyOrNull != nullptr);
        CHECK(applyOrNull->text() == QStringLiteral("Apply changes"));
        applyOrNull->click();
    });
    editOrNull->click();
    LevelWorkspace& level = GetLevel(workspace);
    REQUIRE(QTest::qWaitFor([&level]() { return level.CanStartOperation() == true; }, MAX_SCENE_WAIT_MILLISECONDS) ==
            true);
    CHECK(level.StatusText().isEmpty() == false);
    checkCreated(level.GetPresentedScene(), fibers[0], format::MaterialModel::MetallicRoughness, baseColor, 0.6F);
    // Repair the project and undo the failed edit: the last good material publishes again.
    Write(artifactPath, artifact);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkCreated(level.GetPresentedScene(), fibers[0], format::MaterialModel::MetallicRoughness, baseColor, 0.6F);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkCreated(GetLevel(workspace).PreviewScene(), fibers[0], format::MaterialModel::Diffuse, baseColor, 1);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    CHECK(workspace.Dirty() == false);
    workspace.StopPreview();

    // Reopening keeps the created material, and reimporting the source applies it to the new slot as well.
    WorldWorkspace reopened;
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    for (const QString& id : fibers)
    {
        checkCreated(GetLevel(reopened).PreviewScene(), id, format::MaterialModel::Diffuse, baseColor, 1);
    }
    REQUIRE(reopened.ImportScene(source) == true);
    WaitForValidatedScene(reopened);
    const RenderScene& reimported = GetLevel(reopened).PreviewScene();
    REQUIRE(reimported.Objects.size() == 3);
    for (const PreviewObject& object : reimported.Objects)
    {
        CHECK(getBinding(reopened, object.Id) == original);
        checkCreated(reimported, object.Id, format::MaterialModel::Diffuse, baseColor, 1);
    }
    reopened.StopPreview();
}

TEST_CASE("World coating instances share storage and retain slot and shared edits on reimport",
          "[editor][world][coating][instancing]")
{
    using namespace ludus::foundation;
    namespace format = ludus::content::scene_format;
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    const QString external = directory.filePath(QStringLiteral("external"));
    REQUIRE(QDir().mkpath(external) == true);
    const QString source = QDir(external).filePath(QStringLiteral("coat.pbrt"));
    Write(source, QByteArrayLiteral(R"(LookAt 0 0 3 0 0 0 0 1 0
Camera "perspective" "float fov" 45
WorldBegin
MakeNamedMaterial "Varnish" "string type" "coateddiffuse" "rgb reflectance" [0.5 0.3 0.1]
    "float roughness" 0.125 "bool remaproughness" false
MakeNamedMaterial "Imported" "string type" "hair"
NamedMaterial "Imported"
Shape "trianglemesh" "point3 P" [-1 -1 0 1 -1 0 0 1 0] "integer indices" [0 1 2]
NamedMaterial "Varnish"
Shape "trianglemesh" "point3 P" [2 -1 0 3 -1 0 2 1 0] "integer indices" [0 1 2]
)"));
    QString error;
    const QString relativeLevel = QStringLiteral("scenes/coat.lulevel");
    REQUIRE(ludus::editor::tests::PrepareImportedLevel(directory.path(), relativeLevel, external, error, "coat.pbrt"));
    const QJsonObject level = QJsonDocument::fromJson(Read(directory.filePath(relativeLevel))).object();
    WorldDocument document = Document();
    REQUIRE(document.ImportScene(level, error) == true);
    QString first;
    QString donor;
    for (const QJsonValue value : level.value(QStringLiteral("objects")).toArray())
    {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        if (name == QStringLiteral("Imported"))
        {
            first = object.value(QStringLiteral("id")).toString();
        }
        else if (name == QStringLiteral("Varnish"))
        {
            donor = object.value(QStringLiteral("id")).toString();
        }
    }
    REQUIRE(first.isEmpty() == false);
    REQUIRE(donor.isEmpty() == false);
    const QString material =
        document.ScenePlacement(document.Entity(first)).value(QStringLiteral("material")).toString();
    const QString replacement =
        document.ScenePlacement(document.Entity(donor)).value(QStringLiteral("material")).toString();
    REQUIRE(document.MapSceneMaterial(material, replacement, error) == true);
    REQUIRE(document.RemoveEntity(donor, error) == true);
    QString second;
    REQUIRE(document.DuplicateEntity(ludus::platform::GetSystemEntropySource(), error, first, second) == true);
    Write(path, document.Encode());

    WorldWorkspace workspace;
    if (PreviewViewport::CanPresent() == true)
    {
        workspace.show();
    }
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    WaitForValidatedScene(workspace);

    const auto checkAlpha = [](const WorldWorkspace& view, const QString& id, float32 alpha) {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(view).PreviewScene(), id);
        REQUIRE(materialOrNull != nullptr);
        CHECK(materialOrNull->Scattering.Model == format::MaterialModel::CoatedDiffuse);
        CHECK(materialOrNull->Scattering.AlphaU == alpha);
    };
    const auto checkSharing = [](const WorldWorkspace& view, usize count) {
        const RenderScene& scene = GetLevel(view).PreviewScene();
        REQUIRE(scene.Definition != nullptr);
        CHECK(scene.Definition->Materials.GetSize() == count);
        Array<usize> owners;
        REQUIRE(preview_lighting::BuildMaterialOwners(scene, owners) == true);
        usize unique = 0;
        for (usize index = 0; index < owners.GetSize(); ++index)
        {
            if (owners[index] == index)
            {
                ++unique;
            }
        }
        CHECK(unique == count);
    };
    const QString alphaField = QStringLiteral("scene/coating/alpha");
    const auto applyShared = [](WorldWorkspace& inoutView, const QString& included, const QString& excluded) {
        QPushButton* sharedOrNull = inoutView.findChild<QPushButton*>(QStringLiteral("worldApplySharedCoating"));
        REQUIRE(sharedOrNull != nullptr);
        REQUIRE(sharedOrNull->isEnabled() == true);
        QTimer::singleShot(0, [&included, &excluded]() {
            QMessageBox* dialogOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            REQUIRE(dialogOrNull != nullptr);
            CHECK(dialogOrNull->defaultButton() == dialogOrNull->button(QMessageBox::Cancel));
            CHECK(dialogOrNull->informativeText().contains(included) == true);
            if (excluded.isEmpty() == false)
            {
                CHECK(dialogOrNull->informativeText().contains(excluded) == false);
            }
            QPushButton* applyOrNull =
                dialogOrNull->findChild<QPushButton*>(QStringLiteral("worldConfirmSharedCoating"));
            REQUIRE(applyOrNull != nullptr);
            applyOrNull->click();
        });
        sharedOrNull->click();
        WaitForValidatedScene(inoutView);
    };

    checkSharing(workspace, 1);
    RenderPreview& preview = GetLevel(workspace).GetRenderPreview();
    const bool canVerifyDraws =
        PreviewViewport::CanPresent() == true && QApplication::platformName() == QStringLiteral("cocoa");
    uint32 sharedDraws = 0;
    if (canVerifyDraws == true)
    {
        preview.SetShading(PreviewShading::Lit);
        REQUIRE(QTest::qWaitFor([&preview]() { return preview.Report().Visible == 2 && preview.Frames() > 0; },
                                MAX_SCENE_WAIT_MILLISECONDS) == true);
        sharedDraws = preview.Report().Draws;
        CHECK(sharedDraws > 0);
    }
    const auto checkDraws = [&](uint32 draws) {
        if (canVerifyDraws == true)
        {
            REQUIRE(QTest::qWaitFor([&preview, draws]() { return preview.Report().Draws == draws; },
                                    MAX_SCENE_WAIT_MILLISECONDS) == true);
        }
    };
    REQUIRE(workspace.SelectEntity(first) == true);
    Type(Field(workspace, alphaField), QStringLiteral("0.1250000001"));
    REQUIRE(workspace.Apply() == true);
    CHECK(workspace.Document().ScenePlacement(workspace.Document().Entity(first)).contains(QStringLiteral("coating")) ==
          false);
    CHECK(workspace.Document().GetSharedCoating(replacement).isEmpty() == true);
    checkSharing(workspace, 1);

    Type(Field(workspace, alphaField), QStringLiteral("0.25"));
    applyShared(workspace, second, {});
    checkAlpha(workspace, first, 0.25F);
    checkAlpha(workspace, second, 0.25F);
    CHECK(workspace.Document().GetSharedCoating(replacement).value(QStringLiteral("alpha")).toDouble() == 0.25);
    checkSharing(workspace, 1);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkAlpha(workspace, first, 0.125F);
    checkAlpha(workspace, second, 0.125F);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkAlpha(workspace, second, 0.25F);

    REQUIRE(workspace.SelectEntity(first) == true);
    Type(Field(workspace, alphaField), QStringLiteral("0.5"));
    REQUIRE(workspace.Apply() == true);
    WaitForValidatedScene(workspace);
    checkAlpha(workspace, first, 0.5F);
    checkAlpha(workspace, second, 0.25F);
    checkSharing(workspace, 2);
    checkDraws(sharedDraws + 1);
    workspace.Undo();
    WaitForValidatedScene(workspace);
    checkAlpha(workspace, first, 0.25F);
    checkSharing(workspace, 1);
    workspace.Redo();
    WaitForValidatedScene(workspace);
    checkAlpha(workspace, first, 0.5F);
    checkSharing(workspace, 2);

    REQUIRE(workspace.SelectEntity(second) == true);
    Type(Field(workspace, alphaField), QStringLiteral("0.5"));
    applyShared(workspace, second, first);
    checkAlpha(workspace, first, 0.5F);
    checkAlpha(workspace, second, 0.5F);
    checkSharing(workspace, 1);
    checkDraws(sharedDraws);
    Type(Field(workspace, alphaField), QStringLiteral("0.375"));
    applyShared(workspace, second, first);
    checkAlpha(workspace, first, 0.5F);
    checkAlpha(workspace, second, 0.375F);
    checkSharing(workspace, 2);
    REQUIRE(workspace.Save() == true);
    WaitForValidatedScene(workspace);
    CHECK(workspace.Dirty() == false);
    workspace.StopPreview();
    workspace.hide();

    WorldWorkspace reopened;
    if (PreviewViewport::CanPresent() == true)
    {
        reopened.show();
    }
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    checkAlpha(reopened, first, 0.5F);
    checkAlpha(reopened, second, 0.375F);
    REQUIRE(reopened.ImportScene(source) == true);
    WaitForValidatedScene(reopened);
    const RenderScene& imported = GetLevel(reopened).PreviewScene();
    REQUIRE(imported.Objects.size() == 4);
    usize inheritedSlots = 0;
    for (const PreviewObject& object : imported.Objects)
    {
        const QString binding = reopened.Document()
                                    .ScenePlacement(reopened.Document().Entity(object.Id))
                                    .value(QStringLiteral("material"))
                                    .toString();
        if (binding == material)
        {
            ++inheritedSlots;
            checkAlpha(reopened, object.Id, object.Id == first ? 0.5F : 0.375F);
        }
        else
        {
            CHECK(binding == replacement);
            checkAlpha(reopened, object.Id, 0.375F);
        }
    }
    CHECK(inheritedSlots == 3);
    checkSharing(reopened, 2);
    REQUIRE(reopened.SelectEntity(first) == true);
    QPushButton* inheritOrNull = reopened.findChild<QPushButton*>(QStringLiteral("worldRestoreCoating"));
    REQUIRE(inheritOrNull != nullptr);
    inheritOrNull->click();
    WaitForValidatedScene(reopened);
    checkAlpha(reopened, first, 0.375F);
    checkSharing(reopened, 1);
    reopened.StopPreview();
}

TEST_CASE("Material document edits a coated-diffuse slot with World history and preview", "[editor][world][coating]")
{
    namespace format = ludus::content::scene_format;
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    const QString external = directory.filePath(QStringLiteral("external"));
    REQUIRE(QDir().mkpath(external) == true);
    Write(QDir(external).filePath(QStringLiteral("varnish.pbrt")), QByteArrayLiteral(R"(LookAt 0 0 3 0 0 0 0 1 0
Camera "perspective" "float fov" 45
WorldBegin
MakeNamedMaterial "Varnish" "string type" "coateddiffuse" "rgb reflectance" [0.5 0.3 0.1]
    "float roughness" 0.1 "bool remaproughness" false
NamedMaterial "Varnish"
Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0] "integer indices" [0 1 2]
)"));
    QString error;
    const QString relativeLevel = QStringLiteral("scenes/varnish.lulevel");
    REQUIRE(
        ludus::editor::tests::PrepareImportedLevel(directory.path(), relativeLevel, external, error, "varnish.pbrt") ==
        true);
    const QByteArray importedLevel = Read(directory.filePath(relativeLevel));
    const QString catalog = directory.filePath(QStringLiteral("assets/catalog.json"));
    const QByteArray originalCatalog = Read(catalog);
    const QJsonObject level = QJsonDocument::fromJson(importedLevel).object();
    WorldDocument document = Document();
    REQUIRE(document.ImportScene(level, error) == true);
    const QString selected =
        level.value(QStringLiteral("objects")).toArray().first().toObject().value(QStringLiteral("id")).toString();
    QString sibling;
    REQUIRE(document.DuplicateEntity(ludus::platform::GetSystemEntropySource(), error, selected, sibling) == true);
    const QJsonValue originalBinding =
        document.ScenePlacement(document.Entity(selected)).value(QStringLiteral("material"));
    Write(path, document.Encode());

    WorldWorkspace workspace;
    if (PreviewViewport::CanPresent() == true)
    {
        workspace.show();
    }
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    WaitForValidatedScene(workspace);
    REQUIRE(workspace.SelectEntity(selected) == true);
    const auto getMaterial = [](const WorldWorkspace& source, const QString& id) {
        const ludus::resources::SceneMaterial* materialOrNull =
            GetSceneObjectMaterialOrNull(GetLevel(source).PreviewScene(), id);
        REQUIRE(materialOrNull != nullptr);
        return *materialOrNull;
    };
    const format::MaterialScattering original = getMaterial(workspace, selected).Scattering;
    REQUIRE(original.Model == format::MaterialModel::CoatedDiffuse);
    const auto openDocument = [](WorldWorkspace& inoutWorkspace, const QString& entry) {
        QPushButton* openOrNull = inoutWorkspace.findChild<QPushButton*>(entry);
        REQUIRE(openOrNull != nullptr);
        REQUIRE(openOrNull->isEnabled() == true);
        openOrNull->click();
        QDialog* documentOrNull = inoutWorkspace.findChild<QDialog*>(QStringLiteral("worldMaterialDocument"));
        REQUIRE(documentOrNull != nullptr);
        REQUIRE(documentOrNull->isVisible() == true);
        return documentOrNull;
    };
    const auto getInput = [](const QDialog& view, const QString& key) {
        QLineEdit* inputOrNull = view.findChild<QLineEdit*>(QStringLiteral("materialCoating:") + key);
        REQUIRE(inputOrNull != nullptr);
        return inputOrNull;
    };
    const auto click = [](const QDialog& view, const QString& name) {
        QPushButton* buttonOrNull = view.findChild<QPushButton*>(name);
        REQUIRE(buttonOrNull != nullptr);
        REQUIRE(buttonOrNull->isEnabled() == true);
        buttonOrNull->click();
    };
    const auto getLabel = [](const QDialog& view, const QString& name) {
        const QLabel* labelOrNull = view.findChild<QLabel*>(name);
        REQUIRE(labelOrNull != nullptr);
        return labelOrNull->text();
    };
    QPushButton* revealOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldRevealMaterial"));
    REQUIRE(revealOrNull != nullptr);
    revealOrNull->click();
    QTreeWidget* listOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldMaterials"));
    REQUIRE(listOrNull != nullptr);
    REQUIRE(listOrNull->currentItem() != nullptr);
    QTreeWidgetItem* resource = listOrNull->currentItem();
    REQUIRE(resource->childCount() == 2);
    const ludus::foundation::int32 slotIndex = resource->child(0)->data(0, Qt::UserRole).toString() == selected ? 0 : 1;
    listOrNull->setCurrentItem(resource->child(slotIndex));
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    QDialog* view = openDocument(workspace, QStringLiteral("worldEditListedMaterial"));
    const QListWidget* usesOrNull = view->findChild<QListWidget*>(QStringLiteral("materialUses"));
    REQUIRE(usesOrNull != nullptr);
    CHECK(usesOrNull->count() == 2);
    // The opened slot remains the edit target when World selection changes.
    REQUIRE(workspace.SelectEntity(sibling) == true);
    const QString alpha = QStringLiteral("alpha");
    const QString eta = QStringLiteral("eta");
    const QString thickness = QStringLiteral("thickness");
    const QString apply = QStringLiteral("materialApply");
    const QString undo = QStringLiteral("materialUndo");
    const QString preview = QStringLiteral("materialPreviewStatus");
    Type(getInput(*view, alpha), QStringLiteral("0"));
    click(*view, apply);
    CHECK(getLabel(*view, QStringLiteral("materialStatus")).contains(QStringLiteral("enter a finite value")) == true);
    CHECK(getMaterial(workspace, selected).Scattering.AlphaU == original.AlphaU);

    Type(getInput(*view, alpha), QStringLiteral("0.25"));
    Type(getInput(*view, eta), QStringLiteral("1.75"));
    Type(getInput(*view, thickness), QStringLiteral("0.125"));
    CHECK(getMaterial(workspace, selected).Scattering.AlphaU == original.AlphaU);
    click(*view, apply);
    INFO(qPrintable(getLabel(*view, QStringLiteral("materialStatus"))));
    REQUIRE(workspace.Document()
                .ScenePlacement(workspace.Document().Entity(selected))
                .contains(QStringLiteral("coating")) == true);
    CHECK(getLabel(*view, preview).contains(QStringLiteral("Preparing")) == true);
    CHECK(getMaterial(workspace, selected).Scattering.AlphaU == original.AlphaU);
    WaitForValidatedScene(workspace);
    CHECK(getLabel(*view, preview).contains(QStringLiteral("approximates")) == true);
    const auto checkEdited = [&](const WorldWorkspace& source) {
        const ludus::resources::SceneMaterial material = getMaterial(source, selected);
        CHECK(material.Scattering.Model == format::MaterialModel::CoatedDiffuse);
        CHECK(material.Scattering.AlphaU == 0.25F);
        CHECK(material.Scattering.AlphaV == 0.25F);
        CHECK(material.Scattering.CoatingEta == 1.75F);
        CHECK(material.Scattering.CoatingThickness == 0.125F);
        CHECK(material.Roughness == 0.5F);
        CHECK(getMaterial(source, sibling).Scattering.AlphaU == original.AlphaU);
        CHECK(source.Document().ScenePlacement(source.Document().Entity(selected)).value(QStringLiteral("material")) ==
              originalBinding);
    };
    checkEdited(workspace);

    click(*view, undo);
    WaitForValidatedScene(workspace);
    CHECK(getMaterial(workspace, selected).Scattering.AlphaU == original.AlphaU);
    CHECK(getInput(*view, eta)->text().toFloat() == original.CoatingEta);
    click(*view, QStringLiteral("materialRedo"));
    WaitForValidatedScene(workspace);
    checkEdited(workspace);
    CHECK(getInput(*view, eta)->text() == QStringLiteral("1.75"));

    click(*view, QStringLiteral("materialSave"));
    WaitForValidatedScene(workspace);
    REQUIRE(workspace.Dirty() == false);
    CHECK(Read(directory.filePath(relativeLevel)) == importedLevel);
    CHECK(Read(catalog) == originalCatalog);
    REQUIRE(view->close() == true);
    workspace.StopPreview();
    workspace.hide();

    WorldWorkspace reopened;
    if (PreviewViewport::CanPresent() == true)
    {
        reopened.show();
    }
    reopened.SetProject(directory.path(), 2);
    REQUIRE(reopened.Open(path) == true);
    WaitForValidatedScene(reopened);
    REQUIRE(reopened.SelectEntity(selected) == true);
    checkEdited(reopened);
    QDialog* reopenedView = openDocument(reopened, QStringLiteral("worldEditMaterial"));
    CHECK(getInput(*reopenedView, thickness)->text() == QStringLiteral("0.125"));

    // Force the cook to fail after Apply. The draft and its history survive with the last accepted scene.
    const QString hiddenCatalog = catalog + QStringLiteral(".hidden");
    REQUIRE(QFile::rename(catalog, hiddenCatalog) == true);
    Type(getInput(*reopenedView, alpha), QStringLiteral("0.5"));
    click(*reopenedView, apply);
    WaitForValidatedScene(reopened);
    CHECK(getMaterial(reopened, selected).Scattering.AlphaU == 0.25F);
    CHECK(getInput(*reopenedView, alpha)->text() == QStringLiteral("0.5"));
    CHECK(getLabel(*reopenedView, preview).contains(QStringLiteral("Viewport preparation failed")) == true);
    REQUIRE(QFile::rename(hiddenCatalog, catalog) == true);
    click(*reopenedView, undo);
    WaitForValidatedScene(reopened);
    checkEdited(reopened);

    CHECK(getLabel(*reopenedView, preview).contains(QStringLiteral("Viewport preparation failed")) == false);
    click(*reopenedView, QStringLiteral("materialInherit"));
    QMessageBox* confirmationOrNull =
        reopenedView->findChild<QMessageBox*>(QStringLiteral("materialConfirmInheritance"));
    REQUIRE(confirmationOrNull != nullptr);
    CHECK(confirmationOrNull->defaultButton() == confirmationOrNull->button(QMessageBox::Cancel));
    QPushButton* removeOrNull = confirmationOrNull->findChild<QPushButton*>(QStringLiteral("materialRemoveOverride"));
    REQUIRE(removeOrNull != nullptr);
    removeOrNull->click();
    WaitForValidatedScene(reopened);
    CHECK(getMaterial(reopened, selected).Scattering.AlphaU == original.AlphaU);
    click(*reopenedView, undo);
    WaitForValidatedScene(reopened);
    checkEdited(reopened);
    REQUIRE(reopenedView->close() == true);
    reopened.StopPreview();
}

TEST_CASE("Placement-only World edits derive the presented scene and keep everything else assembled",
          "[editor][world][placement]")
{
    using namespace ludus::foundation;
    WorldDocument document = Document();
    QString error;
    REQUIRE(document.ImportScene(ImportedLevel(), error) == true);
    const QString id = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9700");
    const QJsonObject unplaced = document.MakeDraftWithoutScenePlacements();
    QJsonObject placement = document.ScenePlacement(document.Entity(id));
    placement.insert(QStringLiteral("position"), QJsonArray{5.25, -1, 7});
    placement.insert(QStringLiteral("rotation"), QJsonArray{10, 20, 30});
    placement.insert(QStringLiteral("scale"), QJsonArray{2, 1.5, 0.5});
    // Bound worlds keep scene placement in the scene_transform component.
    QJsonObject components = document.Entity(id).value(QStringLiteral("components")).toObject();
    QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
    const QStringList names{QStringLiteral("x"),
                            QStringLiteral("y"),
                            QStringLiteral("z"),
                            QStringLiteral("rx"),
                            QStringLiteral("ry"),
                            QStringLiteral("rz"),
                            QStringLiteral("sx"),
                            QStringLiteral("sy"),
                            QStringLiteral("sz")};
    const QStringList keys{QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")};
    for (qsizetype i = 0; i < names.size(); ++i)
    {
        transform.insert(names[i], placement.value(keys[i / 3]).toArray()[i % 3]);
    }
    components.insert(QStringLiteral("transform"), transform);
    const QJsonObject before = document.SceneLevel();
    REQUIRE(document.Edit(id, components, document.Entity(id).value(QStringLiteral("name")).toString(), error) == true);
    CHECK(document.MakeDraftWithoutScenePlacements() == unplaced);
    const QJsonObject level = document.SceneLevel();
    REQUIRE(level != before);

    // The gizmo's composition of a root instance is the cook's object transform, bit for bit.
    const QByteArray bytes = QJsonDocument(level).toJson(QJsonDocument::Compact);
    Array<float32> cooked;
    ludus::content::scene_cook::AssetError cookError;
    REQUIRE(ludus::content::scene_cook::ComposeLevelTransforms(
                {reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())},
                cooked,
                cookError) == true);
    const math::Affine3 meshToWorld =
        WorldDocument::MakeSceneMeshToParent(document.ScenePlacement(document.Entity(id)));
    for (usize column = 0; column < 3; ++column)
    {
        CHECK(meshToWorld.Columns[column].X == cooked[column * 3]);
        CHECK(meshToWorld.Columns[column].Y == cooked[column * 3 + 1]);
        CHECK(meshToWorld.Columns[column].Z == cooked[column * 3 + 2]);
    }
    CHECK(meshToWorld.Translation.X == cooked[9]);
    CHECK(meshToWorld.Translation.Y == cooked[10]);
    CHECK(meshToWorld.Translation.Z == cooked[11]);

    // Renaming is not a placement edit.
    REQUIRE(document.Edit(id, components, QStringLiteral("Renamed"), error) == true);
    CHECK(document.MakeDraftWithoutScenePlacements() != unplaced);
}

TEST_CASE("Viewport placement commits present the cooked transform at once and validate in the background",
          "[editor][world][object-interaction][placement]")
{
    QTemporaryDir directory;
    WorldWorkspace workspace;
    const QString id = OpenPlacementWorld(directory, workspace);
    WaitForValidatedScene(workspace);
    LevelWorkspace& scene = GetLevel(workspace);
    ViewportObjectInteraction& interaction = scene.GetViewport().GetObjectInteraction();
    interaction.SelectionRequested(id);
    const QByteArray validatedIdentity = scene.PreviewScene().Identity;
    const ludus::foundation::float32 originalX = GetPresentedX(workspace, id);

    interaction.PlacementCommitted(id, MovePlacement(workspace, id, 2));
    // No blocking cook: the moved scene is already presented and the gizmo stays usable.
    CHECK(scene.Busy() == false);
    CHECK(scene.GetPlacementValidation() == PlacementValidation::Running);
    CHECK(GetPresentedX(workspace, id) == originalX + 2);
    CHECK(workspace.CanUndo() == true);
    CHECK(workspace.CanSave() == true);
    const QByteArray placedIdentity = scene.PreviewScene().Identity;
    CHECK(placedIdentity != validatedIdentity);
    const ludus::foundation::math::Affine3 meshToWorld =
        WorldDocument::MakeSceneMeshToParent(workspace.Document().ScenePlacement(workspace.Document().Entity(id)));
    CHECK(GetPresentedObject(workspace, id).Transform.Translation == meshToWorld.Translation);
    CHECK(GetPresentedObject(workspace, id).Transform.Columns[0] == meshToWorld.Columns[0]);

    WaitForValidatedScene(workspace);
    // The cook wrote the same transforms, so the derived scene stays presented without a renderer restart.
    CHECK(scene.PreviewScene().Identity == placedIdentity);
    QLabel* statusOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldStatus"));
    REQUIRE(statusOrNull != nullptr);
    CHECK(statusOrNull->text().contains(QStringLiteral("validated")) == true);
}

TEST_CASE("An imported World publishes probe moves without assembling its scene",
          "[editor][world][object-interaction][probe-gi]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString source = QDir(root).filePath(QStringLiteral("external"));
    const QString levelPath = QStringLiteral("scenes/room.lulevel");
    QString error;
    REQUIRE(tests::CreateFixtureSource(source) == true);
    REQUIRE(tests::PrepareImportedLevel(root, levelPath, source, error) == true);
    QByteArray schema;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const QJsonObject level = QJsonDocument::fromJson(Read(QDir(root).filePath(levelPath))).object();
    REQUIRE(document.ImportScene(level, error) == true);
    const QString mesh =
        level.value(QStringLiteral("objects")).toArray().first().toObject().value(QStringLiteral("id")).toString();
    QString volume;
    REQUIRE(CreateWorldObject(document,
                              ludus::platform::GetSystemEntropySource(),
                              QStringLiteral("GI Probe Volume"),
                              volume,
                              error) == true);
    const QString path = QDir(root).filePath(QStringLiteral("probe.world.json"));
    Write(path, document.Encode());

    WorldWorkspace workspace;
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(path) == true);
    WaitForValidatedScene(workspace);
    LevelWorkspace& scene = GetLevel(workspace);
    ViewportObjectInteraction& interaction = scene.GetViewport().GetObjectInteraction();
    REQUIRE(workspace.SelectEntity(volume) == true);
    const QByteArray sceneIdentity = scene.PreviewScene().Identity;
    const auto moveProbe = [&interaction, &volume](ludus::foundation::float64 xMetres) {
        interaction.PlacementCommitted(volume,
                                       QJsonObject{{QStringLiteral("position"), QJsonArray{xMetres, 0, 0}},
                                                   {QStringLiteral("scale"),
                                                    QJsonArray{DEFAULT_GI_PROBE_VOLUME_SIZE_METRES,
                                                               DEFAULT_GI_PROBE_VOLUME_SIZE_METRES,
                                                               DEFAULT_GI_PROBE_VOLUME_SIZE_METRES}}});
    };
    moveProbe(1);
    CHECK(scene.CanStartOperation() == true);
    CHECK(scene.PreviewScene().ProbeGrid.Counts[0] == 0);
    CHECK(scene.PreviewScene().Identity == sceneIdentity);
    CHECK(ReadWorldGiProbeOrigin(workspace.Document().Entity(volume).value(QStringLiteral("components")).toObject())[0]
              .toDouble() == 1);

    QPushButton* bakeOrNull = workspace.findChild<QPushButton*>(QStringLiteral("worldBakeGi"));
    REQUIRE(bakeOrNull != nullptr);
    bakeOrNull->click();
    REQUIRE(scene.CanStartOperation() == true);
    REQUIRE(scene.PreviewScene().ProbeGrid.Counts[0] != 0);
    CHECK(scene.PreviewScene().ProbeGrid.OriginWorld.X == 1);

    moveProbe(2);
    REQUIRE(workspace.CanUndo() == true);
    CHECK(scene.CanStartOperation() == true);
    CHECK(scene.PreviewScene().ProbeGrid.OriginWorld.X == 2);
    workspace.Undo();
    CHECK(scene.PreviewScene().ProbeGrid.OriginWorld.X == 1);
    CHECK(scene.PreviewScene().Identity == sceneIdentity);

    REQUIRE(workspace.SelectEntity(mesh) == true);
    interaction.PlacementCommitted(mesh, MovePlacement(workspace, mesh, 1));
    REQUIRE(scene.GetPlacementValidation() == PlacementValidation::Running);
    REQUIRE(workspace.SelectEntity(volume) == true);
    moveProbe(2);
    CHECK(scene.Busy() == false);
    CHECK(scene.GetPlacementValidation() == PlacementValidation::Running);
    CHECK(scene.PreviewScene().ProbeGrid.OriginWorld.X == 2);
    WaitForValidatedScene(workspace);
    CHECK(scene.PreviewScene().ProbeGrid.OriginWorld.X == 2);
    workspace.Undo();
    CHECK(scene.CanStartOperation() == true);
    CHECK(scene.PreviewScene().ProbeGrid.OriginWorld.X == 1);
}

TEST_CASE("Placement edits, Undo and Redo continue while the background validation runs",
          "[editor][world][object-interaction][placement]")
{
    QTemporaryDir directory;
    WorldWorkspace workspace;
    const QString id = OpenPlacementWorld(directory, workspace);
    WaitForValidatedScene(workspace);
    LevelWorkspace& scene = GetLevel(workspace);
    ViewportObjectInteraction& interaction = scene.GetViewport().GetObjectInteraction();
    interaction.SelectionRequested(id);
    const QByteArray validatedIdentity = scene.PreviewScene().Identity;
    const ludus::foundation::float32 originalX = GetPresentedX(workspace, id);

    // Without an event loop turn the first cook cannot finish: each edit below happens while it runs.
    interaction.PlacementCommitted(id, MovePlacement(workspace, id, 2));
    interaction.PlacementCommitted(id, MovePlacement(workspace, id, 3));
    CHECK(GetPresentedX(workspace, id) == originalX + 5);
    // The running cook validates an older placement; the newest waits for it.
    CHECK(scene.GetPlacementValidation() == PlacementValidation::Pending);
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(GetPresentedX(workspace, id) == originalX + 2);
    workspace.Undo();
    // Back at the validated placement, its own cooked scene returns with nothing left to validate.
    CHECK(GetPresentedX(workspace, id) == originalX);
    CHECK(scene.PreviewScene().Identity == validatedIdentity);
    CHECK(scene.GetPlacementValidation() == PlacementValidation::Ready);
    REQUIRE(workspace.CanRedo() == true);
    workspace.Redo();
    CHECK(GetPresentedX(workspace, id) == originalX + 2);
    CHECK(scene.GetPlacementValidation() != PlacementValidation::Ready);

    WaitForValidatedScene(workspace);
    CHECK(GetPresentedX(workspace, id) == originalX + 2);
    CHECK(workspace.Document()
              .ScenePlacement(workspace.Document().Entity(id))
              .value(QStringLiteral("position"))
              .toArray()[0]
              .toDouble() == static_cast<ludus::foundation::float64>(originalX) + 2);
    workspace.Redo();
    CHECK(GetPresentedX(workspace, id) == originalX + 5);
    WaitForValidatedScene(workspace);
    CHECK(GetPresentedX(workspace, id) == originalX + 5);
}

TEST_CASE("Saving during a background placement validation writes the validated latest World",
          "[editor][world][object-interaction][placement]")
{
    QTemporaryDir directory;
    WorldWorkspace workspace;
    const QString id = OpenPlacementWorld(directory, workspace);
    WaitForValidatedScene(workspace);
    LevelWorkspace& scene = GetLevel(workspace);
    ViewportObjectInteraction& interaction = scene.GetViewport().GetObjectInteraction();
    interaction.SelectionRequested(id);
    const ludus::foundation::float32 originalX = GetPresentedX(workspace, id);

    interaction.PlacementCommitted(id, MovePlacement(workspace, id, 2));
    REQUIRE(scene.GetPlacementValidation() == PlacementValidation::Running);
    // Save waits for the validation, then verifies and writes whatever the World holds by then.
    REQUIRE(workspace.Save() == true);
    CHECK(workspace.Dirty() == true);
    interaction.PlacementCommitted(id, MovePlacement(workspace, id, 1));
    QElapsedTimer timer;
    timer.start();
    while ((workspace.Dirty() == true || workspace.CanSave() == false) && timer.elapsed() < MAX_SCENE_WAIT_MILLISECONDS)
    {
        QTest::qWait(10);
    }
    REQUIRE(workspace.Dirty() == false);
    CHECK(Read(workspace.Path()) == workspace.Document().Encode());
    WorldDocument reopened = Document();
    QString error;
    REQUIRE(reopened.Load(Read(workspace.Path()), error) == true);
    CHECK(reopened.ScenePlacement(reopened.Entity(id)).value(QStringLiteral("position")).toArray()[0].toDouble() ==
          static_cast<ludus::foundation::float64>(originalX) + 3);
    WaitForValidatedScene(workspace);
    CHECK(GetPresentedX(workspace, id) == originalX + 3);
}

TEST_CASE("A non-placement edit during a placement validation waits for a full assembly",
          "[editor][world][object-interaction][placement]")
{
    QTemporaryDir directory;
    WorldWorkspace workspace;
    const QString id = OpenPlacementWorld(directory, workspace);
    WaitForValidatedScene(workspace);
    LevelWorkspace& scene = GetLevel(workspace);
    ViewportObjectInteraction& interaction = scene.GetViewport().GetObjectInteraction();
    interaction.SelectionRequested(id);
    const ludus::foundation::float32 originalX = GetPresentedX(workspace, id);
    interaction.PlacementCommitted(id, MovePlacement(workspace, id, 2));
    const QByteArray placedIdentity = scene.PreviewScene().Identity;
    const qsizetype entityCount = workspace.Document().Draft().value(QStringLiteral("entities")).toArray().size();

    QAction* addOrNull = workspace.findChild<QAction*>(QStringLiteral("worldAdd"));
    REQUIRE(addOrNull != nullptr);
    REQUIRE(addOrNull->isEnabled() == true);
    addOrNull->trigger();
    CHECK(workspace.Document().Draft().value(QStringLiteral("entities")).toArray().size() == entityCount + 1);
    // The derived scene stays presented; World edits wait for the assembly that replaces it.
    CHECK(scene.PreviewScene().Identity == placedIdentity);
    CHECK(workspace.CanUndo() == false);
    CHECK(workspace.CanSave() == false);

    WaitForValidatedScene(workspace);
    CHECK(GetPresentedX(workspace, id) == originalX + 2);
    CHECK(workspace.CanUndo() == true);
}

TEST_CASE("A failed placement validation presents the last validated scene again",
          "[editor][world][object-interaction][placement]")
{
    QTemporaryDir directory;
    WorldWorkspace workspace;
    const QString id = OpenPlacementWorld(directory, workspace);
    WaitForValidatedScene(workspace);
    LevelWorkspace& scene = GetLevel(workspace);
    ViewportObjectInteraction& interaction = scene.GetViewport().GetObjectInteraction();
    interaction.SelectionRequested(id);
    const QByteArray validatedIdentity = scene.PreviewScene().Identity;
    const ludus::foundation::float32 originalX = GetPresentedX(workspace, id);
    QList<PlacementValidationResult> results;
    QObject::connect(&scene,
                     &LevelWorkspace::PlacementValidationFinished,
                     &scene,
                     [&results](PlacementValidationResult result) { results.append(result); });
    // Deriving reads no project files; the authoritative cook does, and now fails.
    REQUIRE(QFile::remove(directory.filePath(QStringLiteral("assets/catalog.json"))) == true);

    interaction.PlacementCommitted(id, MovePlacement(workspace, id, 2));
    CHECK(GetPresentedX(workspace, id) == originalX + 2);
    WaitForValidatedScene(workspace);
    REQUIRE(results.size() == 1);
    CHECK(results[0] == PlacementValidationResult::Rejected);
    CHECK(GetPresentedX(workspace, id) == originalX);
    CHECK(scene.PreviewScene().Identity == validatedIdentity);
    // The World keeps the edit, as after any failed assembly, and Undo still works.
    CHECK(workspace.Dirty() == true);
    REQUIRE(workspace.CanUndo() == true);
    workspace.Undo();
    CHECK(GetPresentedX(workspace, id) == originalX);
}

TEST_CASE("World entity search keeps group commands scoped to visible matches", "[editor][world][search]")
{
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    QLineEdit* search = workspace.findChild<QLineEdit*>(QStringLiteral("worldEntitySearch"));
    QLabel* count = workspace.findChild<QLabel*>(QStringLiteral("worldEntityCount"));
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(search != nullptr);
    REQUIRE(count != nullptr);
    REQUIRE(entitiesOrNull->topLevelItemCount() > 1);
    const QByteArray before = workspace.Document().Encode();
    QTreeWidgetItem* firstOrNull = entitiesOrNull->topLevelItem(0);
    REQUIRE(firstOrNull != nullptr);
    const QString identity = firstOrNull->data(0, Qt::UserRole).toString();
    WorldEntitySearch* controls = workspace.findChild<WorldEntitySearch*>();
    REQUIRE(controls != nullptr);
    QSignalSpy selectionChanges(controls, &WorldEntitySearch::SelectionChanged);
    entitiesOrNull->selectAll();
    search->setText(QStringLiteral("  %1  ").arg(identity.toUpper()));
    CHECK(selectionChanges.count() == 1);
    CHECK(firstOrNull->isHidden() == false);
    CHECK(workspace.Document().Encode() == before);
    CHECK(count->text() == QStringLiteral("1 of %1").arg(entitiesOrNull->topLevelItemCount()));
    CHECK(count->accessibleName() == QStringLiteral("1 of %1 entities match").arg(entitiesOrNull->topLevelItemCount()));
    REQUIRE(entitiesOrNull->selectedItems().size() == 1);
    CHECK(entitiesOrNull->selectedItems().front() == firstOrNull);
    entitiesOrNull->selectAll();
    CHECK(entitiesOrNull->selectedItems().size() == 1);
    workspace.findChild<QAction*>(QStringLiteral("worldDuplicate"))->trigger();
    CHECK(entitiesOrNull->topLevelItemCount() ==
          QJsonDocument::fromJson(before).object().value(QStringLiteral("entities")).toArray().size() + 1);
    // A document rebuild reapplies the query. The new stable ID does not match it.
    CHECK(count->text() == QStringLiteral("1 of %1").arg(entitiesOrNull->topLevelItemCount()));
    CHECK(count->accessibleName() == QStringLiteral("1 of %1 entities match").arg(entitiesOrNull->topLevelItemCount()));
    search->setText(QStringLiteral("no matching entity"));
    CHECK(entitiesOrNull->selectedItems().isEmpty() == true);
    CHECK(workspace.findChild<QAction*>(QStringLiteral("worldDuplicate"))->isEnabled() == false);
    CHECK(workspace.findChild<QAction*>(QStringLiteral("worldDelete"))->isEnabled() == false);
    CHECK(count->toolTip().contains(QStringLiteral("Clear or change")) == true);
    const QByteArray filtered = workspace.Document().Encode();
    REQUIRE(workspace.SelectEntity(identity) == true);
    CHECK(search->text().isEmpty() == true);
    CHECK(entitiesOrNull->selectedItems().size() == 1);
    CHECK(workspace.Document().Encode() == filtered);
    for (ludus::foundation::int32 index = 0; index < entitiesOrNull->topLevelItemCount(); ++index)
    {
        CHECK(entitiesOrNull->topLevelItem(index)->isHidden() == false);
    }
    entitiesOrNull->selectAll();
    CHECK(entitiesOrNull->selectedItems().size() == entitiesOrNull->topLevelItemCount());
    search->setText(entitiesOrNull->topLevelItem(0)->text(0).toUpper());
    CHECK(entitiesOrNull->topLevelItem(0)->isHidden() == false);
    entitiesOrNull->clearSelection();
    entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(0), 0, QItemSelectionModel::ClearAndSelect);
    QLineEdit* name = workspace.findChild<QLineEdit*>(QStringLiteral("worldEntityName"));
    REQUIRE(name != nullptr);
    Type(name, QStringLiteral("Pending rename"));
    CHECK(search->isEnabled() == false);
    workspace.findChild<QPushButton*>(QStringLiteral("worldReset"))->click();
    CHECK(search->isEnabled() == true);
}

TEST_CASE("World hierarchy shows imported descendants and reveals them for selection", "[editor][world][hierarchy]")
{
    QTemporaryDir directory;
    const QString path = Fixture(directory);
    const QString parentKey = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9701");
    const QString childKey = QStringLiteral("b8517f65-8127-4bf4-99c2-e53b1a6d9702");
    QJsonObject level = ImportedLevel();
    QJsonObject grandchild = level.value(QStringLiteral("objects")).toArray().first().toObject();
    const QString grandchildKey = grandchild.value(QStringLiteral("id")).toString();
    QJsonObject parent = grandchild;
    parent.insert(QStringLiteral("id"), parentKey);
    parent.insert(QStringLiteral("name"), QStringLiteral("Parent"));
    QJsonObject child = grandchild;
    child.insert(QStringLiteral("id"), childKey);
    child.insert(QStringLiteral("name"), QStringLiteral("Child"));
    child.insert(QStringLiteral("parent"), parentKey);
    grandchild.insert(QStringLiteral("name"), QStringLiteral("Grandchild"));
    grandchild.insert(QStringLiteral("parent"), childKey);
    level.insert(QStringLiteral("objects"), QJsonArray{grandchild, parent, child});

    WorldDocument document = Document();
    QString error;
    REQUIRE(document.ImportScene(level, error) == true);
    const QByteArray before = document.Encode();
    Write(path, before);

    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path) == true);
    QTreeWidget* treeOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    QLineEdit* searchOrNull = workspace.findChild<QLineEdit*>(QStringLiteral("worldEntitySearch"));
    const QLabel* countOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldEntityCount"));
    REQUIRE(treeOrNull != nullptr);
    REQUIRE(searchOrNull != nullptr);
    REQUIRE(countOrNull != nullptr);
    REQUIRE(treeOrNull->topLevelItemCount() == document.Draft().value(QStringLiteral("entities")).toArray().size() - 2);

    QTreeWidgetItem* parentRowOrNull = treeOrNull->topLevelItem(treeOrNull->topLevelItemCount() - 1);
    REQUIRE(parentRowOrNull != nullptr);
    CHECK(parentRowOrNull->data(0, Qt::UserRole).toString() == parentKey);
    CHECK(parentRowOrNull->isExpanded() == true);
    REQUIRE(parentRowOrNull->childCount() == 1);
    QTreeWidgetItem* childRowOrNull = parentRowOrNull->child(0);
    REQUIRE(childRowOrNull != nullptr);
    CHECK(childRowOrNull->data(0, Qt::UserRole).toString() == childKey);
    REQUIRE(childRowOrNull->childCount() == 1);
    QTreeWidgetItem* grandchildRowOrNull = childRowOrNull->child(0);
    REQUIRE(grandchildRowOrNull != nullptr);
    CHECK(grandchildRowOrNull->data(0, Qt::UserRole).toString() == grandchildKey);
    CHECK(countOrNull->text() == QString::number(treeOrNull->topLevelItemCount() + 2));
    CHECK(countOrNull->accessibleName() == QStringLiteral("%1 entities").arg(treeOrNull->topLevelItemCount() + 2));

    treeOrNull->setCurrentItem(grandchildRowOrNull);
    REQUIRE(grandchildRowOrNull->isSelected() == true);
    parentRowOrNull->setExpanded(false);
    CHECK(grandchildRowOrNull->isSelected() == false);
    searchOrNull->setText(grandchildKey);
    CHECK(parentRowOrNull->isExpanded() == true);
    CHECK(childRowOrNull->isExpanded() == true);
    CHECK(parentRowOrNull->isHidden() == false);
    CHECK(childRowOrNull->isHidden() == false);
    CHECK(countOrNull->text() == QStringLiteral("1 of %1").arg(treeOrNull->topLevelItemCount() + 2));
    treeOrNull->selectAll();
    REQUIRE(treeOrNull->selectedItems().size() == 1);
    CHECK(treeOrNull->selectedItems().front() == grandchildRowOrNull);

    searchOrNull->setText(QStringLiteral("no matching entity"));
    parentRowOrNull->setExpanded(false);
    REQUIRE(workspace.SelectEntity(grandchildKey) == true);
    CHECK(searchOrNull->text().isEmpty() == true);
    const QTreeWidgetItem* currentOrNull = treeOrNull->currentItem();
    REQUIRE(currentOrNull != nullptr);
    CHECK(currentOrNull->data(0, Qt::UserRole).toString() == grandchildKey);
    CHECK(currentOrNull->isSelected() == true);
    CHECK(currentOrNull->parent()->isExpanded() == true);
    CHECK(currentOrNull->parent()->parent()->isExpanded() == true);
    CHECK(workspace.Document().Encode() == before);
}

TEST_CASE("Atmosphere entities retain their Sun across deletion, undo and reopen", "[editor][world][atmosphere]")
{
    using namespace ludus::foundation;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    QString atmosphereId;
    REQUIRE(CreateWorldObject(document, entropy, QStringLiteral("Atmosphere"), atmosphereId, error) == true);
    const QJsonObject created = document.Draft();
    QString secondId;
    CHECK(CreateWorldObject(document, entropy, QStringLiteral("Atmosphere"), secondId, error) == false);
    QStringList copies;
    CHECK(document.DuplicateEntities(entropy, {atmosphereId}, copies, error) == false);
    CHECK(document.Draft() == created);
    CHECK(error.contains(QStringLiteral("one Atmosphere")) == true);
    RenderScene scene;
    REQUIRE(RenderScene::Load(directory.path(), document, scene, error) == true);
    CHECK(scene.Atmosphere.IsEnabled == false);
    CHECK(scene.AtmosphereStatus.contains(QStringLiteral("directional light")) == true);
    CHECK(scene.ShouldUseEmptyWorldView() == false);

    QString sunId;
    const WorldSunPlacement placement
    {
        .RotationDegrees = {-90, 0, 0},
        .IlluminanceLux = 10,
    };
    REQUIRE(CreateWorldSun(document, entropy, placement, sunId, error) == true);
    REQUIRE(RenderScene::Load(directory.path(), document, scene, error) == true);
    const QByteArray assetsIdentity = scene.AssetsIdentity;
    const QString atmosphereComponent = GetWorldAtmosphereComponentId();
    const QJsonObject atmosphere = document.Entity(atmosphereId);
    QJsonObject components = atmosphere.value(QStringLiteral("components")).toObject();
    QJsonObject values = components.value(atmosphereComponent).toObject();
    values.insert(QStringLiteral("sun"), sunId);
    // An A1 world omits the A2 fields and keeps their defaults after loading.
    values.remove(QStringLiteral("stars"));
    values.remove(QStringLiteral("moon"));
    components.insert(atmosphereComponent, values);
    REQUIRE(document.Edit(atmosphereId, components, atmosphere.value(QStringLiteral("name")).toString(), error) ==
            true);
    REQUIRE(RenderScene::Load(directory.path(), document, scene, error) == true);
    CHECK(scene.Atmosphere.IsEnabled == true);
    CHECK(scene.Atmosphere.DirectionToSunWorld.Y == Catch::Approx(1));
    CHECK(scene.Atmosphere.SolarIlluminance.X == Catch::Approx(10));
    CHECK(scene.ShouldUseEmptyWorldView() == false);
    CHECK(scene.AssetsIdentity == assetsIdentity);

    REQUIRE(document.RemoveEntities({sunId}, error) == true);
    REQUIRE(RenderScene::Load(directory.path(), document, scene, error) == true);
    CHECK(scene.Atmosphere.IsEnabled == false);
    CHECK(scene.AtmosphereStatus.isEmpty() == false);
    REQUIRE(document.Undo() == true);
    WorldDocument reopened;
    REQUIRE(reopened.LoadSchema(schema, error) == true);
    REQUIRE(reopened.Load(document.Encode(), error) == true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.IsEnabled == true);
    CHECK(scene.Atmosphere.SunSourceId == ComputeEntitySourceId(sunId));
    CHECK(scene.Atmosphere.ShouldLightScene == true);
    CHECK(scene.Atmosphere.Celestial.IsEnabled == true);
    CHECK(scene.Atmosphere.Celestial.StarRadiance > 0);
    CHECK(scene.Atmosphere.Celestial.MilkyWayRadiance == 0);
    CHECK(scene.Atmosphere.Celestial.LunarAngularRadiusRadians == 0);

    QString moonId;
    REQUIRE(CreateWorldSun(reopened, entropy, placement, moonId, error) == true);
    values.insert(QStringLiteral("moon"), moonId);
    components.insert(atmosphereComponent, values);
    REQUIRE(reopened.Edit(atmosphereId, components, atmosphere.value(QStringLiteral("name")).toString(), error) ==
            true);
    const QJsonObject moon = reopened.Entity(moonId);
    QJsonObject moonComponents = moon.value(QStringLiteral("components")).toObject();
    const QString lightComponent = QStringLiteral("light");
    const QJsonObject staticMoon = moonComponents.value(lightComponent).toObject();
    QJsonObject earthMoon = staticMoon;
    earthMoon.insert(QString(WORLD_CELESTIAL_ROLE), QString(WORLD_CELESTIAL_DYNAMIC));
    earthMoon.insert(QString(WORLD_CELESTIAL_DRIVER), QStringLiteral("Earth Moon"));
    earthMoon.insert(QString(WORLD_CELESTIAL_TIME), 0.5);
    moonComponents.insert(lightComponent, earthMoon);
    const QString moonName = moon.value(QStringLiteral("name")).toString();
    REQUIRE(reopened.Edit(moonId, moonComponents, moonName, error) == true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.Celestial.MilkyWayRadiance > 0);
    CHECK(scene.Atmosphere.Celestial.Planets[0].Illuminance.X > 0);
    const math::Vector3 midnightCenterWorld = scene.Atmosphere.Celestial.GalacticCenterWorld;
    const QByteArray authoredWorld = reopened.Encode();
    RenderScene sampled = scene;
    REQUIRE(sampled.TrySampleEarthTime(0, error) == true);
    CHECK(sampled.Atmosphere.Celestial.GalacticCenterWorld != midnightCenterWorld);
    CHECK(reopened.Encode() == authoredWorld);
    CHECK(scene.Atmosphere.Celestial.GalacticCenterWorld == midnightCenterWorld);
    const float64 invalidTime = ludus::gameplay::world::MAX_EARTH_CELESTIAL_DAYS + 1;
    CHECK(sampled.TrySampleEarthTime(invalidTime, error) == false);

    earthMoon.insert(QString(WORLD_CELESTIAL_TIME), 0);
    moonComponents.insert(lightComponent, earthMoon);
    REQUIRE(reopened.Edit(moonId, moonComponents, moonName, error) == true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.Celestial.GalacticCenterWorld != midnightCenterWorld);
    CHECK(sampled.Atmosphere.Celestial.GalacticCenterWorld == scene.Atmosphere.Celestial.GalacticCenterWorld);
    CHECK(sampled.Atmosphere.Celestial.DirectionToMoonWorld == scene.Atmosphere.Celestial.DirectionToMoonWorld);
    CHECK(sampled.Atmosphere.Celestial.LunarPeakRadiance == scene.Atmosphere.Celestial.LunarPeakRadiance);
    values.insert(QStringLiteral("stars"), false);
    components.insert(atmosphereComponent, values);
    REQUIRE(reopened.Edit(atmosphereId, components, atmosphere.value(QStringLiteral("name")).toString(), error) ==
            true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.Celestial.MilkyWayRadiance == 0);
    CHECK(scene.Atmosphere.Celestial.Planets[0].Illuminance.X == 0);
    moonComponents.insert(lightComponent, staticMoon);
    REQUIRE(reopened.Edit(moonId, moonComponents, moonName, error) == true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.AtmosphereMoonSourceId == ComputeEntitySourceId(moonId));
    CHECK(scene.Atmosphere.Celestial.DirectionToMoonWorld.Y == Catch::Approx(1));
    CHECK(scene.Atmosphere.Celestial.LunarPeakRadiance.X > 0);
    CHECK(scene.Atmosphere.Celestial.StarRadiance == 0);
    REQUIRE(reopened.RemoveEntities({moonId}, error) == true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.IsEnabled == true);
    CHECK(scene.AtmosphereStatus.contains(QStringLiteral("Moon")) == true);
    CHECK(scene.Atmosphere.Celestial.LunarAngularRadiusRadians == 0);
    REQUIRE(reopened.Undo() == true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.Celestial.LunarAngularRadiusRadians > 0);
    values.insert(QStringLiteral("stars"), true);
    components.insert(atmosphereComponent, values);
    REQUIRE(reopened.Edit(atmosphereId, components, atmosphere.value(QStringLiteral("name")).toString(), error) ==
            true);
    // The angular radius a direct light carries for its shadow penumbra; -1 when the scene has no such light.
    const auto findLightRadius = [&scene](const QString& id) {
        const uint64 sourceId = ComputeEntitySourceId(id);
        for (uint32 i = 0; i < scene.AuthoredLights.Count; ++i)
        {
            if (scene.AuthoredLights.Lights[i].SourceId == sourceId)
            {
                return scene.AuthoredLights.Lights[i].AngularRadiusRadians;
            }
        }
        return -1.0F;
    };
    // A static Sun keeps the delta direction and hard shadows of existing directional lights.
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(findLightRadius(sunId) == 0);
    const auto setEarthPreset = [&reopened, &error](const QString& id, QLatin1StringView preset) {
        const QJsonObject entity = reopened.Entity(id);
        QJsonObject entityComponents = entity.value(QStringLiteral("components")).toObject();
        QJsonObject light = entityComponents.value(QStringLiteral("light")).toObject();
        light.insert(WORLD_CELESTIAL_ROLE, WORLD_CELESTIAL_DYNAMIC);
        light.insert(WORLD_CELESTIAL_DRIVER, preset);
        entityComponents.insert(QStringLiteral("light"), light);
        REQUIRE(reopened.Edit(id, entityComponents, entity.value(QStringLiteral("name")).toString(), error) == true);
    };
    setEarthPreset(sunId, WORLD_CELESTIAL_EARTH_SUN);
    setEarthPreset(moonId, WORLD_CELESTIAL_EARTH_MOON);
    const QJsonObject beforeEarthEdit = reopened.Draft();
    const QJsonObject sun = reopened.Entity(sunId);
    QJsonObject sunComponents = sun.value(QStringLiteral("components")).toObject();
    QJsonObject sunLight = sunComponents.value(QStringLiteral("light")).toObject();
    sunLight.insert(WORLD_CELESTIAL_TIME, 19.69);
    sunComponents.insert(QStringLiteral("light"), sunLight);
    REQUIRE(reopened.Edit(sunId, sunComponents, sun.value(QStringLiteral("name")).toString(), error) == true);
    const QJsonObject synchronizedMoonLight = reopened.Entity(moonId)
                                                  .value(QStringLiteral("components"))
                                                  .toObject()
                                                  .value(QStringLiteral("light"))
                                                  .toObject();
    CHECK(reopened.Draft().value(WORLD_EARTH_OBSERVER).toObject().value(WORLD_CELESTIAL_TIME).toDouble() == 19.69);
    CHECK(synchronizedMoonLight.contains(WORLD_CELESTIAL_TIME) == false);
    CHECK(MakeWorldLightValues(synchronizedMoonLight, reopened.Draft()).value(WORLD_CELESTIAL_TIME).toDouble() ==
          19.69);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.Celestial.MilkyWayRadiance > 0);
    CHECK(scene.Atmosphere.Celestial.Planets[0].Illuminance.X > 0);
    CHECK(scene.Atmosphere.SolarIlluminance.X > 100'000);
    CHECK(scene.Atmosphere.Celestial.LunarAngularRadiusRadians > 0.004F);
    CHECK(scene.Atmosphere.Celestial.DirectionToSunAtMoonWorld != math::Vector3{});
    // Direct shadows use the same disks as the sky.
    CHECK(findLightRadius(sunId) == scene.Atmosphere.SolarAngularRadiusRadians);
    CHECK(findLightRadius(moonId) == scene.Atmosphere.Celestial.LunarAngularRadiusRadians);
    REQUIRE(reopened.Undo() == true);
    CHECK(reopened.Draft() == beforeEarthEdit);
    REQUIRE(reopened.RemoveEntities({atmosphereId}, error) == true);
    REQUIRE(RenderScene::Load(directory.path(), reopened, scene, error) == true);
    CHECK(scene.Atmosphere.IsEnabled == false);
    CHECK(scene.AtmosphereStatus.isEmpty() == true);
    CHECK(scene.ShouldUseEmptyWorldView() == true);
}
