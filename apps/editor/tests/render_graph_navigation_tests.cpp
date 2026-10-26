#include "internal/render_graph_canvas.h"
#include "internal/render_graph_navigation.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
QString GetRootFile()
{
    return QStringLiteral("main.lurender.json");
}
QString GetConfiguration()
{
    return QStringLiteral("Default");
}
QString MakePackage(const QTemporaryDir& project)
{
    const QByteArray source = QByteArrayLiteral(R"JSON({"version":2,"id":"Main","default":"Default","configurations":[{
      "id":"Default","inputs":[],"outputs":[{"id":"display","type":"ColorTexture","source":"tone.color","required":false}],
      "nodes":[
        {"id":"texture","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","extent":[96,64],"samples":1,"imported":false,"initialized":false,"presentation":false},
        {"id":"tone","kind":"ToneMap","exposure":1},
        {"id":"output","kind":"Export","type":"ColorTexture"},
        {"id":"branch","kind":"Export","type":"ColorTexture","enabled":false},
        {"id":"unused","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","extent":[96,64],"samples":1,"imported":false,"initialized":false,"presentation":false}],
      "edges":[{"from":"texture.texture","to":"tone.color"},{"from":"tone.color","to":"output.texture"},{"from":"texture.texture","to":"branch.texture"}]}]})JSON");
    QFile file(QDir(project.path()).filePath(GetRootFile()));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    const QByteArray bytes = EncodeCanonicalJson(QJsonDocument::fromJson(source).object());
    REQUIRE(file.write(bytes) == bytes.size());
    return project.path();
}
QAction& FindAction(RenderGraphCanvas& inoutCanvas, const QString& name)
{
    QAction* actionOrNull = inoutCanvas.findChild<QAction*>(name);
    REQUIRE(actionOrNull != nullptr);
    return *actionOrNull;
}
QGraphicsRectItem& FindFrame(const RenderGraphCanvas& canvas, const QString& identity)
{
    QGraphicsRectItem* resultOrNull = nullptr;
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        QGraphicsRectItem* frameOrNull = qgraphicsitem_cast<QGraphicsRectItem*>(item);
        if (frameOrNull != nullptr && frameOrNull->data(0).toString() == identity)
        {
            resultOrNull = frameOrNull;
            break;
        }
    }
    REQUIRE(resultOrNull != nullptr);
    return *resultOrNull;
}
QGraphicsPathItem& FindEdge(const RenderGraphCanvas& canvas, const QString& source)
{
    QGraphicsPathItem* resultOrNull = nullptr;
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        QGraphicsPathItem* edgeOrNull = qgraphicsitem_cast<QGraphicsPathItem*>(item);
        if (edgeOrNull != nullptr && edgeOrNull->toolTip().startsWith(source) == true)
        {
            resultOrNull = edgeOrNull;
            break;
        }
    }
    REQUIRE(resultOrNull != nullptr);
    return *resultOrNull;
}
void ShowCanvas(RenderGraphCanvas& inoutCanvas)
{
    inoutCanvas.resize(1000, 700);
    inoutCanvas.ShowConfiguration(GetRootFile(), GetConfiguration());
    inoutCanvas.show();
    inoutCanvas.activateWindow();
    QCoreApplication::processEvents();
}
} // namespace

