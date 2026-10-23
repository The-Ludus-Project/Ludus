// Visual Studio-style editor shell: document well, tool windows, commands and layout
// persistence (docs/development/editor-workspace.md#workspace-layout).

#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/error_list.h"
#include "internal/main_window.h"
#include "internal/options_dialog.h"
#include "internal/project_explorer.h"
#include "internal/render_graph_canvas.h"
#include "internal/renderer_workspace.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLatin1StringView>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSize>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

using namespace ludus::editor;
using ludus::foundation::int32;

namespace
{
/// MainWindow's short-window threshold below which the bottom tool windows start closed.
constexpr int SHORT_WINDOW_HEIGHT_PIXELS = 1000;

/// objectNames of every tool window the shell creates.
QStringList GetToolWindows()
{
    return {
        QStringLiteral("projectExplorerDock"),
        QStringLiteral("propertiesDock"),
        QStringLiteral("errorListDock"),
        QStringLiteral("outputDock"),
        QStringLiteral("contentDock"),
        QStringLiteral("renderPreviewDock"),
        QStringLiteral("liveInspectorDock"),
        QStringLiteral("timingDock"),
    };
}
/// objectNames of the command buttons the Renderer document had before its commands
/// moved to the shell's menus and Render Graph toolbar.
QStringList GetRetiredRendererButtons()
{
    return {
        QStringLiteral("renderGraphNew"),
        QStringLiteral("renderGraphOpen"),
        QStringLiteral("renderGraphValidate"),
        QStringLiteral("renderGraphBuild"),
        QStringLiteral("renderGraphBuildPreview"),
        QStringLiteral("renderGraphCook"),
        QStringLiteral("renderGraphLivePreview"),
    };
}
constexpr QLatin1StringView PACKAGE_GRAPH{"renderer/main.lurender.json"};
// Diagnostic rows the default Error List shows without scrolling.
constexpr int32 MIN_VISIBLE_DIAGNOSTIC_ROWS = 2;
// The render-graph canvas keeps room for several nodes beside the source tree, and the
// tree keeps room for nested node names.
constexpr int32 MIN_CANVAS_WIDTH_PIXELS = 300;
constexpr int32 MIN_SOURCE_TREE_WIDTH_PIXELS = 150;

QByteArray ReadBytes(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

void WriteBytes(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

/// Version-1 project whose source root is the temporary directory, with the RG2 offscreen
/// render-graph package under renderer/.
QString WriteProject(const QTemporaryDir& directory)
{
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& file : {QStringLiteral("main.lurender.json"),
                                QStringLiteral("rasterize.lurender.json"),
                                QStringLiteral("main_shaders/color.slang"),
                                QStringLiteral("main_shaders/depth.slang"),
                                QStringLiteral("main_shaders/geometry.slang")})
    {
        WriteBytes(QDir(root).filePath(QStringLiteral("renderer/") + file), ReadBytes(source.filePath(file)));
    }
    QByteArray manifest = ReadBytes(source.filePath(QStringLiteral("main.lushader.json.in")));
    manifest.replace("@RG2_TEST_PROFILES@", "[\"metal\", \"wgsl\"]");
    WriteBytes(QDir(root).filePath(QStringLiteral("renderer/main.lushader.json")), manifest);
    const QString descriptor = QDir(root).filePath(QStringLiteral("ludus.project.json"));
    WriteBytes(descriptor, QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    return descriptor;
}

DocumentWell& Well(const MainWindow& window)
{
    DocumentWell* well = window.findChild<DocumentWell*>();
    REQUIRE(well != nullptr);
    return *well;
}

QDockWidget* Dock(const MainWindow& window, const QString& name)
{
    QDockWidget* dock = window.findChild<QDockWidget*>(name);
    REQUIRE(dock != nullptr);
    return dock;
}

QAction* Action(const MainWindow& window, const QString& name)
{
    QAction* action = window.findChild<QAction*>(name);
    REQUIRE(action != nullptr);
    return action;
}

QWidget* Document(const MainWindow& window, const QString& name)
{
    QWidget* document = window.findChild<QWidget*>(name);
    REQUIRE(document != nullptr);
    return document;
}

void Settle()
{
    // Dock sizes and tab-group geometry apply on the event loop after a layout change.
    QApplication::processEvents();
    QTest::qWait(20);
    QApplication::processEvents();
}

void CheckDefaultToolWindows(const MainWindow& window)
{
    CHECK(window.centralWidget() == &Well(window));
    // Short windows start with the bottom group closed; View reopens it at the default height.
    const bool isShortWindow = window.height() < SHORT_WINDOW_HEIGHT_PIXELS;
    for (const QString& name : GetToolWindows())
    {
        INFO(name.toStdString());
        const bool isBottom = window.dockWidgetArea(Dock(window, name)) == Qt::BottomDockWidgetArea;
        CHECK(Dock(window, name)->isHidden() == (isShortWindow == true && isBottom == true));
        CHECK(Dock(window, name)->isFloating() == false);
    }
    if (isShortWindow == true)
    {
        for (const QString& command :
             {QStringLiteral("view.output"), QStringLiteral("view.timing"), QStringLiteral("view.errorList")})
        {
            Action(window, command)->trigger();
        }
        Settle();
    }
    QDockWidget* explorer = Dock(window, QStringLiteral("projectExplorerDock"));
    QDockWidget* properties = Dock(window, QStringLiteral("propertiesDock"));
    QDockWidget* errors = Dock(window, QStringLiteral("errorListDock"));
    CHECK(window.dockWidgetArea(explorer) == Qt::RightDockWidgetArea);
    CHECK(window.dockWidgetArea(properties) == Qt::RightDockWidgetArea);
    CHECK(window.dockWidgetArea(errors) == Qt::BottomDockWidgetArea);
    // Project Explorer and Properties are stacked, not tabbed, so both stay visible.
    CHECK(window.tabifiedDockWidgets(explorer).contains(properties) == false);
    CHECK(window.tabifiedDockWidgets(explorer).contains(Dock(window, QStringLiteral("contentDock"))) == true);
    CHECK(window.tabifiedDockWidgets(explorer).contains(Dock(window, QStringLiteral("renderPreviewDock"))) == true);
    CHECK(window.tabifiedDockWidgets(properties).contains(Dock(window, QStringLiteral("liveInspectorDock"))) == true);
    CHECK(window.tabifiedDockWidgets(errors).contains(Dock(window, QStringLiteral("outputDock"))) == true);
    CHECK(window.tabifiedDockWidgets(errors).contains(Dock(window, QStringLiteral("timingDock"))) == true);
}

/// Every toolbar command is shown in full: none hidden behind the overflow button and no
/// button narrower than its text needs.
void CheckToolbarsUnclipped(const MainWindow& window)
{
    for (QToolBar* toolbar : window.findChildren<QToolBar*>())
    {
        if (toolbar->isHidden() == true || toolbar->parentWidget() != &window)
        {
            continue;
        }
        INFO(toolbar->objectName().toStdString());
        QToolButton* overflowOrNull = toolbar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
        CHECK((overflowOrNull == nullptr || overflowOrNull->isVisible() == false));
        for (QAction* action : toolbar->actions())
        {
            if (action->isSeparator() == true)
            {
                continue;
            }
            QWidget* button = toolbar->widgetForAction(action);
            REQUIRE(button != nullptr);
            INFO(action->iconText().toStdString());
            CHECK(button->isVisible() == true);
            CHECK(button->width() >= button->sizeHint().width());
        }
    }
}

/// Every push button in a widget tree is at least as wide as its text needs.
void CheckButtonsUnclipped(const QWidget& root)
{
    for (QPushButton* button : root.findChildren<QPushButton*>())
    {
        if (button->isVisible() == false)
        {
            continue;
        }
        INFO(button->text().toStdString());
        CHECK(button->width() >= button->sizeHint().width());
    }
}

/// The page the Properties tool window shows.
QWidget* CurrentProperties(const MainWindow& window)
{
    QStackedWidget* stack = window.findChild<QStackedWidget*>(QStringLiteral("propertiesStack"));
    REQUIRE(stack != nullptr);
    REQUIRE(stack->currentWidget() != nullptr);
    return stack->currentWidget();
}

/// True when a visible label in root shows exactly text.
bool ShowsLabel(const QWidget& root, const QString& text)
{
    for (const QLabel* label : root.findChildren<QLabel*>())
    {
        if (label->isVisible() == true && label->text() == text)
        {
            return true;
        }
    }
    return false;
}

// Node renaming exposes the name in an editor and the kind as a separate label.
bool HasVisibleGraphNodeFields(const QWidget& root, const QString& identity, const QString& kind)
{
    const QLineEdit* nameOrNull = root.findChild<QLineEdit*>(QStringLiteral("renderGraphNodeName"));
    return nameOrNull != nullptr && nameOrNull->isVisible() == true && nameOrNull->text() == identity &&
           ShowsLabel(root, kind) == true;
}

/// Click a node on the canvas, as a user selects it.
void ClickNode(RenderGraphCanvas& inoutCanvas, const QString& node)
{
    REQUIRE(inoutCanvas.HasNode(node) == true);
    const QPointF position = inoutCanvas.GetNodePosition(node) + QPointF(20, 10);
    inoutCanvas.centerOn(position);
    QTest::mouseClick(inoutCanvas.viewport(), Qt::LeftButton, Qt::NoModifier, inoutCanvas.mapFromScene(position));
    Settle();
}

void Capture(MainWindow& inoutWindow, const QString& name)
{
    // Optional captures are for human review, never a golden-image assertion.
    const QString prefix = qEnvironmentVariable("LUDUS_EDITOR_LAYOUT_CAPTURE");
    if (prefix.isEmpty() == false)
    {
        REQUIRE(inoutWindow.grab().save(prefix + QStringLiteral("-") + name + QStringLiteral(".png")) == true);
    }
}

QString ProjectLayoutFile(const QTemporaryDir& settings)
{
    const QDir projects(QDir(settings.path()).filePath(QStringLiteral("projects")));
    const QStringList files = projects.entryList({QStringLiteral("*.json")}, QDir::Files);
    REQUIRE(files.size() == 1);
    return projects.filePath(files.first());
}

void CloseWindow(MainWindow& inoutWindow)
{
    REQUIRE(inoutWindow.close() == true);
}
} // namespace

TEST_CASE("Shell puts documents in the centre and tool windows around them", "[editor][layout][shell]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    Settle();
    CheckDefaultToolWindows(window);
    DocumentWell& well = Well(window);
    QWidget* welcome = Document(window, QStringLiteral("projectWelcome"));
    CHECK(well.GetActiveDocumentOrNull() == welcome);
    CHECK(well.IsDocumentAvailable(Document(window, QStringLiteral("configurationWorkspace"))) == true);
    CHECK(well.IsDocumentAvailable(window.findChild<WorldWorkspace*>()) == false);

    controller.OpenProject(WriteProject(directory));
    REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
    Settle();
    WorldWorkspace* world = window.findChild<WorldWorkspace*>();
    REQUIRE(world != nullptr);
    CHECK(well.GetActiveDocumentOrNull() == world);
    CHECK(well.IsDocumentAvailable(welcome) == false);
    CHECK(well.GetGroupIndex(world) == 0);
    // The World's entity inspector is hosted by Properties, not inside the document.
    QWidget* worldProperties = Document(window, QStringLiteral("worldProperties"));
    CHECK(Dock(window, QStringLiteral("propertiesDock"))->isAncestorOf(worldProperties) == true);
    CHECK(worldProperties->isVisible() == true);
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(renderer != nullptr);

    for (const QSize& size : {QSize(1920, 1080), QSize(1280, 800)})
    {
        INFO(size.width());
        window.resize(size);
        Action(window, QStringLiteral("resetWorkspaceLayout"))->trigger();
        Settle();
        CheckDefaultToolWindows(window);
        CHECK(window.width() == size.width());
        // Documents get most of the window; tool windows keep useful default sizes.
        CHECK(well.width() * 100 >= window.width() * 65);
        CHECK(well.height() * 100 >= window.height() * 55);
        CHECK(Dock(window, QStringLiteral("projectExplorerDock"))->width() >= 260);
        // The bottom tool windows keep room for several diagnostic rows. Measured on the
        // table, since the dock's own tab bar height differs between platform styles.
        QTableWidget* diagnostics = window.findChild<QTableWidget*>(QStringLiteral("errorListTable"));
        REQUIRE(diagnostics != nullptr);
        CHECK(diagnostics->viewport()->height() >=
              MIN_VISIBLE_DIAGNOSTIC_ROWS * diagnostics->verticalHeader()->defaultSectionSize());
        CheckToolbarsUnclipped(window);
        REQUIRE(well.OpenDocument(world) == true);
        Settle();
        // The default document and the tool windows have room for every command label.
        CheckButtonsUnclipped(*world);
        CheckButtonsUnclipped(*Dock(window, QStringLiteral("propertiesDock")));
        Capture(window, QStringLiteral("world-%1").arg(size.width()));
        REQUIRE(well.OpenDocument(renderer) == true);
        Settle();
        // The render-graph document is a canvas with its source tree; its commands are the
        // shell's, and its inspector and preview are tool windows.
        REQUIRE(renderer->GetDocument().IsLoaded() == true);
        CHECK(renderer->GetCanvas().isVisible() == true);
        CHECK(renderer->GetCanvas().width() > MIN_CANVAS_WIDTH_PIXELS);
        QWidget* tree = renderer->findChild<QWidget*>(QStringLiteral("renderGraphTree"));
        REQUIRE(tree != nullptr);
        CHECK(tree->width() >= MIN_SOURCE_TREE_WIDTH_PIXELS);
        for (const QString& name : GetRetiredRendererButtons())
        {
            INFO(name.toStdString());
            CHECK(window.findChild<QWidget*>(name) == nullptr);
        }
        CheckButtonsUnclipped(*renderer);
        CHECK(CurrentProperties(window)->objectName() == QStringLiteral("renderGraphProperties"));
        CheckButtonsUnclipped(*Dock(window, QStringLiteral("propertiesDock")));
        CheckToolbarsUnclipped(window);
        Capture(window, QStringLiteral("render-graph-%1").arg(size.width()));
        QDockWidget* preview = Dock(window, QStringLiteral("renderPreviewDock"));
        preview->raise();
        Settle();
        CHECK(preview->isAncestorOf(window.findChild<QWidget*>(QStringLiteral("renderGraphPreviewPanel"))) == true);
        CheckButtonsUnclipped(*preview);
        Capture(window, QStringLiteral("render-preview-%1").arg(size.width()));
    }
}

TEST_CASE("Window menu shows hides and resets every tool window", "[editor][layout][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    // Tall enough that the default arrangement opens every tool window.
    window.resize(1600, 1100);
    window.show();
    Settle();
    QMenu* menu = window.findChild<QMenu*>(QStringLiteral("windowMenu"));
    REQUIRE(menu != nullptr);
    for (const QString& name : GetToolWindows())
    {
        INFO(name.toStdString());
        QDockWidget* dock = Dock(window, name);
        QAction* toggle = dock->toggleViewAction();
        CHECK(menu->actions().contains(toggle) == true);
        CHECK(toggle->isChecked() == true);
        toggle->trigger();
        CHECK(dock->isHidden() == true);
        toggle->trigger();
        CHECK(dock->isHidden() == false);
        toggle->trigger();
    }
    // View commands show and focus a tool window instead of toggling it.
    Action(window, QStringLiteral("view.errorList"))->trigger();
    CHECK(Dock(window, QStringLiteral("errorListDock"))->isHidden() == false);
    Dock(window, QStringLiteral("propertiesDock"))->setFloating(true);
    window.addDockWidget(Qt::LeftDockWidgetArea, Dock(window, QStringLiteral("errorListDock")));
    for (QToolBar* toolbar : window.findChildren<QToolBar*>())
    {
        toolbar->hide();
    }
    CHECK(menu->actions().contains(Action(window, QStringLiteral("resetWorkspaceLayout"))) == true);
    Action(window, QStringLiteral("resetWorkspaceLayout"))->trigger();
    Settle();
    CheckDefaultToolWindows(window);
    for (const QString& name :
         {QStringLiteral("standardToolbar"), QStringLiteral("gameToolbar"), QStringLiteral("renderGraphToolbar")})
    {
        QToolBar* toolbar = window.findChild<QToolBar*>(name);
        REQUIRE(toolbar != nullptr);
        CHECK(toolbar->isHidden() == false);
    }
}

TEST_CASE("Tool windows and document groups restore and corrupt layouts fall back", "[editor][layout][shell]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    const QString preferences = QDir(settings.path()).filePath(QStringLiteral("workspace.json"));
    const QString descriptor = WriteProject(directory);
    {
        EditorController controller(ToolingPaths{},
                                    nullptr,
                                    QDir(settings.path()).filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, preferences);
        window.show();
        controller.OpenProject(descriptor);
        Settle();
        DocumentWell& well = Well(window);
        RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
        const QString graph = QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(PACKAGE_GRAPH);
        REQUIRE(window.findChild<ProjectExplorer*>()->ActivateItem(ProjectItemKind::RenderGraph, graph) == true);
        CHECK(well.GetActiveDocumentOrNull() == renderer);
        CHECK(renderer->GetDocument().IsLoaded() == true);
        REQUIRE(well.SplitActiveDocument(Qt::Horizontal) == true);
        CHECK(well.GetGroupCount() == 2);
        CHECK(well.GetGroupIndex(renderer) == 1);
        CHECK(well.GetGroupIndex(window.findChild<WorldWorkspace*>()) == 0);
        window.addDockWidget(Qt::LeftDockWidgetArea, Dock(window, QStringLiteral("errorListDock")));
        Dock(window, QStringLiteral("timingDock"))->hide();
        CloseWindow(window);
    }
    {
        EditorController controller(ToolingPaths{},
                                    nullptr,
                                    QDir(settings.path()).filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, preferences);
        window.show();
        controller.OpenProject(descriptor);
        Settle();
        DocumentWell& well = Well(window);
        CHECK(window.dockWidgetArea(Dock(window, QStringLiteral("errorListDock"))) == Qt::LeftDockWidgetArea);
        CHECK(Dock(window, QStringLiteral("timingDock"))->isHidden() == true);
        CHECK(well.GetGroupCount() == 2);
        CHECK(well.GetSplitOrientation() == Qt::Horizontal);
        CHECK(well.GetGroupIndex(window.findChild<RendererWorkspace*>()) == 1);
        CHECK(well.GetGroupIndex(window.findChild<WorldWorkspace*>()) == 0);
        CloseWindow(window);
    }
    // A corrupt document arrangement keeps the project's default World document; the
    // project's valid tool-window arrangement is still restored.
    const QString projectLayout = ProjectLayoutFile(settings);
    QJsonObject object = QJsonDocument::fromJson(ReadBytes(projectLayout)).object();
    object.insert(QStringLiteral("documents"),
                  QJsonObject{{QStringLiteral("version"), 1},
                              {QStringLiteral("orientation"), QStringLiteral("diagonal")},
                              {QStringLiteral("groups"), QJsonArray{QStringLiteral("rendererWorkspace")}}});
    WriteBytes(projectLayout, QJsonDocument(object).toJson(QJsonDocument::Compact));
    {
        EditorController controller(ToolingPaths{},
                                    nullptr,
                                    QDir(settings.path()).filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, preferences);
        window.show();
        controller.OpenProject(descriptor);
        Settle();
        DocumentWell& well = Well(window);
        CHECK(well.GetGroupCount() == 1);
        CHECK(well.GetActiveDocumentOrNull() == window.findChild<WorldWorkspace*>());
        CHECK(window.dockWidgetArea(Dock(window, QStringLiteral("errorListDock"))) == Qt::LeftDockWidgetArea);
        CloseWindow(window);
    }
    // Layouts saved by an older shell (version 4: no Render Preview tool window) are not restored.
    for (const QString& path : {preferences, ProjectLayoutFile(settings)})
    {
        QJsonObject old = QJsonDocument::fromJson(ReadBytes(path)).object();
        old.insert(QStringLiteral("version"), 4);
        WriteBytes(path, QJsonDocument(old).toJson(QJsonDocument::Compact));
    }
    {
        EditorController controller(ToolingPaths{},
                                    nullptr,
                                    QDir(settings.path()).filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, preferences);
        window.show();
        controller.OpenProject(descriptor);
        Settle();
        DocumentWell& well = Well(window);
        CHECK(well.GetGroupCount() == 1);
        CHECK(well.GetActiveDocumentOrNull() == window.findChild<WorldWorkspace*>());
        CheckDefaultToolWindows(window);
        CloseWindow(window);
    }
}

TEST_CASE("Save and Undo follow the active document tab", "[editor][layout][shell][document]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    window.activateWindow();
    controller.OpenProject(WriteProject(directory));
    Settle();
    DocumentWell& well = Well(window);
    QAction* save = Action(window, QStringLiteral("document.save"));
    QAction* undo = Action(window, QStringLiteral("document.undo"));
    WorldWorkspace* world = window.findChild<WorldWorkspace*>();
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(well.OpenDocument(Document(window, QStringLiteral("projectSettingsScroll"))) == true);
    CHECK(save->text() == QStringLiteral("&Save Project Settings"));
    REQUIRE(well.OpenDocument(Document(window, QStringLiteral("configurationWorkspace"))) == true);
    CHECK(save->text() == QStringLiteral("&Save Preferences"));
    REQUIRE(well.OpenDocument(renderer) == true);
    CHECK(save->text() == QStringLiteral("&Save Render Graph"));

    // A World edit made through Properties is saved and undone as the World document.
    const QString path = QFileInfo(directory.path()).canonicalFilePath() + QStringLiteral("/shell.world.json");
    REQUIRE(world->Create(path) == true);
    REQUIRE(well.OpenDocument(world) == true);
    CHECK(save->text() == QStringLiteral("&Save World"));
    QListWidget* entities = world->findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entities != nullptr);
    const int32 before = entities->count();
    QPushButton* add = world->findChild<QPushButton*>(QStringLiteral("worldAdd"));
    REQUIRE(add != nullptr);
    add->click();
    Settle();
    CHECK(entities->count() == before + 1);
    // Focus in the Properties tool window keeps the World as the active document.
    QPushButton* apply = window.findChild<QPushButton*>(QStringLiteral("worldApply"));
    REQUIRE(apply != nullptr);
    apply->setFocus();
    Settle();
    CHECK(well.GetActiveDocumentOrNull() == world);
    CHECK(save->text() == QStringLiteral("&Save World"));
    // With the World and the Render Graph in separate groups, the active group decides.
    REQUIRE(well.OpenDocument(renderer) == true);
    REQUIRE(well.SplitActiveDocument(Qt::Horizontal) == true);
    CHECK(save->text() == QStringLiteral("&Save Render Graph"));
    REQUIRE(well.OpenDocument(world) == true);
    entities->setFocus();
    Settle();
    CHECK(save->text() == QStringLiteral("&Save World"));
    REQUIRE(undo->isEnabled() == true);
    undo->trigger();
    CHECK(entities->count() == before);
    save->trigger();
    CHECK(world->Dirty() == false);
}

TEST_CASE("Project Explorer opens documents and Error List navigates to graph nodes and lines",
          "[editor][layout][shell][render-graph]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    controller.OpenProject(WriteProject(directory));
    Settle();
    DocumentWell& well = Well(window);
    ProjectExplorer* explorer = window.findChild<ProjectExplorer*>();
    REQUIRE(explorer != nullptr);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    CHECK(explorer->GetFiles(ProjectItemKind::RenderGraph).contains(QDir(root).filePath(PACKAGE_GRAPH)) == true);
    CHECK(explorer->GetFiles(ProjectItemKind::Shader)
              .contains(QDir(root).filePath(QStringLiteral("renderer/main_shaders/geometry.slang"))) == true);
    REQUIRE(explorer->ActivateItem(ProjectItemKind::ProjectSettings, {}) == true);
    CHECK(well.GetActiveDocumentOrNull() == Document(window, QStringLiteral("projectSettingsScroll")));

    // A shader opens the render-graph package that owns it.
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(explorer->ActivateItem(ProjectItemKind::Shader,
                                   QDir(root).filePath(QStringLiteral("renderer/main_shaders/geometry.slang"))) ==
            true);
    CHECK(well.GetActiveDocumentOrNull() == renderer);
    REQUIRE(renderer->GetDocument().IsLoaded() == true);
    CHECK(renderer->GetDocument().GetRootGraph() == QStringLiteral("main.lurender.json"));
    CHECK(Action(window, QStringLiteral("renderGraph.validate"))->isEnabled() == true);

    // A mapped graph error and a shader diagnostic, as the shared CLI reports them.
    RenderGraphJobResult result;
    result.Request.ProjectEpoch = controller.State().ProjectEpoch;
    result.Request.Action = QStringLiteral("build");
    result.IsSuccessful = false;
    result.Code = QStringLiteral("RenderGraphInitialization");
    result.Message = QStringLiteral("depth_load Load reads an uninitialized target");
    result.Location = QJsonObject{{QStringLiteral("source"), QStringLiteral("rasterize.lurender.json")},
                                  {QStringLiteral("configuration"), QStringLiteral("ColorAndDepth")},
                                  {QStringLiteral("node"), QStringLiteral("draw")}};
    result.Diagnostics =
        QJsonArray{QJsonObject{{QStringLiteral("severity"), QStringLiteral("warning")},
                               {QStringLiteral("message"), QStringLiteral("implicit truncation")},
                               {QStringLiteral("program"), QStringLiteral("Geometry")},
                               {QStringLiteral("target"), QStringLiteral("metal")},
                               {QStringLiteral("location"),
                                QJsonObject{{QStringLiteral("source"), QStringLiteral("main_shaders/geometry.slang")},
                                            {QStringLiteral("line"), 3}}}}};
    REQUIRE(well.OpenDocument(window.findChild<WorldWorkspace*>()) == true);
    renderer->AcceptResult(result);
    ErrorList* errors = window.findChild<ErrorList*>();
    REQUIRE(errors != nullptr);
    REQUIRE(errors->GetEntries().size() == 2);
    CHECK(errors->GetCount(ErrorListSeverity::Error) == 1);
    CHECK(errors->GetCount(ErrorListSeverity::Warning) == 1);
    CHECK(errors->GetEntries().at(1).Origin == QStringLiteral("Shader"));
    CHECK(Dock(window, QStringLiteral("errorListDock"))->isVisible() == true);
    REQUIRE(errors->ActivateVisibleRow(0) == true);
    CHECK(well.GetActiveDocumentOrNull() == renderer);
    CHECK(renderer->GetSelection() == QStringLiteral("rasterize.lurender.json#ColorAndDepth/draw"));
    REQUIRE(errors->ActivateVisibleRow(1) == true);
    CHECK(renderer->GetSourceFile() == QStringLiteral("main_shaders/geometry.slang"));
    CHECK(renderer->GetSourceLine() == 3);
    // Severity filters hide rows without discarding them.
    QToolButton* warnings = window.findChild<QToolButton*>(QStringLiteral("errorListWarnings"));
    REQUIRE(warnings != nullptr);
    warnings->click();
    CHECK(errors->GetVisibleCount() == 1);
    warnings->click();
    CHECK(errors->GetVisibleCount() == 2);

    // Build diagnostics select their Output line; render-graph rows are kept.
    const QString output = QStringLiteral("[1/2] Building CXX object game.o\n"
                                          "src/game.cpp:12:5: error: use of undeclared identifier 'x'\n"
                                          "CMake Warning at CMakeLists.txt:7 (message):\n"
                                          "  unused variable\n");
    QPlainTextEdit* text = window.findChild<QPlainTextEdit*>(QStringLiteral("workspaceOutput"));
    REQUIRE(text != nullptr);
    text->setPlainText(output);
    const QList<ErrorListEntry> build = ParseBuildErrorEntries(output);
    REQUIRE(build.size() == 2);
    CHECK(build.at(0).File == QStringLiteral("src/game.cpp"));
    CHECK(build.at(0).Severity == ErrorListSeverity::Error);
    CHECK(build.at(0).SourceIndex == 1);
    CHECK(build.at(1).File == QStringLiteral("CMakeLists.txt"));
    CHECK(build.at(1).Severity == ErrorListSeverity::Warning);
    CHECK(build.at(1).Description == QStringLiteral("unused variable"));
    errors->ReplaceEntries(ErrorListProducer::Build, build);
    CHECK(errors->GetEntries().size() == 4);
    REQUIRE(errors->ActivateVisibleRow(2) == true);
    CHECK(Dock(window, QStringLiteral("outputDock"))->isVisible() == true);
    CHECK(text->textCursor().block().blockNumber() == 1);
}

TEST_CASE("Document well refuses malformed arrangements and keeps closed views alive", "[editor][layout][shell]")
{
    DocumentWell well;
    QWidget* first = new QWidget;
    first->setObjectName(QStringLiteral("first"));
    QWidget* second = new QWidget;
    second->setObjectName(QStringLiteral("second"));
    well.AddDocument(first, QStringLiteral("First"));
    well.AddDocument(second, QStringLiteral("Second"));
    well.resize(800, 600);
    well.show();
    CHECK(well.GetActiveDocumentOrNull() == nullptr);
    REQUIRE(well.OpenDocument(first) == true);
    CHECK(well.CanSplit(Qt::Horizontal) == false); // One document cannot leave its only group.
    REQUIRE(well.OpenDocument(second) == true);
    REQUIRE(well.SplitActiveDocument(Qt::Vertical) == true);
    CHECK(well.CanSplit(Qt::Vertical) == false); // The new group holds only one document.
    const QJsonObject saved = well.SaveState();
    well.CloseDocument(second);
    CHECK(well.GetGroupCount() == 1);
    CHECK(second->parent() != nullptr);
    CHECK(well.IsDocumentOpen(second) == false);
    for (const QJsonObject& malformed : {
             QJsonObject{},
             QJsonObject{{QStringLiteral("version"), 2}},
             QJsonObject{{QStringLiteral("version"), 1},
                         {QStringLiteral("orientation"), QStringLiteral("horizontal")},
                         {QStringLiteral("groups"), QJsonArray{1, 2, 3, 4, 5}}},
         })
    {
        CHECK(well.RestoreState(malformed) == false);
        CHECK(well.GetGroupCount() == 1);
        CHECK(well.IsDocumentOpen(first) == true);
    }
    REQUIRE(well.RestoreState(saved) == true);
    CHECK(well.GetGroupCount() == 2);
    CHECK(well.GetSplitOrientation() == Qt::Vertical);
    CHECK(well.GetGroupIndex(second) == 1);
    well.SetDocumentAvailable(second, false);
    CHECK(well.IsDocumentOpen(second) == false);
    CHECK(well.OpenDocument(second) == false);
    well.MergeGroups();
    CHECK(well.GetGroupCount() == 1);
}

namespace
{
QWidget* MakeDocument(const QString& name)
{
    QWidget* document = new QWidget;
    document->setObjectName(name);
    return document;
}

QStringList GetSavedDocumentNames(const QJsonObject& state)
{
    QStringList names;
    for (const QJsonValueConstRef group : state.value(QStringLiteral("groups")).toArray())
    {
        for (const QJsonValueConstRef name : group.toObject().value(QStringLiteral("documents")).toArray())
        {
            names.append(name.toString());
        }
        names.append(group.toObject().value(QStringLiteral("current")).toString());
    }
    return names;
}
} // namespace

TEST_CASE("Document well hides ordinary views and asks owners to close owner-closed views", "[editor][layout][shell]")
{
    DocumentWell well;
    QWidget* hidden = MakeDocument(QStringLiteral("hidden"));
    QWidget* owned = MakeDocument(QStringLiteral("owned"));
    well.AddDocument(hidden, QStringLiteral("Hidden"));
    well.AddDocument(owned, QStringLiteral("Owned"), DocumentCloseMode::OwnerCloses);
    well.resize(800, 600);
    well.show();
    CHECK(well.IsDocumentOwnerClosed(hidden) == false);
    CHECK(well.IsDocumentOwnerClosed(owned) == true);
    QSignalSpy requests(&well, &DocumentWell::CloseRequested);
    REQUIRE(well.OpenDocument(hidden) == true);
    REQUIRE(well.OpenDocument(owned) == true);

    // An ordinary view hides without asking; an ignored request keeps the owned view open.
    well.CloseDocument(hidden);
    CHECK(well.IsDocumentOpen(hidden) == false);
    CHECK(requests.isEmpty() == true);
    well.CloseDocument(owned);
    REQUIRE(requests.size() == 1);
    CHECK(requests.at(0).at(0).value<QWidget*>() == owned);
    CHECK(well.IsDocumentOpen(owned) == true);
    CHECK(well.GetActiveDocumentOrNull() == owned);

    // The tab close button and Close All route through the same request.
    QTabWidget* group = well.findChild<QTabWidget*>(QStringLiteral("documentGroup"));
    REQUIRE(group != nullptr);
    Q_EMIT group->tabCloseRequested(group->indexOf(owned));
    CHECK(requests.size() == 2);
    REQUIRE(well.OpenDocument(hidden) == true);
    well.CloseAllDocuments();
    CHECK(well.IsDocumentOpen(hidden) == false);
    CHECK(requests.size() == 3);
    CHECK(well.IsDocumentOpen(owned) == true);

    // Unavailability is not a user close, so the owned view hides without a request.
    well.SetDocumentAvailable(owned, false);
    CHECK(well.IsDocumentOpen(owned) == false);
    CHECK(requests.size() == 3);
    // Closing a view that is not open asks nothing.
    well.CloseDocument(owned);
    CHECK(requests.size() == 3);
}

TEST_CASE("Document well removes owner-closed views while active or split", "[editor][layout][shell]")
{
    DocumentWell well;
    QWidget* first = MakeDocument(QStringLiteral("first"));
    QWidget* second = MakeDocument(QStringLiteral("second"));
    QWidget* third = MakeDocument(QStringLiteral("third"));
    well.AddDocument(first, QStringLiteral("First"));
    well.AddDocument(second, QStringLiteral("Second"), DocumentCloseMode::OwnerCloses);
    well.AddDocument(third, QStringLiteral("Third"), DocumentCloseMode::OwnerCloses);
    well.resize(800, 600);
    well.show();
    QSignalSpy active(&well, &DocumentWell::ActiveDocumentChanged);
    // Owners close on request; removing during Close All must not reach a deleted view.
    QObject::connect(&well, &DocumentWell::CloseRequested, &well, [&well](QWidget* document) {
        REQUIRE(well.RemoveDocument(document) == true);
        CHECK(document->parent() == nullptr);
        CHECK(document->isVisible() == false);
        delete document;
    });

    // Removing the active view publishes its replacement and hands the view back.
    REQUIRE(well.OpenDocument(first) == true);
    REQUIRE(well.OpenDocument(second) == true);
    REQUIRE(well.GetActiveDocumentOrNull() == second);
    const QJsonObject withSecond = well.SaveState();
    CHECK(GetSavedDocumentNames(withSecond).contains(QStringLiteral("second")) == true);
    active.clear();
    well.CloseDocument(second);
    CHECK(well.GetDocuments() == QList<QWidget*>{first, third});
    CHECK(well.GetActiveDocumentOrNull() == first);
    REQUIRE(active.isEmpty() == false);
    CHECK(active.last().at(0).value<QWidget*>() == first);
    CHECK(GetSavedDocumentNames(well.SaveState()).contains(QStringLiteral("second")) == false);
    // A saved arrangement naming the removed view restores without it.
    REQUIRE(well.RestoreState(withSecond) == true);
    CHECK(well.GetDocuments() == QList<QWidget*>{first, third});
    CHECK(well.IsDocumentOpen(first) == true);
    CHECK(GetSavedDocumentNames(well.SaveState()).contains(QStringLiteral("second")) == false);

    // Removing the only view of a split group removes that group.
    REQUIRE(well.OpenDocument(third) == true);
    REQUIRE(well.SplitActiveDocument(Qt::Horizontal) == true);
    REQUIRE(well.GetGroupCount() == 2);
    REQUIRE(well.GetGroupIndex(third) == 1);
    well.CloseAllDocuments();
    CHECK(well.GetGroupCount() == 1);
    CHECK(well.GetDocuments() == QList<QWidget*>{first});
    CHECK(well.GetActiveDocumentOrNull() == nullptr);
    CHECK(GetSavedDocumentNames(well.SaveState()).contains(QStringLiteral("third")) == false);

    // Removing a closed or unknown view changes nothing.
    QWidget unknown;
    CHECK(well.RemoveDocument(&unknown) == false);
    CHECK(well.RemoveDocument(first) == true);
    CHECK(well.GetDocuments().isEmpty() == true);
    delete first;
}

TEST_CASE("Window Close Document and Close All ask owners of owner-closed documents", "[editor][layout][shell]")
{
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    Settle();
    DocumentWell& well = Well(window);
    QWidget* welcome = Document(window, QStringLiteral("projectWelcome"));
    QWidget* owned = MakeDocument(QStringLiteral("ownedDocument"));
    well.AddDocument(owned, QStringLiteral("Owned"), DocumentCloseMode::OwnerCloses);
    QSignalSpy requests(&well, &DocumentWell::CloseRequested);
    REQUIRE(well.OpenDocument(owned) == true);

    Action(window, QStringLiteral("window.closeDocument"))->trigger();
    REQUIRE(requests.size() == 1);
    CHECK(requests.at(0).at(0).value<QWidget*>() == owned);
    CHECK(well.IsDocumentOpen(owned) == true);
    REQUIRE(well.OpenDocument(welcome) == true);
    Action(window, QStringLiteral("window.closeAllDocuments"))->trigger();
    CHECK(requests.size() == 2);
    CHECK(well.IsDocumentOpen(welcome) == false);
    CHECK(well.IsDocumentOpen(owned) == true);
    REQUIRE(well.RemoveDocument(owned) == true);
    delete owned;
    CHECK(well.GetActiveDocumentOrNull() == nullptr);
}

TEST_CASE("Properties follows the render-graph canvas selection", "[editor][layout][shell][render-graph]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.resize(1280, 800);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window) == true);
    controller.OpenProject(WriteProject(directory));
    Settle();
    DocumentWell& well = Well(window);
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(renderer != nullptr);
    const QString graph = QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(PACKAGE_GRAPH);
    REQUIRE(window.findChild<ProjectExplorer*>()->ActivateItem(ProjectItemKind::RenderGraph, graph) == true);
    REQUIRE(well.GetActiveDocumentOrNull() == renderer);
    Settle();
    QWidget* properties = CurrentProperties(window);
    CHECK(properties->objectName() == QStringLiteral("renderGraphProperties"));
    CHECK(properties->isVisible() == true);
    CHECK(Dock(window, QStringLiteral("propertiesDock"))->isAncestorOf(properties) == true);

    // A node clicked on the canvas is the node Properties shows and edits.
    RenderGraphCanvas& canvas = renderer->GetCanvas();
    ClickNode(canvas, QStringLiteral("color"));
    CHECK(renderer->GetSelection() == QStringLiteral("main.lurender.json#Default/color"));
    CHECK(HasVisibleGraphNodeFields(*properties, QStringLiteral("color"), QStringLiteral("Target")) == true);
    QComboBox* format = properties->findChild<QComboBox*>(QStringLiteral("renderGraphField_format"));
    REQUIRE(format != nullptr);
    CHECK(format->isVisible() == true);
    ClickNode(canvas, QStringLiteral("camera"));
    CHECK(HasVisibleGraphNodeFields(*properties, QStringLiteral("camera"), QStringLiteral("Camera")) == true);
    CHECK(HasVisibleGraphNodeFields(*properties, QStringLiteral("color"), QStringLiteral("Target")) == false);
    QComboBox* role = properties->findChild<QComboBox*>(QStringLiteral("renderGraphField_role"));
    REQUIRE(role != nullptr);
    const int32 other = role->currentIndex() == 0 ? 1 : 0;
    role->setCurrentIndex(other);
    Q_EMIT role->activated(other);
    Settle();
    CHECK(renderer->IsDirty() == true);
    CHECK(renderer->CanUndo() == true);

    // Another document shows its own page; returning keeps the canvas selection.
    REQUIRE(well.OpenDocument(window.findChild<WorldWorkspace*>()) == true);
    Settle();
    CHECK(CurrentProperties(window)->objectName() == QStringLiteral("worldProperties"));
    REQUIRE(well.OpenDocument(renderer) == true);
    Settle();
    CHECK(CurrentProperties(window) == properties);
    CHECK(HasVisibleGraphNodeFields(*properties, QStringLiteral("camera"), QStringLiteral("Camera")) == true);
    // Error List navigation selects the authored node, which Properties then shows.
    REQUIRE(renderer->SelectNode(QStringLiteral("rasterize.lurender.json"),
                                 QStringLiteral("ColorAndDepth"),
                                 QStringLiteral("draw")) == true);
    Settle();
    CHECK(HasVisibleGraphNodeFields(*properties, QStringLiteral("draw"), QStringLiteral("Raster")) == true);
}

TEST_CASE("Live Preview and Cancel are shell commands over the Renderer document",
          "[editor][layout][shell][render-graph]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    QAction* live = Action(window, QStringLiteral("renderGraph.livePreview"));
    QAction* cancel = Action(window, QStringLiteral("renderGraph.cancel"));
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(renderer != nullptr);
    // Without a project nothing is open, so the toggle stays off.
    CHECK(live->isEnabled() == false);
    renderer->SetLivePreview(true);
    CHECK(renderer->IsLivePreviewEnabled() == false);

    controller.OpenProject(WriteProject(directory));
    Settle();
    REQUIRE(renderer->GetDocument().IsLoaded() == true);
    CHECK(live->isEnabled() == true);
    CHECK(cancel->isEnabled() == false);
    // Native World rendering starts live automatically; the command can pause and resume it.
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    CHECK(renderer->IsLivePreviewEnabled() == true);
    CHECK(live->isChecked() == true);
    live->trigger();
    CHECK(renderer->IsLivePreviewEnabled() == false);
#endif
    live->trigger();
    CHECK(renderer->IsLivePreviewEnabled() == true);
    CHECK(live->isChecked() == true);
    // Cancel (from any caller) turns Live Preview off, and the toggle follows.
    renderer->CancelJob();
    CHECK(renderer->IsLivePreviewEnabled() == false);
    CHECK(live->isChecked() == false);
    renderer->SetLivePreview(true);
    CHECK(live->isChecked() == true);
    live->trigger();
    CHECK(renderer->IsLivePreviewEnabled() == false);
    CHECK(live->isChecked() == false);
}

TEST_CASE("Tools Options round-trips the render-graph build settings", "[editor][layout][shell][render-graph]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    controller.OpenProject(WriteProject(directory));
    Settle();
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(renderer != nullptr);
    QMenu* tools = window.findChild<QMenu*>(QStringLiteral("toolsMenu"));
    REQUIRE(tools != nullptr);
    QAction* options = Action(window, QStringLiteral("tools.options"));
    CHECK(tools->actions().contains(options) == true);
    // The worker socket is an editor preference; it is left as it is.
    RenderGraphBuildSettings initial = renderer->GetBuildSettings();
    initial.ShouldOverrideTools = true;
    initial.Tools = RenderGraphBuildTools
    {
        .Helper = QStringLiteral("/tools/helper"),
        .Compiler = QStringLiteral("/tools/slangc"),
        .Validator = QStringLiteral("/tools/spirv-val"),
        .SpirvCross = {},
    };
    renderer->SetBuildSettings(initial);

    const auto openOptions = [&window, options]() {
        options->trigger();
        Settle();
        OptionsDialog* dialog = window.findChild<OptionsDialog*>();
        REQUIRE(dialog != nullptr);
        CHECK(dialog->isVisible() == true);
        return dialog;
    };
    OptionsDialog* dialog = openOptions();
    CHECK(dialog->GetBuildSettings() == initial);
    QLineEdit* compiler = dialog->findChild<QLineEdit*>(QStringLiteral("renderGraphCompiler"));
    REQUIRE(compiler != nullptr);
    CHECK(compiler->text() == QStringLiteral("/tools/slangc"));
    CHECK(compiler->isEnabled() == true);
    compiler->setText(QStringLiteral("  /tools/other-slangc "));
    dialog->accept();
    Settle();
    RenderGraphBuildSettings expected = initial;
    expected.Tools.Compiler = QStringLiteral("/tools/other-slangc");
    CHECK(renderer->GetBuildSettings() == expected);

    // An incomplete override is refused in place; Cancel changes nothing.
    dialog = openOptions();
    dialog->findChild<QLineEdit*>(QStringLiteral("renderGraphCompiler"))->clear();
    dialog->accept();
    CHECK(dialog->isVisible() == true);
    QLabel* problem = dialog->findChild<QLabel*>(QStringLiteral("optionsProblem"));
    REQUIRE(problem != nullptr);
    CHECK(problem->text().isEmpty() == false);
    dialog->reject();
    Settle();
    CHECK(renderer->GetBuildSettings() == expected);

    // Turning the override off keeps the paths for later and builds use the project's SDK.
    dialog = openOptions();
    QCheckBox* overrideTools = dialog->findChild<QCheckBox*>(QStringLiteral("renderGraphOverrideTools"));
    REQUIRE(overrideTools != nullptr);
    overrideTools->setChecked(false);
    CHECK(dialog->findChild<QLineEdit*>(QStringLiteral("renderGraphCompiler"))->isEnabled() == false);
    dialog->accept();
    Settle();
    CHECK(renderer->GetBuildSettings().ShouldOverrideTools == false);
    CHECK(renderer->GetBuildSettings().Tools == expected.Tools);
    CHECK(window.findChild<OptionsDialog*>() == nullptr);
}

TEST_CASE("Missing rendering tools offer project repair directly above the viewport", "[editor][setup][shader-setup]")
{
    QTemporaryDir directory;
    const QString descriptor = WriteProject(directory);
    WriteBytes(descriptor,
               ReadBytes(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/apps/editor/tests/fixtures/valid_v2_cmake.json")));
    EditorController controller(ToolingPaths{});
    MainWindow window(&controller);
    window.show();
    controller.OpenProject(descriptor);
    Settle();
    RendererWorkspace* rendererOrNull = window.findChild<RendererWorkspace*>();
    REQUIRE(rendererOrNull != nullptr);
    REQUIRE(rendererOrNull->OpenGraph(QDir(directory.path()).filePath(PACKAGE_GRAPH)) == true);
    RenderGraphJobResult result;
    result.Request.ProjectEpoch = controller.State().ProjectEpoch;
    result.Request.Action = QStringLiteral("build");
    result.Code = QStringLiteral("MissingTools");
    result.Message =
        QStringLiteral("Rendering tools need preparation. Choose Repair setup to prepare them automatically.");
    rendererOrNull->AcceptResult(result);
    Settle();
    QToolBar* noticeOrNull = window.findChild<QToolBar*>(QStringLiteral("projectSetupNotice"));
    QPushButton* repairOrNull = window.findChild<QPushButton*>(QStringLiteral("projectSetupNoticeRepair"));
    REQUIRE(noticeOrNull != nullptr);
    REQUIRE(repairOrNull != nullptr);
    CHECK(controller.State().SetupProblem == result.Message);
    CHECK(noticeOrNull->isVisible() == true);
    REQUIRE(repairOrNull->isEnabled() == true);
    repairOrNull->click();
    QDialog* dialogOrNull = window.findChild<QDialog*>(QStringLiteral("projectSetupDialog"));
    REQUIRE(dialogOrNull != nullptr);
    CHECK(dialogOrNull->isVisible() == true);
    dialogOrNull->reject();
}

TEST_CASE("The setup notice is one dismissible line and the status bar keeps one line", "[editor][setup][layout]")
{
    QTemporaryDir directory;
    const QString descriptor = WriteProject(directory);
    WriteBytes(descriptor,
               ReadBytes(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/apps/editor/tests/fixtures/valid_v2_cmake.json")));
    EditorController controller(ToolingPaths{});
    MainWindow window(&controller);
    window.show();
    controller.OpenProject(descriptor);
    Settle();
    QToolBar* noticeOrNull = window.findChild<QToolBar*>(QStringLiteral("projectSetupNotice"));
    QLabel* textOrNull = window.findChild<QLabel*>(QStringLiteral("projectSetupNoticeText"));
    QToolButton* dismissOrNull = window.findChild<QToolButton*>(QStringLiteral("projectSetupNoticeDismiss"));
    QLabel* statusOrNull = window.findChild<QLabel*>(QStringLiteral("workspaceStatus"));
    REQUIRE(noticeOrNull != nullptr);
    REQUIRE(textOrNull != nullptr);
    REQUIRE(dismissOrNull != nullptr);
    REQUIRE(statusOrNull != nullptr);
    controller.ReportProjectSetupProblem(QStringLiteral("Rendering tools need preparation.\nslangc was not found."));
    Settle();
    CHECK(noticeOrNull->isVisible() == true);
    CHECK(textOrNull->text().contains(QLatin1Char('\n')) == false);
    CHECK(textOrNull->text().contains(QStringLiteral("Rendering tools need preparation.")) == true);
    CHECK(textOrNull->toolTip().contains(QStringLiteral("slangc was not found.")) == true);
    CHECK(noticeOrNull->height() <=
          2 * window.findChild<QPushButton*>(QStringLiteral("projectSetupNoticeRepair"))->sizeHint().height());
    CHECK(statusOrNull->text().contains(QLatin1Char('\n')) == false);
    CHECK(statusOrNull->text().count(QStringLiteral("Rendering tools need preparation.")) == 1);

    dismissOrNull->click();
    CHECK(noticeOrNull->isVisible() == false);
    CHECK(Action(window, QStringLiteral("checkSetupAction"))->isVisible() == true);
    CHECK(statusOrNull->text().contains(QStringLiteral("Rendering tools need preparation.")) == true);
    // The same problem stays dismissed; a different one is shown again.
    controller.ReportProjectSetupProblem(QStringLiteral("Rendering tools need preparation.\nslangc was not found."));
    Settle();
    CHECK(noticeOrNull->isVisible() == false);
    controller.ReportProjectSetupProblem(QStringLiteral("SDK or tool inputs changed; run repair"));
    Settle();
    CHECK(noticeOrNull->isVisible() == true);
}

TEST_CASE("Remove Unused Shaders is available in Edit and on the canvas with Properties hidden",
          "[editor][render-graph][shader-cleanup][shell]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    const QString descriptor = WriteProject(directory);
    const QString manifestPath = QDir(directory.path()).filePath(QStringLiteral("renderer/main.lushader.json"));
    QJsonObject manifest = QJsonDocument::fromJson(ReadBytes(manifestPath)).object();
    QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    QJsonObject unused = programs.first().toObject();
    unused.insert(QStringLiteral("id"), QStringLiteral("UnusedProgram"));
    unused.insert(QStringLiteral("source"), QStringLiteral("unused_exclusive.slang"));
    programs.append(unused);
    manifest.insert(QStringLiteral("programs"), programs);
    WriteBytes(manifestPath, EncodeCanonicalJson(manifest));
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("renderer/unused_exclusive.slang"));
    WriteBytes(sourcePath, QByteArrayLiteral("// authored source\n"));
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    QAction* cleanup = Action(window, QStringLiteral("renderGraph.removeUnusedShaders"));
    CHECK(cleanup->isEnabled() == false);
    controller.OpenProject(descriptor);
    REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->OpenGraph(QDir(directory.path()).filePath(QStringLiteral("renderer/main.lurender.json"))) ==
            true);
    renderer->SetLivePreview(false);
    static_cast<void>(Well(window).OpenDocument(renderer));
    window.show();
    Dock(window, QStringLiteral("propertiesDock"))->hide();
    Settle();
    REQUIRE(cleanup->isEnabled() == true);
    bool hasMenuCommand = false;
    for (QAction* menuAction : window.menuBar()->actions())
    {
        if (menuAction->menu() != nullptr && menuAction->menu()->title() == QStringLiteral("&Edit"))
        {
            hasMenuCommand = menuAction->menu()->actions().contains(cleanup);
        }
    }
    CHECK(hasMenuCommand == true);
    const QByteArray original = renderer->GetDocument().GetDraft(QStringLiteral("main.lushader.json"));
    SECTION("Edit command")
    {
        cleanup->trigger();
    }
    SECTION("canvas context menu")
    {
        bool hasCanvasCommand = false;
        QTimer::singleShot(0, &window, [&hasCanvasCommand]() {
            QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menuOrNull == nullptr)
            {
                return;
            }
            QAction* cleanupOrNull =
                menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphRemoveUnusedShadersAction"));
            if (cleanupOrNull != nullptr)
            {
                hasCanvasCommand = cleanupOrNull->isEnabled();
                cleanupOrNull->trigger();
            }
            menuOrNull->close();
        });
        RenderGraphCanvas& canvas = renderer->GetCanvas();
        const QPoint position = canvas.viewport()->rect().center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
        QCoreApplication::sendEvent(canvas.viewport(), &event);
        CHECK(hasCanvasCommand == true);
    }
    CHECK(renderer->GetDocument().GetDraft(QStringLiteral("main.lushader.json")).contains("UnusedProgram") == false);
    CHECK(renderer->GetDocument().GetFiles().contains(QStringLiteral("unused_exclusive.slang")) == false);
    CHECK(QFileInfo::exists(sourcePath) == true);
    renderer->Undo();
    CHECK(renderer->GetDocument().GetDraft(QStringLiteral("main.lushader.json")) == original);
    CHECK(renderer->GetDocument().GetFiles().contains(QStringLiteral("unused_exclusive.slang")) == true);
    renderer->Redo();
    REQUIRE(renderer->Save() == true);
    CHECK(QFileInfo::exists(sourcePath) == false);
}

TEST_CASE("Project Settings saves rendering edits and routes their undo to the graph", "[editor][shell][resolution]")
{
    QTemporaryDir directory;
    QTemporaryDir settings;
    EditorController controller(ToolingPaths{}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    window.show();
    window.activateWindow();
    const QString descriptor = WriteProject(directory);
    const QByteArray descriptorBefore = ReadBytes(descriptor);
    controller.OpenProject(descriptor);
    Settle();
    RendererWorkspace* renderer = window.findChild<RendererWorkspace*>();
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->OpenGraph(QDir(directory.path()).filePath(QStringLiteral("renderer/main.lurender.json"))) ==
            true);
    renderer->SetLivePreview(false);
    REQUIRE(Well(window).OpenDocument(Document(window, QStringLiteral("projectSettingsScroll"))) == true);
    Dock(window, QStringLiteral("propertiesDock"))->hide();
    QPushButton* enable = window.findChild<QPushButton*>(QStringLiteral("projectEnableResolution"));
    QSpinBox* width = window.findChild<QSpinBox*>(QStringLiteral("projectOutputWidth"));
    QPushButton* save = window.findChild<QPushButton*>(QStringLiteral("projectSaveSettings"));
    REQUIRE(enable != nullptr);
    REQUIRE(width != nullptr);
    REQUIRE(save != nullptr);
    enable->click();
    width->setValue(1920);
    width->setFocus();
    Settle();
    QAction* undo = Action(window, QStringLiteral("document.undo"));
    REQUIRE(undo->isEnabled() == true);
    undo->trigger();
    CHECK(width->value() == 96);
    QAction* redo = Action(window, QStringLiteral("document.redo"));
    REQUIRE(redo->isEnabled() == true);
    redo->trigger();
    CHECK(width->value() == 1920);
    REQUIRE(save->isEnabled() == true);
    // Keyboard Save must commit a spin box's pending text even without focus loss.
    QLineEdit* input = width->findChild<QLineEdit*>();
    REQUIRE(input != nullptr);
    input->setText(QStringLiteral("2560"));
    Action(window, QStringLiteral("document.save"))->trigger();
    CHECK(renderer->IsDirty() == false);
    CHECK(width->value() == 2560);
    CHECK(ReadBytes(descriptor) == descriptorBefore);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(QDir(directory.path()).filePath(QStringLiteral("renderer")),
                          QStringLiteral("main.lurender.json")) == RenderGraphDocumentStatus::Ok);
    QJsonObject graph;
    REQUIRE(reopened.GetGraph(QStringLiteral("main.lurender.json"), graph) == true);
    CHECK(QJsonDocument(graph).toJson().contains("2560") == true);
    REQUIRE(controller.CloseProject() == true);
}
