#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPoint>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 CREATION_WORKSPACE_WIDTH = 1280;
constexpr int32 CREATION_WORKSPACE_HEIGHT = 800;
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");

QString MakePackage(const QTemporaryDir& project)
{
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    REQUIRE(QDir().mkpath(package) == true);
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& file : {ROOT_GRAPH, QStringLiteral("rasterize.lurender.json")})
    {
        REQUIRE(QFile::copy(source.filePath(file), QDir(package).filePath(file)) == true);
    }
    return package;
}

/// What the open Create Node submenu offered. Recorded inside the popup's event loop and
/// asserted after it returns, so a failure reports instead of throwing through Qt.
struct CreateMenuObservation
{
    bool HasMenu = false;
    bool IsEnabled = false;
    QStringList Kinds;
    bool HasSelection = false;
    bool IsSelectionEnabled = false;
    QString SelectionText;
};

/// Record the active popup's Create Node submenu, close the popup, then trigger `kind` when offered.
void ObserveCreateMenu(const QString& kind, CreateMenuObservation& outObservation)
{
    QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    if (menuOrNull == nullptr)
    {
        return;
    }
    QMenu* createOrNull = menuOrNull->findChild<QMenu*>(QStringLiteral("renderGraphCreateNodeMenu"));
    if (createOrNull == nullptr)
    {
        menuOrNull->close();
        return;
    }
    outObservation.HasMenu = true;
    outObservation.IsEnabled = createOrNull->isEnabled();
    QAction* selectedOrNull = nullptr;
    for (QAction* action : createOrNull->actions())
    {
        const QString offered = action->data().toString();
        outObservation.Kinds.append(offered);
        if (kind.isEmpty() == false && offered == kind)
        {
            selectedOrNull = action;
        }
    }
    menuOrNull->close();
    if (selectedOrNull != nullptr)
    {
        outObservation.HasSelection = true;
        outObservation.IsSelectionEnabled = selectedOrNull->isEnabled();
        outObservation.SelectionText = selectedOrNull->text();
        selectedOrNull->trigger();
    }
}
} // namespace

TEST_CASE("Create Node menus author every supported type inside an empty library", "[editor][render-graph][node-menu]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.resize(CREATION_WORKSPACE_WIDTH, CREATION_WORKSPACE_HEIGHT);
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, QStringLiteral("Default"), {}) == true);
    REQUIRE(workspace.CreateSubgraph(QStringLiteral("MenuLibrary")) == true);
    const QString library = QStringLiteral("MenuLibrary.lurender.json");
    const QByteArray parent = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    workspace.show();
    QCoreApplication::processEvents();
    bool shouldUseTree = false;
    SECTION("canvas background") {}
    SECTION("source tree file row")
    {
        shouldUseTree = true;
    }
    // The Inspector's node-type list owns the Create Node entries; author each one it offers.
    QComboBox* newKindOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphNewKind"));
    REQUIRE(newKindOrNull != nullptr);
    QStringList kinds;
    for (int32 index = 0; index < newKindOrNull->count(); ++index)
    {
        kinds.append(newKindOrNull->itemText(index));
    }
    REQUIRE(kinds.isEmpty() == false);
    CHECK(kinds.contains(QStringLiteral("Subgraph")) == true);
    for (const QString& kind : kinds)
    {
        INFO(kind.toStdString());
        INFO(shouldUseTree);
        // A file row still resolves to its displayed default configuration.
        static_cast<void>(workspace.SelectNode(library, {}, {}));
        const QByteArray before = workspace.GetDocument().GetDraft(library);
        CreateMenuObservation observation;
        QTimer::singleShot(0, &workspace, [&kind, &observation]() { ObserveCreateMenu(kind, observation); });
        if (shouldUseTree == true)
        {
            QTreeWidget* treeOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
            REQUIRE(treeOrNull != nullptr);
            const QList<QTreeWidgetItem*> files =
                treeOrNull->findItems(library + QStringLiteral(" *"), Qt::MatchExactly);
            REQUIRE(files.size() == 1);
            treeOrNull->scrollToItem(files.first());
            Q_EMIT treeOrNull->customContextMenuRequested(treeOrNull->visualItemRect(files.first()).center());
        }
        else
        {
            RenderGraphCanvas& canvas = workspace.GetCanvas();
            // Inspector widths and graph auto-fit can put a port at (1, 1). Pick
            // actual background beyond the authored items instead of a fixed pixel.
            const QPointF background = canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(100, 100);
            canvas.centerOn(background);
            const QPoint position = canvas.mapFromScene(background);
            REQUIRE(canvas.viewport()->rect().contains(position) == true);
            REQUIRE(canvas.itemAt(position) == nullptr);
            QContextMenuEvent context(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
            QCoreApplication::sendEvent(canvas.viewport(), &context);
        }
        REQUIRE(observation.HasMenu == true);
        CHECK(observation.IsEnabled == true);
        CHECK(observation.Kinds == kinds);
        REQUIRE(observation.HasSelection == true);
        CHECK(observation.IsSelectionEnabled == true);
        if (kind == QStringLiteral("Subgraph"))
        {
            CHECK(observation.SelectionText == QStringLiteral("Subgraph Reference"));
        }
        const QString algorithm = newKindOrNull->itemData(newKindOrNull->findText(kind)).toString();
        const bool isUpscalePreset = algorithm.isEmpty() == false;
        const QString identity = isUpscalePreset == true ? algorithm : kind.toLower();
        const QString expectedKind = isUpscalePreset == true ? QStringLiteral("Upscale") : kind;
        CHECK(workspace.GetSelection() == library + QStringLiteral("#Default/") + identity);
        QJsonObject graph;
        REQUIRE(workspace.GetDocument().GetGraph(library, graph) == true);
        const QJsonObject node = graph.value(QStringLiteral("configurations"))
                                     .toArray()
                                     .at(0)
                                     .toObject()
                                     .value(QStringLiteral("nodes"))
                                     .toArray()
                                     .last()
                                     .toObject();
        CHECK(node.value(QStringLiteral("kind")).toString() == expectedKind);
        if (isUpscalePreset == true)
        {
            CHECK(node.value(QStringLiteral("algorithm")).toString() == algorithm);
        }
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == parent);
        const QByteArray after = workspace.GetDocument().GetDraft(library);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(library) == before);
        workspace.Redo();
        CHECK(workspace.GetDocument().GetDraft(library) == after);
    }
    CHECK(QFileInfo::exists(QDir(package).filePath(library)) == false);
}

TEST_CASE("Create Node menu cancellation preserves the current text draft", "[editor][render-graph][node-menu]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    static_cast<void>(workspace.SelectNode(ROOT_GRAPH, QStringLiteral("Default"), {}));
    SECTION("cancel menu") {}
    SECTION("accepted Source edit")
    {
        QPlainTextEdit* sourceOrNull = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
        REQUIRE(sourceOrNull != nullptr);
        sourceOrNull->appendPlainText(QStringLiteral(" "));
    }
    const QByteArray before = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    const uint64 revision = workspace.GetDocument().GetRevision();
    CreateMenuObservation observation;
    QTimer::singleShot(0, &workspace, [&observation]() { ObserveCreateMenu({}, observation); });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPoint position(1, 1);
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(observation.HasMenu == true);
    CHECK(observation.IsEnabled == true);
    CHECK(observation.HasSelection == false);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == before);
    CHECK(workspace.GetDocument().GetRevision() == revision);
}