TEST_CASE("dependency tracing follows directed transitive paths, interfaces and cycles",
          "[editor][render-graph][navigation]")
{
    QWidget canvas;
    QToolBar toolbar(&canvas);
    RenderGraphNavigation navigation(toolbar, canvas);
    navigation.SetGraph({{QStringLiteral("input"), {}, false},
                         {QStringLiteral("a"), {}, true},
                         {QStringLiteral("b"), {}, true},
                         {QStringLiteral("c"), {}, true},
                         {QStringLiteral("d"), {}, true},
                         {QStringLiteral("#outputs"), {}, false},
                         {QStringLiteral("other"), {}, true},
                         {QStringLiteral("isolated"), {}, true}},
                        {{QStringLiteral("input"), QStringLiteral("a")},
                         {QStringLiteral("a"), QStringLiteral("b")},
                         {QStringLiteral("a"), QStringLiteral("c")},
                         {QStringLiteral("b"), QStringLiteral("d")},
                         {QStringLiteral("c"), QStringLiteral("d")},
                         {QStringLiteral("d"), QStringLiteral("#outputs")},
                         {QStringLiteral("b"), QStringLiteral("c")},
                         {QStringLiteral("c"), QStringLiteral("b")},
                         {QStringLiteral("missing"), QStringLiteral("a")},
                         {QStringLiteral("a"), QStringLiteral("b")}});
    navigation.SetSelection({QStringLiteral("d")});
    navigation.SetTraceDirection(RenderGraphTraceDirection::Upstream);
    CHECK(navigation.GetTrace().Nodes == QSet<QString>{QStringLiteral("input"),
                                                       QStringLiteral("a"),
                                                       QStringLiteral("b"),
                                                       QStringLiteral("c"),
                                                       QStringLiteral("d")});
    CHECK(navigation.GetTrace().Edges == QSet<qsizetype>{0, 1, 2, 3, 4, 6, 7, 9});
    navigation.SetSelection({QStringLiteral("a")});
    navigation.SetTraceDirection(RenderGraphTraceDirection::Downstream);
    CHECK(navigation.GetTrace().Nodes == QSet<QString>{QStringLiteral("a"),
                                                       QStringLiteral("b"),
                                                       QStringLiteral("c"),
                                                       QStringLiteral("d"),
                                                       QStringLiteral("#outputs")});
    CHECK(navigation.GetTrace().Edges == QSet<qsizetype>{1, 2, 3, 4, 5, 6, 7, 9});
    navigation.SetSelection({QStringLiteral("d"), QStringLiteral("isolated")});
    navigation.SetTraceDirection(RenderGraphTraceDirection::Upstream);
    CHECK(navigation.GetTrace().Nodes.contains(QStringLiteral("isolated")) == true);
    CHECK(navigation.GetTrace().Nodes.contains(QStringLiteral("other")) == false);
    navigation.SetSelection({QStringLiteral("input"), QStringLiteral("missing")});
    CHECK(navigation.GetTraceDirection() == RenderGraphTraceDirection::None);
    CHECK(navigation.GetTrace().Nodes.isEmpty() == true);
    navigation.SetTraceDirection(RenderGraphTraceDirection::Downstream);
    CHECK(navigation.GetTraceDirection() == RenderGraphTraceDirection::None);
}

TEST_CASE("dependency tracing handles long chains without recursion", "[editor][render-graph][navigation]")
{
    QWidget canvas;
    QToolBar toolbar(&canvas);
    RenderGraphNavigation navigation(toolbar, canvas);
    QVector<RenderGraphNavigationNode> nodes;
    QVector<RenderGraphNavigationEdge> edges;
    constexpr qsizetype NODE_COUNT = 4096;
    for (qsizetype index = 0; index < NODE_COUNT; ++index)
    {
        nodes.append({QString::number(index), {}, true});
        if (index > 0)
        {
            edges.append({QString::number(index - 1), QString::number(index)});
        }
    }
    navigation.SetGraph(nodes, edges);
    navigation.SetSelection({QString::number(NODE_COUNT - 1)});
    navigation.SetTraceDirection(RenderGraphTraceDirection::Upstream);
    CHECK(navigation.GetTrace().Nodes.size() == NODE_COUNT);
    CHECK(navigation.GetTrace().Edges.size() == NODE_COUNT - 1);
    navigation.SetGraph({}, {});
    CHECK(navigation.GetTraceDirection() == RenderGraphTraceDirection::None);
    CHECK(navigation.GetTrace().Nodes.isEmpty() == true);
}

