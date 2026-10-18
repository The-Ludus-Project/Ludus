#include "internal/browser_player.h"
#include "internal/controller.h"
#include "internal/main_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPixmap>
#include <QScrollArea>
#include <QSize>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

using namespace ludus::editor;

TEST_CASE("Browser players admit only shipped sample identifiers", "[editor][browser-player]")
{
    QTemporaryDir directory;
    const auto descriptor = directory.filePath(QStringLiteral("ludus.project.json"));
    CHECK(BrowserPlayer(descriptor).isEmpty());
    QFile metadata(directory.filePath(QStringLiteral("ludus.web.json")));
    for (const auto& id :
         {QStringLiteral("cornell-box"), QStringLiteral("live-edit-game"), QStringLiteral("scripted-game")})
    {
        REQUIRE(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metadata.write(
            QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("player"), id}}).toJson());
        metadata.close();
        CHECK(BrowserPlayer(descriptor) == id);
    }
    for (const auto& bytes : {QByteArrayLiteral("{\"version\":1,\"player\":\"../escape\"}"),
                              QByteArrayLiteral("{\"version\":2,\"player\":\"cornell-box\"}"),
                              QByteArrayLiteral("{\"version\":1,\"player\":\"https://example.test\"}"),
                              QByteArrayLiteral("invalid")})
    {
        REQUIRE(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metadata.write(bytes);
        metadata.close();
        CHECK(BrowserPlayer(descriptor).isEmpty());
    }
}

namespace
{
QDockWidget* Dock(MainWindow& window, const QString& name)
{
    auto* dock = window.findChild<QDockWidget*>(name);
    REQUIRE(dock != nullptr);
    return dock;
}

void WriteSetting(QFile& file, const QString& key, const QJsonValue& value)
{
    REQUIRE(file.open(QIODevice::ReadOnly));
    auto object = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    object.insert(key, value);
    REQUIRE(file.open(QIODevice::WriteOnly));
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
}

void Reset(MainWindow& window)
{
    auto* action = window.findChild<QAction*>(QStringLiteral("resetWorkspaceLayout"));
    REQUIRE(action != nullptr);
    action->trigger();
    QApplication::processEvents();
}

void CheckDefaultPanels(MainWindow& window)
{
    CHECK(window.findChild<QDockWidget*>(QStringLiteral("recentProjectsDock")) == nullptr);
    auto* inspector = Dock(window, QStringLiteral("liveInspectorDock"));
    auto* output = Dock(window, QStringLiteral("outputDock"));
    CHECK(window.dockWidgetArea(inspector) == Qt::RightDockWidgetArea);
    CHECK(window.dockWidgetArea(output) == Qt::BottomDockWidgetArea);
    for (auto* dock : {inspector, output})
    {
        CHECK_FALSE(dock->isHidden());
        CHECK_FALSE(dock->isFloating());
    }
    // Timing shares the inspector's area as a background tab.
    QDockWidget* timing = Dock(window, QStringLiteral("timingDock"));
    CHECK(window.dockWidgetArea(timing) == Qt::RightDockWidgetArea);
    CHECK(window.tabifiedDockWidgets(inspector).contains(timing));
    CHECK_FALSE(timing->isFloating());
}
} // namespace

