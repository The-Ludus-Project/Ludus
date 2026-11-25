#include "internal/render_graph_canvas.h"
#include "internal/render_graph_navigation.h"
#include "internal/renderer_workspace.h"
#include "mouse_fixture.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
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
        tests::DoubleClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, point);
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

TEST_CASE("wires of a hub card rest dimmed until it is focused", "[editor][render-graph][navigation][hub]")
{
    constexpr int HUB_WIRES = 6;
    QJsonArray nodes;
    QJsonArray edges;
    const auto addTarget = [&nodes](const QString& identity) {
        nodes.append(QJsonObject{{QStringLiteral("id"), identity},
                                 {QStringLiteral("kind"), QStringLiteral("Target")},
                                 {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                                 {QStringLiteral("format"), QStringLiteral("RGBA8Unorm")},
                                 {QStringLiteral("extent"), QJsonArray{96, 64}},
                                 {QStringLiteral("samples"), 1},
                                 {QStringLiteral("imported"), false},
                                 {QStringLiteral("initialized"), false},
                                 {QStringLiteral("presentation"), false}});
    };
    addTarget(QStringLiteral("hub"));
    addTarget(QStringLiteral("plain"));
    for (int index = 0; index < HUB_WIRES; ++index)
    {
        const QString exportName = QStringLiteral("export%1").arg(index);
        nodes.append(QJsonObject{{QStringLiteral("id"), exportName},
                                 {QStringLiteral("kind"), QStringLiteral("Export")},
                                 {QStringLiteral("type"), QStringLiteral("ColorTexture")}});
        edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("hub.texture")},
                                 {QStringLiteral("to"), exportName + QStringLiteral(".texture")}});
    }
    nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("lone")},
                             {QStringLiteral("kind"), QStringLiteral("Export")},
                             {QStringLiteral("type"), QStringLiteral("ColorTexture")}});
    edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("plain.texture")},
                             {QStringLiteral("to"), QStringLiteral("lone.texture")}});
    const QJsonObject graph{{QStringLiteral("version"), 2},
                            {QStringLiteral("id"), QStringLiteral("Main")},
                            {QStringLiteral("default"), QStringLiteral("Default")},
                            {QStringLiteral("configurations"),
                             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("Default")},
                                                    {QStringLiteral("inputs"), QJsonArray{}},
                                                    {QStringLiteral("outputs"), QJsonArray{}},
                                                    {QStringLiteral("nodes"), nodes},
                                                    {QStringLiteral("edges"), edges}}}}};
    QTemporaryDir project;
    QFile file(QDir(project.path()).filePath(GetRootFile()));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    const QByteArray bytes = EncodeCanonicalJson(graph);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    RenderGraphDocument document;
    REQUIRE(document.Load(project.path(), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    const float64 hubOpacity = FindEdge(canvas, QStringLiteral("hub.texture → export0.texture")).opacity();
    CHECK(hubOpacity < 1);
    CHECK(FindEdge(canvas, QStringLiteral("plain.texture → lone.texture")).opacity() == 1);
    // Selecting the hub brings its wires back to full strength.
    canvas.SelectNode(QStringLiteral("hub"));
    CHECK(FindEdge(canvas, QStringLiteral("hub.texture → export0.texture")).opacity() == 1);
}

namespace
{
/// A Target card with the defaults the canvas tests share.
QJsonObject MakeTarget(const QString& identity)
{
    return QJsonObject{{QStringLiteral("id"), identity},
                       {QStringLiteral("kind"), QStringLiteral("Target")},
                       {QStringLiteral("type"), QStringLiteral("ColorTexture")},
                       {QStringLiteral("format"), QStringLiteral("RGBA8Unorm")},
                       {QStringLiteral("extent"), QJsonArray{96, 64}},
                       {QStringLiteral("samples"), 1},
                       {QStringLiteral("imported"), false},
                       {QStringLiteral("initialized"), false},
                       {QStringLiteral("presentation"), false}};
}
QJsonObject MakeExport(const QString& identity)
{
    return QJsonObject{{QStringLiteral("id"), identity},
                       {QStringLiteral("kind"), QStringLiteral("Export")},
                       {QStringLiteral("type"), QStringLiteral("ColorTexture")}};
}
void WriteGraph(const QTemporaryDir& project, const QJsonArray& nodes, const QJsonArray& edges)
{
    const QJsonObject graph{{QStringLiteral("version"), 2},
                            {QStringLiteral("id"), QStringLiteral("Main")},
                            {QStringLiteral("default"), QStringLiteral("Default")},
                            {QStringLiteral("configurations"),
                             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("Default")},
                                                    {QStringLiteral("inputs"), QJsonArray{}},
                                                    {QStringLiteral("outputs"), QJsonArray{}},
                                                    {QStringLiteral("nodes"), nodes},
                                                    {QStringLiteral("edges"), edges}}}}};
    QFile file(QDir(project.path()).filePath(GetRootFile()));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    const QByteArray bytes = EncodeCanonicalJson(graph);
    REQUIRE(file.write(bytes) == bytes.size());
}
/// The title label of a card: the text item whose tooltip is the card's full "id : kind" title.
QGraphicsSimpleTextItem& FindTitle(const RenderGraphCanvas& canvas, const QString& identity)
{
    QGraphicsSimpleTextItem* resultOrNull = nullptr;
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        QGraphicsSimpleTextItem* textOrNull = qgraphicsitem_cast<QGraphicsSimpleTextItem*>(item);
        if (textOrNull != nullptr && textOrNull->toolTip().startsWith(identity + QStringLiteral(" : ")) == true)
        {
            resultOrNull = textOrNull;
            break;
        }
    }
    REQUIRE(resultOrNull != nullptr);
    return *resultOrNull;
}
} // namespace