TEST_CASE("Find Node ranks existing identities and kinds and cancels without changing the view",
          "[editor][render-graph][navigation]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    canvas.SelectNode(QStringLiteral("texture"));
    const QStringList selected = canvas.GetSelectedNodes();
    const QTransform transform = canvas.viewportTransform();
    const QByteArray draft = document.GetDraft(GetRootFile());
    QSignalSpy requests(&canvas, &RenderGraphCanvas::NodesSelected);
    FindAction(canvas, QStringLiteral("renderGraphFindNode")).trigger();
    QFrame* popup = canvas.findChild<QFrame*>(QStringLiteral("renderGraphFindPopup"));
    QLineEdit* search = canvas.findChild<QLineEdit*>(QStringLiteral("renderGraphFindQuery"));
    QListWidget* results = canvas.findChild<QListWidget*>(QStringLiteral("renderGraphFindResults"));
    REQUIRE(popup != nullptr);
    REQUIRE(search != nullptr);
    REQUIRE(results != nullptr);
    CHECK(popup->isVisible() == true);
    CHECK(results->count() == 5);
    // Interface boxes cannot be authored-node selections, so they are not search results.
    search->setText(QStringLiteral("t"));
    CHECK(results->item(0)->data(Qt::UserRole).toString() == QStringLiteral("texture"));
    search->setText(QStringLiteral("TaRgEt unused"));
    REQUIRE(results->count() == 1);
    CHECK(results->item(0)->data(Qt::UserRole).toString() == QStringLiteral("unused"));
    search->setText(QStringLiteral("Export"));
    REQUIRE(results->count() == 2);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_NAVIGATION_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(capture) == true);
        REQUIRE(popup->grab().save(QDir(capture).filePath(QStringLiteral("find-node.png"))) == true);
    }
    QTest::keyClick(search, Qt::Key_Down);
    CHECK(results->currentRow() == 1);
    QTest::keyClick(search, Qt::Key_Up);
    CHECK(results->currentRow() == 0);
    search->setText(QStringLiteral("no matches"));
    CHECK(results->count() == 0);
    QTest::keyClick(search, Qt::Key_Return);
    CHECK(popup->isVisible() == true);
    CHECK(requests.count() == 0);
    search->clear();
    QTest::keyClicks(search, QStringLiteral("f"));
    QTest::keyClick(search, Qt::Key_Home);
    CHECK(search->text() == QStringLiteral("f"));
    CHECK(canvas.viewportTransform() == transform);
    search->setText(QStringLiteral("output"));
    QTest::keyClick(search, Qt::Key_Escape);
    CHECK(popup->isVisible() == false);
    // Model a confirmation already queued when Escape closed the popup.
    QTest::keyClick(search, Qt::Key_Return);
    CHECK(requests.count() == 0);
    CHECK(canvas.GetSelectedNodes() == selected);
    CHECK(canvas.viewportTransform() == transform);
    CHECK(document.GetDraft(GetRootFile()) == draft);
    CHECK(document.GetRevision() == 0);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("Find Node confirms through keyboard or list and synchronizes the workspace selection",
          "[editor][render-graph][navigation]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(GetRootFile())) == true);
    workspace.SetLivePreview(false);
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    workspace.resize(1100, 750);
    workspace.show();
    workspace.activateWindow();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QCoreApplication::processEvents();
    canvas.setFocus();
    QTest::keySequence(&canvas, QKeySequence::Find);
    QLineEdit* search = canvas.findChild<QLineEdit*>(QStringLiteral("renderGraphFindQuery"));
    QListWidget* results = canvas.findChild<QListWidget*>(QStringLiteral("renderGraphFindResults"));
    REQUIRE(search != nullptr);
    REQUIRE(results != nullptr);
    search->setText(QStringLiteral("output"));
    REQUIRE(results->count() == 1);
    QSignalSpy selected(&canvas, &RenderGraphCanvas::NodesSelected);
    SECTION("Enter confirms from the query")
    {
        QTest::keyClick(search, Qt::Key_Return);
    }
    SECTION("double click confirms from the list")
    {
        const QPoint point = results->visualItemRect(results->item(0)).center();
        QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        CHECK(selected.count() == 0);
        QTest::mouseDClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    }
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("output")});
    CHECK(selected.count() == 1);
    CHECK(canvas.mapToScene(canvas.viewport()->rect())
              .boundingRect()
              .contains(FindFrame(canvas, QStringLiteral("output")).sceneBoundingRect()) == true);
    QTreeWidget* sources = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
    REQUIRE(sources != nullptr);
    REQUIRE(sources->selectedItems().size() == 1);
    CHECK(sources->selectedItems().first()->text(0).startsWith(QStringLiteral("output")) == true);
    CHECK(propertiesOrNull->isVisible() == false);
    CHECK(workspace.GetDocument().GetRevision() == 0);
    CHECK(workspace.GetDocument().CanUndo() == false);
}