TEST_CASE("Workspace panels recover without changing project state and share Play actions", "[editor][layout]")
{
    QTemporaryDir directory;
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    QFile descriptor(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(descriptor.open(QIODevice::WriteOnly));
    const auto bytes = QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json");
    REQUIRE(descriptor.write(bytes) == bytes.size());
    descriptor.close();
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("gameToolbar"));
    REQUIRE(toolbar != nullptr);
    for (const auto& name : {QStringLiteral("play.start"), QStringLiteral("play.reload")})
    {
        auto* action = window.findChild<QAction*>(name);
        REQUIRE(action != nullptr);
        CHECK(toolbar->actions().contains(action));
        CHECK_FALSE(action->isEnabled()); // Empty projects cannot launch work.
    }
    controller.OpenProject(descriptor.fileName());
    REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
    auto draft = controller.State().Draft;
    draft.Name = QStringLiteral("Unsaved name");
    controller.EditDraft(draft);
    REQUIRE(controller.State().Dirty());
    const auto epoch = controller.State().ProjectEpoch;
    auto* inspector = Dock(window, QStringLiteral("liveInspectorDock"));
    inspector->hide();
    inspector->toggleViewAction()->trigger();
    CHECK_FALSE(inspector->isHidden());
    inspector->setFloating(true);
    Dock(window, QStringLiteral("outputDock"))->hide();
    toolbar->hide();
    Reset(window);
    CheckDefaultPanels(window);
    CHECK_FALSE(toolbar->isHidden());
    CHECK(controller.State().ProjectEpoch == epoch);
    CHECK(controller.State().Document == DocumentState::ProjectLoaded);
    CHECK(controller.State().Dirty());
    CHECK(controller.State().Draft.Name == QStringLiteral("Unsaved name"));
    REQUIRE(descriptor.open(QIODevice::ReadOnly));
    CHECK(descriptor.readAll() == bytes);
}

TEST_CASE("Local workspace layout survives restart independently of project documents", "[editor][layout]")
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("preferences/workspace.json"));
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    const auto initial = controller.State();
    {
        MainWindow window(&controller, nullptr, path);
        window.show();
        QApplication::processEvents();
        window.addDockWidget(Qt::LeftDockWidgetArea, Dock(window, QStringLiteral("outputDock")));
        Dock(window, QStringLiteral("liveInspectorDock"))->hide();
        window.findChild<QToolBar*>()->hide();
        REQUIRE(window.close());
    }
    REQUIRE(QFileInfo::exists(path));
    MainWindow reopened(&controller, nullptr, path);
    reopened.show();
    QApplication::processEvents();
    CHECK(reopened.dockWidgetArea(Dock(reopened, QStringLiteral("outputDock"))) == Qt::LeftDockWidgetArea);
    CHECK(Dock(reopened, QStringLiteral("liveInspectorDock"))->isHidden());
    CHECK(reopened.findChild<QToolBar*>()->isHidden());
    Reset(reopened);
    CheckDefaultPanels(reopened);
    CHECK(controller.State().DescriptorPath == initial.DescriptorPath);
    CHECK(controller.State().Draft == initial.Draft);
    CHECK(controller.State().Saved == initial.Saved);
    CHECK(controller.State().Dirty() == initial.Dirty());
}

TEST_CASE("Invalid or incompatible layout preferences fall back to recoverable defaults", "[editor][layout]")
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("workspace.json"));
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    {
        MainWindow window(&controller, nullptr, path);
        window.show();
        Dock(window, QStringLiteral("liveInspectorDock"))->hide();
        REQUIRE(window.close());
    }
    QFile preferences(path);
    SECTION("Newer application layout")
    {
        WriteSetting(preferences, QStringLiteral("version"), 999);
    }
    SECTION("Different Qt version")
    {
        WriteSetting(preferences, QStringLiteral("qtVersion"), QStringLiteral("0.0.0"));
    }
    SECTION("Invalid state")
    {
        WriteSetting(preferences,
                     QStringLiteral("state"),
                     QString::fromLatin1(QByteArrayLiteral("corrupt").toBase64()));
    }
    SECTION("Invalid geometry")
    {
        WriteSetting(preferences,
                     QStringLiteral("geometry"),
                     QString::fromLatin1(QByteArrayLiteral("corrupt").toBase64()));
    }
    SECTION("Invalid base64")
    {
        WriteSetting(preferences, QStringLiteral("state"), QStringLiteral("!"));
    }
    SECTION("Oversized decoded state")
    {
        WriteSetting(preferences,
                     QStringLiteral("state"),
                     QString::fromLatin1(QByteArray(32 * 1024 + 1, 'x').toBase64()));
    }
    SECTION("Malformed JSON")
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write("{broken") == 7);
    }
    SECTION("Oversized file")
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(QByteArray(64 * 1024 + 1, 'x')) == 64 * 1024 + 1);
    }
    MainWindow window(&controller, nullptr, path);
    window.show();
    QApplication::processEvents();
    CheckDefaultPanels(window);
    CHECK_FALSE(window.findChild<QToolBar*>()->isHidden());
    REQUIRE(window.close()); // Recovery can replace the invalid preferences.
    CHECK(QFileInfo(path).size() < 65536);
}

