#include "internal/browser_player.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/error_list.h"
#include "internal/main_window.h"
#include "internal/world_workspace.h"

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
#include <QKeySequence>
#include <QPixmap>
#include <QScrollArea>
#include <QSize>
#include <QStatusBar>
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
/// MainWindow's short-window threshold for opening the bottom tool-window group by default.
constexpr int SHORT_WINDOW_HEIGHT_PIXELS = 1000;

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
    CHECK(window.findChild<QDockWidget*>(QStringLiteral("authoringToolsDock")) == nullptr);
    CHECK(window.centralWidget() == window.findChild<DocumentWell*>());
    auto* inspector = Dock(window, QStringLiteral("liveInspectorDock"));
    auto* output = Dock(window, QStringLiteral("outputDock"));
    QDockWidget* errors = Dock(window, QStringLiteral("errorListDock"));
    CHECK(window.dockWidgetArea(inspector) == Qt::RightDockWidgetArea);
    CHECK(window.dockWidgetArea(output) == Qt::BottomDockWidgetArea);
    // Short windows start with the bottom group closed; it is still arranged under the documents.
    const bool isBottomOpen = errors->isHidden() == false;
    CHECK(isBottomOpen == (window.height() >= SHORT_WINDOW_HEIGHT_PIXELS));
    for (QDockWidget* dock : {output, errors, Dock(window, QStringLiteral("timingDock"))})
    {
        CHECK(dock->isHidden() == (isBottomOpen == false));
        dock->show();
    }
    for (QDockWidget* dock : {inspector, output, errors, Dock(window, QStringLiteral("projectExplorerDock"))})
    {
        CHECK_FALSE(dock->isHidden());
        CHECK_FALSE(dock->isFloating());
    }
    // Runtime panels are background tabs: Live Inspector behind Properties, Timing and
    // Output behind the Error List.
    QDockWidget* timing = Dock(window, QStringLiteral("timingDock"));
    CHECK(window.dockWidgetArea(timing) == Qt::BottomDockWidgetArea);
    CHECK(window.tabifiedDockWidgets(errors).contains(timing));
    CHECK(window.tabifiedDockWidgets(errors).contains(output));
    CHECK(window.tabifiedDockWidgets(inspector).contains(Dock(window, QStringLiteral("propertiesDock"))));
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
    SECTION("Older single-dock layout")
    {
        WriteSetting(preferences, QStringLiteral("version"), 3);
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
    DocumentWell* well = window.findChild<DocumentWell*>();
    REQUIRE(well != nullptr);
    auto* project = window.findChild<QScrollArea*>(QStringLiteral("projectSettingsScroll"));
    auto* audio = window.findChild<QScrollArea*>(QStringLiteral("audioWorkspaceScroll"));
    REQUIRE(project != nullptr);
    REQUIRE(audio != nullptr);
    CHECK(well->GetDocuments().contains(project));
    CHECK(well->GetDocuments().contains(audio));
    QFile descriptor(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(descriptor.open(QIODevice::WriteOnly));
    descriptor.write(QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    descriptor.close();
    controller.OpenProject(descriptor.fileName());
    REQUIRE(well->OpenDocument(project) == true);
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
        Dock(window, QStringLiteral("liveInspectorDock"))->raise();
        QApplication::processEvents();
        CHECK(inspector->viewport()->width() > 100);
        CHECK(inspector->viewport()->height() > 100);
        if (captures.isEmpty() == false)
        {
            REQUIRE(window.grab().save(captures + QStringLiteral("-%1.png").arg(size.width())));
        }
    }
    REQUIRE(well->OpenDocument(audio) == true);
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

TEST_CASE("World stays visible beside tool windows and receives focused document actions", "[editor][layout]")
{
    QTemporaryDir directory;
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    QFile descriptor(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(descriptor.open(QIODevice::WriteOnly) == true);
    descriptor.write(QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    descriptor.close();
    window.show();
    window.activateWindow();
    controller.OpenProject(descriptor.fileName());
    QApplication::processEvents();
    WorldWorkspace* world = window.findChild<WorldWorkspace*>();
    DocumentWell* well = window.findChild<DocumentWell*>();
    QAction* save = window.findChild<QAction*>(QStringLiteral("document.save"));
    REQUIRE(world != nullptr);
    REQUIRE(well != nullptr);
    REQUIRE(save != nullptr);
    CHECK(well->GetActiveDocumentOrNull() == world);
    CHECK(world->isVisible() == true);
    CHECK(world->width() > Dock(window, QStringLiteral("projectExplorerDock"))->width());
    REQUIRE(world->Create(QFileInfo(directory.path()).canonicalFilePath() + QStringLiteral("/layout.world.json")) ==
            true);
    for (const QSize& size : {QSize(1400, 900), QSize(1100, 760), QSize(900, 640)})
    {
        window.resize(size);
        // Raising each tool window tab never hides the World document.
        for (QDockWidget* dock : window.findChildren<QDockWidget*>())
        {
            dock->raise();
            QApplication::processEvents();
            CHECK(world->isVisible() == true);
            CHECK(world->width() > 400);
            CHECK(world->height() > 300);
            CHECK(window.width() <= size.width());
            CHECK(window.height() <= size.height());
        }
    }
    REQUIRE(well->OpenDocument(window.findChild<QWidget*>(QStringLiteral("projectSettingsScroll"))) == true);
    CHECK(save->text() == QStringLiteral("&Save Project Settings"));
    REQUIRE(well->OpenDocument(world) == true);
    QApplication::processEvents();
    CHECK(save->text() == QStringLiteral("&Save World"));
    Dock(window, QStringLiteral("projectExplorerDock"))->hide();
    QApplication::processEvents();
    CHECK(world->isVisible() == true);
    Reset(window);
    CHECK(world->isVisible() == true);
}

TEST_CASE("Focus Viewport hides chrome around the World viewport and restores the arrangement exactly",
          "[editor][layout][focus-viewport]")
{
    QTemporaryDir directory;
    const QString preferences = directory.filePath(QStringLiteral("workspace.json"));
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    QFile descriptor(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(descriptor.open(QIODevice::WriteOnly) == true);
    descriptor.write(QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    descriptor.close();
    {
        MainWindow window(&controller, nullptr, preferences);
        window.resize(1470, 920);
        window.show();
        controller.OpenProject(descriptor.fileName());
        REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
        QApplication::processEvents();
        DocumentWell& well = window.GetDocumentWell();
        WorldWorkspace* world = window.findChild<WorldWorkspace*>();
        QWidget* project = window.findChild<QWidget*>(QStringLiteral("projectSettingsScroll"));
        REQUIRE(world != nullptr);
        REQUIRE(project != nullptr);
        REQUIRE(well.OpenDocument(project) == true);
        // A personal arrangement: one tool window moved, one closed, one toolbar hidden.
        window.addDockWidget(Qt::LeftDockWidgetArea, Dock(window, QStringLiteral("outputDock")));
        Dock(window, QStringLiteral("liveInspectorDock"))->hide();
        window.findChild<QToolBar*>(QStringLiteral("renderGraphToolbar"))->hide();
        QApplication::processEvents();

        QAction* focus = window.findChild<QAction*>(QStringLiteral("view.focusViewport"));
        REQUIRE(focus != nullptr);
        CHECK(focus->isCheckable() == true);
        CHECK(focus->shortcut() == QKeySequence(QStringLiteral("Shift+Alt+Return")));
        QWidget* viewport = world->findChild<QWidget*>(QStringLiteral("worldViewport"));
        REQUIRE(viewport != nullptr);
        QWidget* viewportParent = viewport->parentWidget();
        focus->trigger();
        QApplication::processEvents();
        CHECK(focus->isChecked() == true);
        CHECK(well.GetActiveDocumentOrNull() == world);
        for (QDockWidget* dock : window.findChildren<QDockWidget*>())
        {
            INFO(dock->objectName().toStdString());
            CHECK(dock->isHidden() == true);
        }
        for (QToolBar* toolbar : window.findChildren<QToolBar*>(Qt::FindDirectChildrenOnly))
        {
            INFO(toolbar->objectName().toStdString());
            CHECK(toolbar->isHidden() == true);
        }
        CHECK(world->IsChromeVisible() == false);
        CHECK(world->findChild<QWidget*>(QStringLiteral("worldHeader"))->isHidden() == true);
        CHECK(world->findChild<QWidget*>(QStringLiteral("worldHierarchyScroll"))->isHidden() == true);
        CHECK(world->findChild<QWidget*>(QStringLiteral("worldStatus"))->isHidden() == true);
        // The native viewport is neither hidden nor moved; it takes most of the window.
        CHECK(viewport->isHidden() == false);
        CHECK(viewport->parentWidget() == viewportParent);
        CHECK(viewport->height() * 100 >= window.height() * 75);
        CHECK(viewport->width() * 100 >= window.width() * 90);

        // Closing while focused persists the arrangement Focus Viewport returns to.
        REQUIRE(window.close() == true);
    }
    MainWindow window(&controller, nullptr, preferences);
    window.show();
    QApplication::processEvents();
    CHECK(window.dockWidgetArea(Dock(window, QStringLiteral("outputDock"))) == Qt::LeftDockWidgetArea);
    CHECK(Dock(window, QStringLiteral("liveInspectorDock"))->isHidden() == true);
    CHECK(Dock(window, QStringLiteral("projectExplorerDock"))->isHidden() == false);
    CHECK(window.findChild<QToolBar*>(QStringLiteral("renderGraphToolbar"))->isHidden() == true);
    CHECK(window.findChild<QToolBar*>(QStringLiteral("standardToolbar"))->isHidden() == false);

    // Leaving Focus Viewport returns the exact dock state and the previously active document.
    DocumentWell& well = window.GetDocumentWell();
    WorldWorkspace* world = window.findChild<WorldWorkspace*>();
    QWidget* project = window.findChild<QWidget*>(QStringLiteral("projectSettingsScroll"));
    REQUIRE(well.OpenDocument(project) == true);
    QApplication::processEvents();
    const QByteArray before = window.saveState();
    QAction* focus = window.findChild<QAction*>(QStringLiteral("view.focusViewport"));
    focus->trigger();
    QApplication::processEvents();
    CHECK(Dock(window, QStringLiteral("projectExplorerDock"))->isHidden() == true);
    focus->trigger();
    QApplication::processEvents();
    CHECK(focus->isChecked() == false);
    CHECK(window.saveState() == before);
    CHECK(world->IsChromeVisible() == true);
    CHECK(world->findChild<QWidget*>(QStringLiteral("worldHeader"))->isHidden() == false);
    CHECK(world->findChild<QWidget*>(QStringLiteral("worldHierarchyScroll"))->isHidden() == false);
    CHECK(well.GetActiveDocumentOrNull() == project);

    // Reset Window Layout also leaves Focus Viewport.
    focus->trigger();
    Reset(window);
    CHECK(focus->isChecked() == false);
    CheckDefaultPanels(window);
    CHECK(world->IsChromeVisible() == true);
}

TEST_CASE("Short windows start with the bottom tool windows closed until a problem is reported",
          "[editor][layout][error-list]")
{
    QTemporaryDir directory;
    EditorController controller(ToolingPaths{}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.resize(1470, 920);
    window.show();
    QApplication::processEvents();
    Reset(window);
    QDockWidget* errors = Dock(window, QStringLiteral("errorListDock"));
    for (const QString& name : {QStringLiteral("errorListDock"),
                                QStringLiteral("outputDock"),
                                QStringLiteral("timingDock"),
                                QStringLiteral("profilingDock")})
    {
        INFO(name.toStdString());
        CHECK(Dock(window, name)->isHidden() == true);
    }
    ErrorList* list = window.findChild<ErrorList*>();
    REQUIRE(list != nullptr);
    ErrorListEntry message;
    message.Severity = ErrorListSeverity::Message;
    message.Producer = ErrorListProducer::Build;
    message.Description = QStringLiteral("Build finished.");
    list->ReplaceEntries(ErrorListProducer::Build, {message});
    QApplication::processEvents();
    CHECK(errors->isHidden() == true);

    // Focus Viewport stays in place; the status bar announces the problem instead.
    QAction* focus = window.findChild<QAction*>(QStringLiteral("view.focusViewport"));
    REQUIRE(focus != nullptr);
    focus->trigger();
    ErrorListEntry warning = message;
    warning.Severity = ErrorListSeverity::Warning;
    warning.Description = QStringLiteral("unused variable");
    list->ReplaceEntries(ErrorListProducer::Build, {warning});
    QApplication::processEvents();
    CHECK(errors->isHidden() == true);
    CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("Error List")) == true);
    focus->trigger();
    QApplication::processEvents();
    CHECK(errors->isHidden() == true);

    ErrorListEntry error = warning;
    error.Severity = ErrorListSeverity::Error;
    list->ReplaceEntries(ErrorListProducer::Build, {error});
    QApplication::processEvents();
    CHECK(errors->isHidden() == false);
    CHECK(window.dockWidgetArea(errors) == Qt::BottomDockWidgetArea);
    CHECK(errors->height() <= window.height() / 3);

    // Taller windows keep the bottom group open by default.
    window.resize(1600, 1100);
    Reset(window);
    QApplication::processEvents();
    if (window.height() >= SHORT_WINDOW_HEIGHT_PIXELS)
    {
        CHECK(errors->isHidden() == false);
    }
}