TEST_CASE("dependency trace styling survives rebuilds and Escape restores normal selection styling",
          "[editor][render-graph][navigation]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    RenderGraphNavigation* navigation = canvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigation != nullptr);
    canvas.SelectNode(QStringLiteral("output"));
    const QPointF position = canvas.GetNodePosition(QStringLiteral("tone"));
    const QPointF port = canvas.GetPortPosition(QStringLiteral("tone.color"), true);
    const QByteArray draft = document.GetDraft(GetRootFile());
    const float64 inactiveOpacity = FindFrame(canvas, QStringLiteral("unused")).opacity();
    const float64 disabledOpacity = FindFrame(canvas, QStringLiteral("branch")).opacity();
    FindAction(canvas, QStringLiteral("renderGraphTraceUpstream")).trigger();
    CHECK(navigation->GetTrace().Nodes ==
          QSet<QString>{QStringLiteral("texture"), QStringLiteral("tone"), QStringLiteral("output")});
    CHECK(FindEdge(canvas, QStringLiteral("texture.texture → tone.color")).opacity() == 1);
    CHECK(FindEdge(canvas, QStringLiteral("texture.texture → branch.texture")).opacity() < 1);
    CHECK(FindFrame(canvas, QStringLiteral("unused")).opacity() < inactiveOpacity);
    navigation->OpenFindNode();
    QLineEdit* queryOrNull = canvas.findChild<QLineEdit*>(QStringLiteral("renderGraphFindQuery"));
    REQUIRE(queryOrNull != nullptr);
    QTest::keyClick(queryOrNull, Qt::Key_Escape);
    CHECK(navigation->GetTraceDirection() == RenderGraphTraceDirection::Upstream);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("output")});
    // Hover never overrides an explicit trace, including a hover on another branch.
    QTest::mouseMove(canvas.viewport(),
                     canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("branch.texture"), false)));
    CHECK(navigation->GetTrace().Nodes.contains(QStringLiteral("branch")) == false);
    canvas.Rebuild();
    CHECK(navigation->GetTraceDirection() == RenderGraphTraceDirection::Upstream);
    CHECK(navigation->GetTrace().Nodes.size() == 3);
    canvas.SetReferenceSize(QSize(1280, 720));
    CHECK(FindFrame(canvas, QStringLiteral("unused")).opacity() < inactiveOpacity);
    canvas.SelectNode(QStringLiteral("texture"));
    FindAction(canvas, QStringLiteral("renderGraphTraceDownstream")).trigger();
    CHECK(navigation->GetTrace().Nodes.contains(QStringLiteral("#outputs")) == true);
    CHECK(navigation->GetTrace().Nodes.contains(QStringLiteral("branch")) == true);
    CHECK(FindFrame(canvas, QStringLiteral("branch")).opacity() == disabledOpacity);
    CHECK(FindEdge(canvas, QStringLiteral("tone.color → #outputs.display")).opacity() == 1);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_NAVIGATION_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(capture) == true);
        REQUIRE(canvas.grab().save(QDir(capture).filePath(QStringLiteral("downstream.png"))) == true);
    }
    QTest::keyClick(&canvas, Qt::Key_Escape);
    CHECK(navigation->GetTraceDirection() == RenderGraphTraceDirection::None);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("texture")});
    CHECK(FindFrame(canvas, QStringLiteral("unused")).opacity() == inactiveOpacity);
    CHECK(FindAction(canvas, QStringLiteral("renderGraphClearTrace")).isEnabled() == false);
    CHECK(FindAction(canvas, QStringLiteral("renderGraphTraceUpstream")).isChecked() == false);
    CHECK(FindAction(canvas, QStringLiteral("renderGraphTraceDownstream")).isChecked() == false);
    CHECK(canvas.GetNodePosition(QStringLiteral("tone")) == position);
    CHECK(canvas.GetPortPosition(QStringLiteral("tone.color"), true) == port);
    CHECK(document.GetDraft(GetRootFile()) == draft);
    CHECK(document.GetRevision() == 0);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("Find Node and traces discard stale targets after edits and configuration switches",
          "[editor][render-graph][navigation]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    RenderGraphNavigation* navigation = canvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigation != nullptr);
    canvas.SelectNode(QStringLiteral("output"));
    navigation->SetTraceDirection(RenderGraphTraceDirection::Upstream);
    navigation->OpenFindNode();
    QFrame* popup = canvas.findChild<QFrame*>(QStringLiteral("renderGraphFindPopup"));
    REQUIRE(popup != nullptr);
    REQUIRE(document.RemoveNode(GetRootFile(), GetConfiguration(), QStringLiteral("output")) ==
            RenderGraphDocumentStatus::Ok);
    const uint64 revision = document.GetRevision();
    canvas.Rebuild();
    CHECK(popup->isVisible() == false);
    CHECK(navigation->GetTraceDirection() == RenderGraphTraceDirection::None);
    CHECK(FindAction(canvas, QStringLiteral("renderGraphTraceUpstream")).isEnabled() == false);
    QLineEdit* queryOrNull = canvas.findChild<QLineEdit*>(QStringLiteral("renderGraphFindQuery"));
    REQUIRE(queryOrNull != nullptr);
    QSignalSpy requests(&canvas, &RenderGraphCanvas::NodesSelected);
    QTest::keyClick(queryOrNull, Qt::Key_Return);
    CHECK(requests.count() == 0);
    CHECK(canvas.GetSelectedNodes().isEmpty() == true);
    canvas.SelectNode(QStringLiteral("tone"));
    navigation->SetTraceDirection(RenderGraphTraceDirection::Upstream);
    canvas.ShowConfiguration({}, {});
    CHECK(navigation->GetTraceDirection() == RenderGraphTraceDirection::None);
    CHECK(FindAction(canvas, QStringLiteral("renderGraphFindNode")).isEnabled() == false);
    CHECK(FindAction(canvas, QStringLiteral("renderGraphTraceDownstream")).isEnabled() == false);
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("Find Node shortcuts leave text editing outside the canvas alone", "[editor][render-graph][navigation]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    QWidget window;
    QVBoxLayout* layout = new QVBoxLayout(&window);
    QLineEdit* text = new QLineEdit(&window);
    RenderGraphCanvas* canvas = new RenderGraphCanvas(document, &window);
    layout->addWidget(text);
    layout->addWidget(canvas);
    canvas->ShowConfiguration(GetRootFile(), GetConfiguration());
    window.resize(900, 650);
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    text->setFocus();
    QTest::keySequence(text, QKeySequence::Find);
    QFrame* popup = canvas->findChild<QFrame*>(QStringLiteral("renderGraphFindPopup"));
    REQUIRE(popup != nullptr);
    CHECK(popup->isVisible() == false);
    CHECK(text->hasFocus() == true);
    canvas->setFocus();
    QTest::keySequence(canvas, QKeySequence::Find);
    CHECK(popup->isVisible() == true);
}

TEST_CASE("dependency traces recompute from current draft connections and undo", "[editor][render-graph][navigation]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    RenderGraphNavigation* navigation = canvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigation != nullptr);
    canvas.SelectNode(QStringLiteral("output"));
    navigation->SetTraceDirection(RenderGraphTraceDirection::Upstream);
    REQUIRE(navigation->GetTrace().Nodes.size() == 3);
    REQUIRE(document.Disconnect(GetRootFile(),
                                GetConfiguration(),
                                QStringLiteral("texture.texture"),
                                QStringLiteral("tone.color")) == RenderGraphDocumentStatus::Ok);
    const uint64 editedRevision = document.GetRevision();
    canvas.Rebuild();
    CHECK(navigation->GetTraceDirection() == RenderGraphTraceDirection::Upstream);
    CHECK(navigation->GetTrace().Nodes == QSet<QString>{QStringLiteral("tone"), QStringLiteral("output")});
    CHECK(document.GetRevision() == editedRevision);
    REQUIRE(document.Undo() == true);
    const uint64 undoneRevision = document.GetRevision();
    canvas.Rebuild();
    CHECK(navigation->GetTraceDirection() == RenderGraphTraceDirection::Upstream);
    CHECK(navigation->GetTrace().Nodes.size() == 3);
    CHECK(document.GetRevision() == undoneRevision);
    CHECK(document.CanUndo() == false);
}