TEST_CASE("a long card title stays on one line and reads from its start", "[editor][render-graph][navigation][title]")
{
    constexpr float64 OVERVIEW_ZOOM = 0.5;
    const QString identity = QStringLiteral("computeRayTracingAccumulatedHistory");
    QTemporaryDir project;
    WriteGraph(project,
               QJsonArray{MakeTarget(identity), MakeExport(QStringLiteral("output"))},
               QJsonArray{QJsonObject{{QStringLiteral("from"), identity + QStringLiteral(".texture")},
                                      {QStringLiteral("to"), QStringLiteral("output.texture")}}});
    RenderGraphDocument document;
    REQUIRE(document.Load(project.path(), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    REQUIRE(canvas.RestoreViewState({{}, canvas.GetNodePosition(identity), OVERVIEW_ZOOM, {}, {}}) == true);
    QCoreApplication::processEvents();
    const QString overview = FindTitle(canvas, identity).text();
    INFO(overview.toStdString());
    // One line, shortened only at its end ("computeRayTr…"): never wrapped into clipped word pieces, never shortened
    // from the middle.
    CHECK(overview.contains(QLatin1Char('\n')) == false);
    CHECK(overview.startsWith(QStringLiteral("compute")) == true);
    CHECK(overview.endsWith(QChar(0x2026)) == true);
    CHECK(identity.startsWith(overview.chopped(1)) == true);
    REQUIRE(canvas.RestoreViewState({{}, canvas.GetNodePosition(identity), 1, {}, {}}) == true);
    QCoreApplication::processEvents();
    const QString detailed = FindTitle(canvas, identity).text();
    INFO(detailed.toStdString());
    // The detailed header has one line; it keeps the start and shortens only the end.
    CHECK(detailed.contains(QLatin1Char('\n')) == false);
    CHECK(detailed.startsWith(QStringLiteral("computeRay")) == true);
}

TEST_CASE("a dense graph's resting wires recede and a focused card's wires stand out",
          "[editor][render-graph][navigation][wires]")
{
    constexpr int PAIRS = 40;
    QJsonArray nodes;
    QJsonArray edges;
    for (int index = 0; index < PAIRS; ++index)
    {
        nodes.append(MakeTarget(QStringLiteral("source%1").arg(index)));
        nodes.append(MakeExport(QStringLiteral("sink%1").arg(index)));
        edges.append(QJsonObject{{QStringLiteral("from"), QStringLiteral("source%1.texture").arg(index)},
                                 {QStringLiteral("to"), QStringLiteral("sink%1.texture").arg(index)}});
    }
    QTemporaryDir project;
    WriteGraph(project, nodes, edges);
    RenderGraphDocument document;
    REQUIRE(document.Load(project.path(), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    QGraphicsPathItem& focused = FindEdge(canvas, QStringLiteral("source0.texture → sink0.texture"));
    QGraphicsPathItem& other = FindEdge(canvas, QStringLiteral("source1.texture → sink1.texture"));
    const float64 resting = other.opacity();
    CHECK(resting < 1);
    CHECK(focused.opacity() == resting);
    canvas.SelectNode(QStringLiteral("source0"));
    CHECK(focused.opacity() == 1);
    CHECK(focused.pen().widthF() > other.pen().widthF());
    CHECK(other.opacity() < resting);
    canvas.SelectNode({});
    CHECK(other.opacity() == resting);
}

TEST_CASE("the canvas toolbar groups framing under Fit and tracing under Trace",
          "[editor][render-graph][navigation][toolbar]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakePackage(project), GetRootFile()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    QToolBar* toolbarOrNull = canvas.findChild<QToolBar*>(QStringLiteral("renderGraphFramingToolbar"));
    QToolButton* fitOrNull = canvas.findChild<QToolButton*>(QStringLiteral("renderGraphFit"));
    QToolButton* traceOrNull = canvas.findChild<QToolButton*>(QStringLiteral("renderGraphTrace"));
    REQUIRE(toolbarOrNull != nullptr);
    REQUIRE(fitOrNull != nullptr);
    REQUIRE(traceOrNull != nullptr);
    QAction& upstream = FindAction(canvas, QStringLiteral("renderGraphTraceUpstream"));
    QAction& downstream = FindAction(canvas, QStringLiteral("renderGraphTraceDownstream"));
    QAction& clear = FindAction(canvas, QStringLiteral("renderGraphClearTrace"));
    QAction& fitGraph = FindAction(canvas, QStringLiteral("renderGraphFitGraph"));
    REQUIRE(fitOrNull->menu() != nullptr);
    CHECK(fitOrNull->menu()->actions() ==
          QList<QAction*>{&fitGraph,
                          &FindAction(canvas, QStringLiteral("renderGraphFitSelection")),
                          &FindAction(canvas, QStringLiteral("renderGraphResetZoom"))});
    CHECK(fitOrNull->defaultAction() == &fitGraph);
    CHECK(fitGraph.text() == QStringLiteral("Fit Graph"));
    REQUIRE(traceOrNull->menu() != nullptr);
    CHECK(traceOrNull->menu()->actions().contains(&upstream) == true);
    CHECK(traceOrNull->menu()->actions().contains(&downstream) == true);
    CHECK(traceOrNull->menu()->actions().contains(&clear) == true);
    // The grouped commands are not also loose buttons.
    for (QAction* action : {&upstream, &downstream, &clear, &fitGraph})
    {
        CHECK(toolbarOrNull->actions().contains(action) == false);
    }
    CHECK(traceOrNull->isEnabled() == false);
    canvas.SelectNode(QStringLiteral("tone"));
    CHECK(traceOrNull->isEnabled() == true);
    CHECK(traceOrNull->text() == QStringLiteral("Trace"));
    upstream.trigger();
    CHECK(traceOrNull->text() == QStringLiteral("Trace: Upstream"));
    CHECK(traceOrNull->isChecked() == true);
    clear.trigger();
    CHECK(traceOrNull->text() == QStringLiteral("Trace"));
    CHECK(traceOrNull->isChecked() == false);
    // With no report, diagnostic stepping takes no room.
    CHECK(FindAction(canvas, QStringLiteral("renderGraphNextDiagnostic")).isVisible() == false);
    CHECK(FindAction(canvas, QStringLiteral("renderGraphPreviousDiagnostic")).isVisible() == false);
}

TEST_CASE("Properties summarizes the graph at rest and connects the selection through port pickers",
          "[editor][render-graph][navigation][properties]")
{
    QTemporaryDir project;
    const QString package = MakePackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(GetRootFile())) == true);
    workspace.SetLivePreview(false);
    QWidget side;
    QVBoxLayout* layout = new QVBoxLayout(&side);
    layout->addWidget(workspace.TakeInspectorPanel(&side));
    side.resize(300, 700);
    side.show();
    workspace.show();
    QCoreApplication::processEvents();
    QLabel* summaryOrNull = side.findChild<QLabel*>(QStringLiteral("renderGraphSummary"));
    QWidget* connectionsOrNull = side.findChild<QWidget*>(QStringLiteral("renderGraphConnections"));
    QWidget* nodeToolsOrNull = side.findChild<QWidget*>(QStringLiteral("renderGraphNodeTools"));
    QComboBox* fromOrNull = side.findChild<QComboBox*>(QStringLiteral("renderGraphConnectFrom"));
    QComboBox* toOrNull = side.findChild<QComboBox*>(QStringLiteral("renderGraphConnectTo"));
    QPushButton* connectOrNull = side.findChild<QPushButton*>(QStringLiteral("renderGraphConnect"));
    QTableWidget* edgesOrNull = side.findChild<QTableWidget*>(QStringLiteral("renderGraphEdges"));
    REQUIRE(summaryOrNull != nullptr);
    REQUIRE(connectionsOrNull != nullptr);
    REQUIRE(nodeToolsOrNull != nullptr);
    REQUIRE(fromOrNull != nullptr);
    REQUIRE(toOrNull != nullptr);
    REQUIRE(connectOrNull != nullptr);
    REQUIRE(edgesOrNull != nullptr);
    CHECK(summaryOrNull->isVisible() == true);
    CHECK(summaryOrNull->text().startsWith(QStringLiteral("5 nodes · 3 connections")) == true);
    CHECK(summaryOrNull->text().contains(QStringLiteral("Outputs: display")) == true);
    CHECK(summaryOrNull->text().contains(QStringLiteral("Exports: output, branch")) == true);
    CHECK(connectionsOrNull->isVisible() == false);
    CHECK(nodeToolsOrNull->isVisible() == false);
    REQUIRE(workspace.SelectNode(GetRootFile(), GetConfiguration(), QStringLiteral("unused")) == true);
    QCoreApplication::processEvents();
    CHECK(summaryOrNull->isVisible() == false);
    CHECK(connectionsOrNull->isVisible() == true);
    CHECK(nodeToolsOrNull->isVisible() == true);
    CHECK(edgesOrNull->rowCount() == 0);
    CHECK(fromOrNull->isEditable() == false);
    CHECK(toOrNull->isEditable() == false);
    // The selection's own ports lead; every entry is a real endpoint.
    CHECK(fromOrNull->currentText() == QStringLiteral("unused.texture"));
    CHECK(fromOrNull->findText(QStringLiteral("texture.texture")) >= 0);
    CHECK(toOrNull->findText(QStringLiteral("tone.color")) >= 0);
    CHECK(toOrNull->findText(QStringLiteral("#outputs.display")) >= 0);
    toOrNull->setCurrentIndex(toOrNull->findText(QStringLiteral("branch.texture")));
    connectOrNull->click();
    QCoreApplication::processEvents();
    REQUIRE(workspace.SelectNode(GetRootFile(), GetConfiguration(), QStringLiteral("unused")) == true);
    QCoreApplication::processEvents();
    REQUIRE(edgesOrNull->rowCount() == 1);
    CHECK(edgesOrNull->item(0, 0)->text() == QStringLiteral("unused.texture"));
    CHECK(edgesOrNull->item(0, 1)->text() == QStringLiteral("branch.texture"));
    // Without a preview failure there is nothing to point at in the Error List.
    CHECK(workspace.IsRenderProblemInErrorList() == false);
    QToolButton* chipOrNull = workspace.findChild<QToolButton*>(QStringLiteral("renderGraphProblemChip"));
    REQUIRE(chipOrNull != nullptr);
    CHECK(chipOrNull->isVisible() == false);
}