TEST_CASE("Abandoning an unclosed window does not publish a new layout", "[editor][layout]")
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("workspace.json"));
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    {
        MainWindow window(&controller, nullptr, path);
        Dock(window, QStringLiteral("outputDock"))->hide();
    }
    CHECK_FALSE(QFileInfo::exists(path));
}

TEST_CASE("Authoring areas remain reachable at smaller workspace sizes", "[editor][layout]")
{
    QTemporaryDir directory;
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("workspaceTabs"));
    REQUIRE(tabs != nullptr);
    auto* project = window.findChild<QScrollArea*>(QStringLiteral("projectSettingsScroll"));
    auto* audio = window.findChild<QScrollArea*>(QStringLiteral("audioWorkspaceScroll"));
    REQUIRE(project != nullptr);
    REQUIRE(audio != nullptr);
    CHECK(tabs->indexOf(project) >= 0);
    CHECK(tabs->indexOf(audio) >= 0);
    QFile descriptor(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(descriptor.open(QIODevice::WriteOnly));
    descriptor.write(QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    descriptor.close();
    controller.OpenProject(descriptor.fileName());
    auto* inspector = window.findChild<QScrollArea*>(QStringLiteral("liveInspectorScroll"));
    REQUIRE(inspector != nullptr);
    // Optional captures are for human review, never a golden-image assertion.
    const auto captures = qEnvironmentVariable("LUDUS_EDITOR_LAYOUT_CAPTURE");
    for (const auto& size : {QSize(1100, 760), QSize(900, 640), QSize(800, 640)})
    {
        window.resize(size);
        QApplication::processEvents();
        CHECK(window.width() <= size.width());
        CHECK(window.height() <= size.height());
        CHECK(project->viewport()->width() > 100);
        CHECK(project->viewport()->height() > 100);
        CHECK(inspector->viewport()->width() > 100);
        CHECK(inspector->viewport()->height() > 100);
        if (captures.isEmpty() == false)
        {
            REQUIRE(window.grab().save(captures + QStringLiteral("-%1.png").arg(size.width())));
        }
    }
    tabs->setCurrentWidget(audio);
    QApplication::processEvents();
    CHECK(audio->isVisible());
    CHECK(audio->viewport()->width() > 100);
    CHECK(audio->viewport()->height() > 100);
    if (captures.isEmpty() == false)
    {
        REQUIRE(window.grab().save(captures + QStringLiteral("-audio.png")));
    }
}

TEST_CASE("Layout save failure does not prevent normal close or alter documents", "[editor][layout]")
{
    QTemporaryDir directory;
    const auto blocker = directory.filePath(QStringLiteral("not-a-directory"));
    QFile file(blocker);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.close();
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    const auto initial = controller.State();
    const auto preferences = blocker + QStringLiteral("/workspace.json");
    MainWindow window(&controller, nullptr, preferences);
    window.show();
    REQUIRE(window.close());
    CHECK_FALSE(QFileInfo::exists(preferences));
    CHECK(controller.State().DescriptorPath == initial.DescriptorPath);
    CHECK(controller.State().Draft == initial.Draft);
    CHECK(controller.State().Saved == initial.Saved);
}
