#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPoint>
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
    const QStringList kinds{QStringLiteral("Scene"),
                            QStringLiteral("Camera"),
                            QStringLiteral("Target"),
                            QStringLiteral("Raster"),
                            QStringLiteral("Subgraph"),
                            QStringLiteral("Export"),
                            QStringLiteral("Present")};
    for (const QString& kind : kinds)
    {
        // A file row still resolves to its displayed default configuration.
        static_cast<void>(workspace.SelectNode(library, {}, {}));
        const QByteArray before = workspace.GetDocument().GetDraft(library);
        QTimer::singleShot(0, &workspace, [kind, kinds]() {
            QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            REQUIRE(menuOrNull != nullptr);
            QMenu* createOrNull = menuOrNull->findChild<QMenu*>(QStringLiteral("renderGraphCreateNodeMenu"));
            REQUIRE(createOrNull != nullptr);
            REQUIRE(createOrNull->isEnabled() == true);
            CHECK(createOrNull->actions().size() == kinds.size());
            QAction* selectedOrNull = nullptr;
            for (QAction* action : createOrNull->actions())
            {
                if (action->data().toString() == kind)
                {
                    selectedOrNull = action;
                }
            }
            REQUIRE(selectedOrNull != nullptr);
            CHECK(selectedOrNull->isEnabled() == true);
            if (kind == QStringLiteral("Subgraph"))
            {
                CHECK(selectedOrNull->text() == QStringLiteral("Subgraph Reference"));
            }
            menuOrNull->close();
            selectedOrNull->trigger();
        });
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
            const QPoint position(1, 1);
            QContextMenuEvent context(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
            QCoreApplication::sendEvent(canvas.viewport(), &context);
        }
        const QString identity = kind.toLower();
        CHECK(workspace.GetSelection() == library + QStringLiteral("#Default/") + identity);
        QJsonObject graph;
        REQUIRE(workspace.GetDocument().GetGraph(library, graph) == true);
        CHECK(graph.value(QStringLiteral("configurations"))
                  .toArray()
                  .at(0)
                  .toObject()
                  .value(QStringLiteral("nodes"))
                  .toArray()
                  .last()
                  .toObject()
                  .value(QStringLiteral("kind"))
                  .toString() == kind);
        CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == parent);
        const QByteArray after = workspace.GetDocument().GetDraft(library);
        workspace.Undo();
        CHECK(workspace.GetDocument().GetDraft(library) == before);
        workspace.Redo();
        CHECK(workspace.GetDocument().GetDraft(library) == after);
    }
    CHECK(QFileInfo::exists(QDir(package).filePath(library)) == false);
}

TEST_CASE("Create Node menu cancellation and pending text preserve the draft", "[editor][render-graph][node-menu]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    static_cast<void>(workspace.SelectNode(ROOT_GRAPH, QStringLiteral("Default"), {}));
    bool hasPendingText = false;
    SECTION("cancel menu") {}
    SECTION("pending Source text")
    {
        hasPendingText = true;
        QPlainTextEdit* sourceOrNull = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
        REQUIRE(sourceOrNull != nullptr);
        sourceOrNull->appendPlainText(QStringLiteral(" "));
    }
    const QByteArray before = workspace.GetDocument().GetDraft(ROOT_GRAPH);
    const uint64 revision = workspace.GetDocument().GetRevision();
    QTimer::singleShot(0, &workspace, [hasPendingText]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QMenu* createOrNull = menuOrNull->findChild<QMenu*>(QStringLiteral("renderGraphCreateNodeMenu"));
        REQUIRE(createOrNull != nullptr);
        CHECK(createOrNull->isEnabled() == (hasPendingText == false));
        menuOrNull->close();
    });
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPoint position(1, 1);
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, canvas.viewport()->mapToGlobal(position));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    CHECK(workspace.GetDocument().GetDraft(ROOT_GRAPH) == before);
    CHECK(workspace.GetDocument().GetRevision() == revision);
}
