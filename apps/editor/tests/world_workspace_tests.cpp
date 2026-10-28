#include "entropy_fixture.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/editor_identity.h"
#include "internal/level_workspace.h"
#include "internal/main_window.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_object_interaction.h"
#include "internal/world_document.h"
#include "internal/world_workspace.h"
#include "scene_fixture.h"
#include <QColor>
#include <QImage>
#include <QPainter>

#include "internal/workspace_style.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRect>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <catch2/catch_test_macros.hpp>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/gameplay/camera/camera.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/platform/entropy.h>

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
    QPushButton* add = workspace.findChild<QPushButton*>(QStringLiteral("worldAdd"));
    REQUIRE(add != nullptr);
    REQUIRE(add->isEnabled());
    add->click();
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
    CHECK_FALSE(document.Restore(malformed, error));
    CHECK(document.Draft() == before);
}
TEST_CASE("World inspector preserves invalid text disables selection and saves one atomic edit", "[editor][world]")
{
    QTemporaryDir dir;
    const auto path = Fixture(dir);
    WorldWorkspace view;
    view.SetProject(dir.path(), 1);
    REQUIRE(view.Open(path));
    auto* list = view.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(list != nullptr);
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    auto* x = Field(view, QStringLiteral("transform/x"));
    auto* name = view.findChild<QLineEdit*>(QStringLiteral("worldEntityName"));
    REQUIRE(name != nullptr);
    const auto before = view.Document().Draft();
    Type(x, QStringLiteral("-"));
    Type(name, QStringLiteral("Artist cube"));
    CHECK(view.Dirty());
    CHECK_FALSE(list->isEnabled());
    CHECK_FALSE(view.CanUndo());
    CHECK_FALSE(view.Save());
    CHECK(view.Document().Draft() == before);
    CHECK(x->text() == QStringLiteral("-"));
    CHECK(name->text() == QStringLiteral("Artist cube"));
    Type(x, QStringLiteral("0.5"));
    REQUIRE(view.Save());
    CHECK_FALSE(view.Dirty());
    CHECK(list->item(0)->text() == QStringLiteral("Artist cube"));
    CHECK(list->isEnabled());
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
    auto* list = view.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
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
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    Type(Field(view, QStringLiteral("transform/x")), QStringLiteral("5"));
    REQUIRE(view.Apply());
    CHECK(view.findChild<QLabel*>(QStringLiteral("worldStatus"))
              ->text()
              .contains(QStringLiteral("Recovery could not be written")));
    lock.unlock();
    REQUIRE(view.Save());

    const auto alias = QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QStringLiteral("alias.world.json"));
    REQUIRE(QFile::link(path, alias));
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
    auto* list = view.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
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
    fresh.findChild<QPushButton*>(QStringLiteral("worldAdd"))->click();
    REQUIRE(fresh.Save());
    CHECK(QFile::exists(newPath));
    CHECK_FALSE(fresh.Dirty());
}
TEST_CASE("World schema and document resource limits are enforced", "[editor][world]")
{
    auto document = Document();
    QString error;
    QString id;
    for (ludus::foundation::uint32 i = 2; i < 128; ++i)
    {
        REQUIRE(document.CreateEntity(ludus::platform::GetSystemEntropySource(), error, QStringLiteral("Entity"), id));
    }
    const auto before = document.Draft();
    CHECK_FALSE(
        document.CreateEntity(ludus::platform::GetSystemEntropySource(), error, QStringLiteral("Overflow"), id));
    CHECK(document.Draft() == before);
    CHECK_FALSE(document.Load(QByteArray(524289, ' '), error));
    CHECK(document.Draft() == before);
    WorldDocument fresh;
    CHECK_FALSE(fresh.LoadSchema(QByteArray(65537, ' '), error));
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
    view.findChild<QListWidget*>(QStringLiteral("worldEntities"))
        ->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
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
    // Commands sit in rows instead of full-width stacks: the file commands beside the path,
    // the entity commands under the hierarchy, Apply beside Reset, Add Component beside its choice.
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
    CHECK(isSameRow("worldAdd", "worldDuplicate") == true);
    CHECK(isSameRow("worldAdd", "worldDelete") == true);
    CHECK(isSameRow("worldApply", "worldReset") == true);
    CHECK(isSameRow("worldAttach", "worldComponentChoice") == true);
    CHECK(isSameRow("worldPreview3D", "worldPreviewDetails") == true);
    QLabel* pathLabel = view.findChild<QLabel*>(QStringLiteral("worldPath"));
    REQUIRE(pathLabel != nullptr);
    CHECK(pathLabel->text() == QStringLiteral("authoring/starter.world.json"));
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
    auto* list = view.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
    auto* name = view.findChild<QLineEdit*>(QStringLiteral("worldEntityName"));
    Type(name, QString(120, QLatin1Char('T')));
    REQUIRE(view.Apply());
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
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
    view.findChild<QListWidget*>(QStringLiteral("worldEntities"))
        ->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
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
    world->findChild<QListWidget*>(QStringLiteral("worldEntities"))
        ->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    Type(Field(*world, QStringLiteral("transform/x")), QStringLiteral("4"));
    auto* save = window.findChild<QAction*>(QStringLiteral("document.save"));
    REQUIRE(save != nullptr);
    CHECK(save->text() == QStringLiteral("&Save World"));
    REQUIRE(save->isEnabled());
    save->trigger();
    CHECK_FALSE(world->Dirty());
    CHECK(Read(project) == SerializeDescriptor(descriptor));
    world->findChild<QListWidget*>(QStringLiteral("worldEntities"))->setFocus();
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
    source.findChild<QListWidget*>(QStringLiteral("worldEntities"))
        ->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
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
} // namespace
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
    REQUIRE(QFile::link(QDir(root).filePath(QStringLiteral("missing.schema.json")),
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
    const auto checkTitle = [&well, world, group](const QString& expected) {
        CHECK(well.GetDocumentTitle(world) == expected);
        const ludus::foundation::int32 index = group->indexOf(world);
        REQUIRE(index >= 0);
        CHECK(group->tabText(index) == expected);
        CHECK(group->tabToolTip(index) == expected);
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

    QPushButton* add = world->findChild<QPushButton*>(QStringLiteral("worldAdd"));
    REQUIRE(add != nullptr);
    REQUIRE(add->isEnabled());
    add->click();
    REQUIRE(world->Dirty());
    checkTitle(QStringLiteral("staircase2.world.json*"));
    world->Undo();
    REQUIRE(world->Dirty() == false);
    checkTitle(QStringLiteral("staircase2.world.json"));

    well.CloseDocument(world);
    REQUIRE(world->Open(firstPath));
    CHECK(well.GetDocumentTitle(world) == QStringLiteral("starter.world.json"));
    REQUIRE(well.OpenDocument(world));
    checkTitle(QStringLiteral("starter.world.json"));
    REQUIRE(world->Create(QFileInfo(firstPath).dir().filePath(QStringLiteral("new.world.json"))));
    checkTitle(QStringLiteral("new.world.json*"));
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
    auto* entities = workspace.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entities != nullptr);
    REQUIRE(entities->count() == 1);
    entities->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    CHECK(workspace.findChild<QLineEdit*>(QStringLiteral("worldField:scene/position0")) == nullptr);
    Type(Field(workspace, QStringLiteral("transform/x")), QStringLiteral("0.25"));
    REQUIRE(workspace.Apply());
    drain();
    auto* choices = workspace.findChild<QComboBox*>(QStringLiteral("worldComponentChoice"));
    REQUIRE(choices != nullptr);
    choices->setCurrentIndex(choices->findData(QStringLiteral("behavior")));
    workspace.findChild<QPushButton*>(QStringLiteral("worldAttach"))->click();
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
    CHECK(mixed.findChild<QListWidget*>(QStringLiteral("worldEntities"))->count() == 4);
    CHECK(mixed.findChild<QListWidget*>(QStringLiteral("worldEntities"))->isEnabled());
    auto* mixedEntities = mixed.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    mixedEntities->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
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
    QListWidget* list = view.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == 2);
    list->setFocus();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
    CHECK(list->selectedItems().size() == 1);
    QTest::mouseClick(list->viewport(),
                      Qt::LeftButton,
                      Qt::ControlModifier,
                      list->visualItemRect(list->item(1)).center());
    CHECK(list->selectedItems().size() == 2);
    QTest::mouseClick(list->viewport(),
                      Qt::LeftButton,
                      Qt::ControlModifier,
                      list->visualItemRect(list->item(0)).center());
    CHECK(list->selectedItems().size() == 1);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
    QTest::mouseClick(list->viewport(),
                      Qt::LeftButton,
                      Qt::ShiftModifier,
                      list->visualItemRect(list->item(1)).center());
    CHECK(list->selectedItems().size() == 2);
    QTest::keyClick(list, Qt::Key_Escape);
    CHECK(list->selectedItems().isEmpty() == true);
    QTest::keySequence(list, QKeySequence(QKeySequence::SelectAll));
    CHECK(list->selectedItems().size() == 2);
    CHECK(view.findChild<QLineEdit*>(QStringLiteral("worldEntityName")) == nullptr);
    const QJsonObject session = view.Session();
    QTest::keyClick(list, Qt::Key_Escape);
    view.RestoreSession(session);
    CHECK(list->selectedItems().size() == 2);
    list->setFocus();
    QApplication::processEvents();
    REQUIRE(list->hasFocus() == true);
    REQUIRE(view.findChild<QPushButton*>(QStringLiteral("worldDuplicate"))->isEnabled() == true);
    QTest::keySequence(list, QKeySequence(Qt::CTRL | Qt::Key_D));
    INFO(view.findChild<QLabel*>(QStringLiteral("worldStatus"))->text().toStdString());
    CHECK(list->count() == 4);
    CHECK(list->selectedItems().size() == 2);
    view.Undo();
    CHECK(list->count() == 2);
    view.Redo();
    CHECK(list->count() == 4);
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_F2);
    QLineEdit* name = view.findChild<QLineEdit*>(QStringLiteral("worldEntityName"));
    REQUIRE(name != nullptr);
    CHECK(name->hasFocus() == true);
    CHECK(name->selectedText() == name->text());
    // A focused text field keeps its own Select All; it cannot select entities.
    QTest::keySequence(name, QKeySequence(QKeySequence::SelectAll));
    CHECK(list->selectedItems().size() == 1);
    list->setFocus();
    QTest::keySequence(list, QKeySequence(QKeySequence::SelectAll));
    QTimer::singleShot(0, []() {
        QMessageBox* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(confirmation != nullptr);
        confirmation->button(QMessageBox::Yes)->click();
    });
    REQUIRE(view.findChild<QPushButton*>(QStringLiteral("worldDelete"))->isEnabled() == true);
    QTest::keySequence(list, QKeySequence(QKeySequence::Delete));
    INFO(view.findChild<QLabel*>(QStringLiteral("worldStatus"))->text().toStdString());
    CHECK(list->count() == 0);
    view.Undo();
    CHECK(list->count() == 4);
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
    REQUIRE(document.Restore(bounded, error) == true);
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
    PlayModePreference() : mSaved(QSettings().value(KEY))
    {
        QSettings().remove(KEY);
    }
    ~PlayModePreference()
    {
        if (mSaved.isValid() == true)
        {
            QSettings().setValue(KEY, mSaved);
            return;
        }
        QSettings().remove(KEY);
    }
    PlayModePreference(const PlayModePreference&) = delete;
    PlayModePreference& operator=(const PlayModePreference&) = delete;
    static inline const QString KEY = QStringLiteral("play/mode");

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
    CHECK(QSettings().value(PlayModePreference::KEY).toString() == QStringLiteral("window"));
    WorldWorkspace second;
    CHECK(second.GetPlayMode() == PlayMode::SeparateWindow);

    second.SetPlayMode(PlayMode::InViewport);
    CHECK(QSettings().value(PlayModePreference::KEY).toString() == QStringLiteral("viewport"));
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
TEST_CASE("Viewport transform drags preview a highlighted wireframe and commit one undoable placement",
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
    QListWidget* entitiesOrNull = workspace.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    REQUIRE(entitiesOrNull->currentItem() != nullptr);
    CHECK(entitiesOrNull->currentItem()->data(Qt::UserRole).toString() == id);
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
    interaction.SelectionRequested(QString());
    CHECK(entitiesOrNull->selectedItems().isEmpty() == true);
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
    // Exceed the sampled wire budget without changing the selectable triangle.
    constexpr usize DENSE_TRIANGLES = ViewportObjectOverlay::MAX_WIRE_TRIANGLES + 1;
    scene.Meshes[0].Indices.clear();
    for (usize triangle = 0; triangle < DENSE_TRIANGLES; ++triangle)
    {
        scene.Meshes[0].Indices.append({0, 1, 2});
    }
    RenderPreview preview;
    ConfigureInteractionPreview(preview, scene);
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, scene.Objects[0].Id, true);
    ViewportObjectOverlay overlay;
    const QRectF framePixel{0, 0, 800, 600};
    interaction.BuildOverlay(framePixel, overlay);
    REQUIRE(overlay.WireLinesPixel.IsEmpty() == false);
    CHECK(overlay.WireLinesPixel.GetSize() <= ViewportObjectOverlay::MAX_WIRE_LINES);
    REQUIRE(overlay.HandleStrokes.IsEmpty() == false);
    CHECK(overlay.HandleStrokes.GetSize() <= ViewportObjectOverlay::MAX_HANDLE_STROKES);
    const QLineF* wireStorage = overlay.WireLinesPixel.GetData();
    const ViewportHandleStroke* handleStorage = overlay.HandleStrokes.GetData();
    const QLineF originalPixel = overlay.WireLinesPixel[0];
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.WireLinesPixel.GetData() == wireStorage);
    CHECK(overlay.HandleStrokes.GetData() == handleStorage);
    CHECK(overlay.WireLinesPixel[0] == originalPixel);
    REQUIRE(interaction.Press(framePixel.center() + QPointF(70, 0), framePixel) == true);
    REQUIRE(interaction.Move(framePixel.center() + QPointF(110, 0), false) == true);
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.WireLinesPixel[0] != originalPixel);
    CHECK(overlay.WireLinesPixel.GetData() == wireStorage);
    interaction.Cancel();
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.WireLinesPixel[0] == originalPixel);
    interaction.SetImageSize({800, 800});
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.FramePixel == QRectF(100, 0, 600, 600));
    interaction.SetActive(false);
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.WireLinesPixel.IsEmpty() == true);
    CHECK(overlay.HandleStrokes.IsEmpty() == true);
    CHECK(overlay.FramePixel.isEmpty() == true);
    CHECK(overlay.WireLinesPixel.GetData() == wireStorage);
    interaction.SetActive(true);
    interaction.SetWorld(document, scene.Objects[0].Id, false);
    interaction.BuildOverlay(framePixel, overlay);
    CHECK(overlay.WireLinesPixel.IsEmpty() == false);
    CHECK(overlay.HandleStrokes.IsEmpty() == true);
}
