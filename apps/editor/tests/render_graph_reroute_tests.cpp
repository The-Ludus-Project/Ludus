#include "internal/render_graph_live_build.h"
#include "internal/render_graph_navigation.h"
#include "internal/render_graph_reroutes.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFocusEvent>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QMenu>
#include <QPen>
#include <QPixmap>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
QString GetRoot()
{
    return QStringLiteral("main.lurender.json");
}
QString GetDefault()
{
    return QStringLiteral("Default");
}
QString GetFrom()
{
    return QStringLiteral("first.color");
}
QString GetTo()
{
    return QStringLiteral("second.color");
}
QJsonObject MakeRoute(const QString& from = GetFrom(), const QString& to = GetTo())
{
    return {
        {QStringLiteral("from"), from},
        {QStringLiteral("to"), to},
        {QStringLiteral("points"), QJsonArray{QJsonObject{{QStringLiteral("x"), 300}, {QStringLiteral("y"), -150}}}}};
}
QJsonObject GetConfiguration(const RenderGraphDocument& document)
{
    return QJsonDocument::fromJson(document.GetDraft(GetRoot()))
        .object()
        .value(QStringLiteral("configurations"))
        .toArray()
        .first()
        .toObject();
}
QJsonArray GetRoutes(const RenderGraphDocument& document)
{
    return GetConfiguration(document)
        .value(QStringLiteral("layout"))
        .toObject()
        .value(QStringLiteral("routes"))
        .toArray();
}
void OpenWorkspace(RendererWorkspace& inoutWorkspace, const QTemporaryDir& project)
{
    const QByteArray bytes = QByteArrayLiteral(R"JSON({"version":2,"id":"Main","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"third.color","required":false}],
      "nodes":[{"id":"first","kind":"ToneMap","exposure":1},
               {"id":"second","kind":"ToneMap","exposure":1},{"id":"third","kind":"ToneMap","exposure":1}],
      "edges":[{"from":"input.color","to":"first.color"},{"from":"first.color","to":"second.color"},
               {"from":"second.color","to":"third.color"}],
      "layout":{"positions":[{"node":"first","x":0,"y":0},{"node":"second","x":400,"y":0},
                                 {"node":"third","x":850,"y":300}]}}]})JSON");
    QFile file(QDir(project.path()).filePath(GetRoot()));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    inoutWorkspace.SetProject(project.path(), 1);
    REQUIRE(inoutWorkspace.OpenGraph(file.fileName()) == true);
    inoutWorkspace.SetLivePreview(false);
    inoutWorkspace.resize(1200, 850);
    inoutWorkspace.show();
    QCoreApplication::processEvents();
    inoutWorkspace.SetChromeVisible(false);
}
QGraphicsPathItem& GetWire(RenderGraphCanvas& inoutCanvas)
{
    for (QGraphicsItem* item : inoutCanvas.scene()->items())
    {
        QGraphicsPathItem* pathOrNull = qgraphicsitem_cast<QGraphicsPathItem*>(item);
        if (pathOrNull != nullptr &&
            pathOrNull->toolTip().startsWith(GetFrom() + QStringLiteral(" → ") + GetTo()) == true)
        {
            return *pathOrNull;
        }
    }
    FAIL("missing authored wire");
    Q_UNREACHABLE();
}
void SelectTrace(RenderGraphCanvas& inoutCanvas)
{
    REQUIRE(inoutCanvas.RestoreViewState(
                {{QStringLiteral("first")}, QPointF(400, 80), 1, RenderGraphTraceDirection::Downstream, {}}) == true);
}
void CheckTrace(RenderGraphCanvas& inoutCanvas)
{
    CHECK(inoutCanvas.GetSelectedNodes() == QStringList{QStringLiteral("first")});
    CHECK(inoutCanvas.CaptureViewState().Trace == RenderGraphTraceDirection::Downstream);
    const RenderGraphNavigation* navigationOrNull = inoutCanvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigationOrNull != nullptr);
    CHECK(navigationOrNull->GetTrace().Nodes.contains(QStringLiteral("third")) == true);
    CHECK(inoutCanvas.GetEdgeCount() == 4);
    CHECK(inoutCanvas.GetNodeCount() == 3);
}
void ClickMenuAction(const QString& name)
{
    QTimer::singleShot(0, [name]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull = menuOrNull->findChild<QAction*>(name);
        REQUIRE(actionOrNull != nullptr);
        QTest::mouseClick(menuOrNull,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          menuOrNull->actionGeometry(actionOrNull).center());
    });
}
void ShowMenu(RenderGraphCanvas& inoutCanvas, QPointF position)
{
    const QPoint point = inoutCanvas.mapFromScene(position);
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, inoutCanvas.viewport()->mapToGlobal(point));
    QCoreApplication::sendEvent(inoutCanvas.viewport(), &event);
}
} // namespace

TEST_CASE("wire route schema rejects malformed, duplicate, orphan and excessive points",
          "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    const QJsonObject configuration = GetConfiguration(workspace.GetDocument());
    CHECK(AreRenderGraphWireRoutesValid({MakeRoute()}, configuration) == true);
    CHECK(AreRenderGraphWireRoutesValid({MakeRoute(QStringLiteral("input.color"), QStringLiteral("first.color")),
                                         MakeRoute(QStringLiteral("third.color"), QStringLiteral("#outputs.color"))},
                                        configuration) == true);
    CHECK(AreRenderGraphWireRoutesValid({MakeRoute(), MakeRoute()}, configuration) == false);
    CHECK(AreRenderGraphWireRoutesValid({MakeRoute(GetFrom(), QStringLiteral("missing.color"))}, configuration) ==
          false);
    for (const QString& key : MakeRoute().keys())
    {
        QJsonObject route = MakeRoute();
        route.remove(key);
        CHECK(AreRenderGraphWireRoutesValid({route}, configuration) == false);
    }
    for (const QJsonValue& coordinate : {QJsonValue(true),
                                         QJsonValue(0.5),
                                         QJsonValue(layout_policy::MAX_COORDINATE + 1),
                                         QJsonValue(QStringLiteral("0"))})
    {
        QJsonObject route = MakeRoute();
        route.insert(QStringLiteral("points"),
                     QJsonArray{QJsonObject{{QStringLiteral("x"), coordinate}, {QStringLiteral("y"), 0}}});
        CHECK(AreRenderGraphWireRoutesValid({route}, configuration) == false);
    }
    QJsonObject route = MakeRoute();
    QJsonArray points;
    for (int32 index = 0; index < layout_policy::MAX_WIRE_REROUTE_POINTS; ++index)
    {
        points.append(QJsonObject{{QStringLiteral("x"), -layout_policy::MAX_COORDINATE},
                                  {QStringLiteral("y"), layout_policy::MAX_COORDINATE}});
    }
    route.insert(QStringLiteral("points"), points);
    CHECK(AreRenderGraphWireRoutesValid({route}, configuration) == true);
    points.append(points.first());
    route.insert(QStringLiteral("points"), points);
    CHECK(AreRenderGraphWireRoutesValid({route}, configuration) == false);
    route.insert(QStringLiteral("points"), QJsonArray{});
    CHECK(AreRenderGraphWireRoutesValid({route}, configuration) == false);
    QJsonArray routes;
    for (int32 index = 0; index <= layout_policy::MAX_WIRE_ROUTES; ++index)
    {
        routes.append(MakeRoute());
    }
    CHECK(AreRenderGraphWireRoutesValid(routes, configuration) == false);
}

TEST_CASE("wire add drag remove save and undo preserve selection tracing and build identity",
          "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    SelectTrace(canvas);
    const QByteArray before = document.GetDraft(GetRoot());
    const QByteArray identity = ComputeRenderGraphBuildIdentity(document, GetDefault(), RenderGraphSource::Draft);
    const uint64 revision = document.GetRevision();
    const QPoint point = canvas.mapFromScene(GetWire(canvas).path().pointAtPercent(0.5));
    QTest::mouseDClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    REQUIRE(GetRoutes(document).size() == 1);
    CHECK(document.GetRevision() == revision + 1);
    CheckTrace(canvas);
    RenderGraphReroutes& reroutes = canvas.GetReroutes();
    const QPointF origin = reroutes.GetPoints(GetFrom(), GetTo()).first();
    // The widget gesture path runs with Properties and workspace chrome hidden.
    const QPoint press = canvas.mapFromScene(origin);
    const QPoint destination = canvas.mapFromScene(origin + QPointF(0, -130));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, press);
    QTest::mouseMove(canvas.viewport(), destination);
    CHECK(document.GetRevision() == revision + 1);
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, destination);
    CHECK(document.GetRevision() == revision + 2);
    const QPointF moved = reroutes.GetPoints(GetFrom(), GetTo()).first();
    CHECK(moved.y() == origin.y() - 130);
    CheckTrace(canvas);
    const QPen wirePen = GetWire(canvas).pen();
    REQUIRE(GetWire(canvas).childItems().size() == 1);
    const QGraphicsEllipseItem* handle =
        qgraphicsitem_cast<QGraphicsEllipseItem*>(GetWire(canvas).childItems().first());
    REQUIRE(handle != nullptr);
    CHECK(handle->pen().color() == wirePen.color());
    CHECK(handle->effectiveOpacity() == GetWire(canvas).effectiveOpacity());
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_REROUTES_CAPTURE");
    if (capture.isEmpty() == false)
    {
        QCoreApplication::processEvents();
        REQUIRE(canvas.grab().save(capture) == true);
    }
    const QByteArray edited = document.GetDraft(GetRoot());
    CHECK(ComputeRenderGraphBuildIdentity(document, GetDefault(), RenderGraphSource::Draft) == identity);
    workspace.Undo();
    CHECK(reroutes.GetPoints(GetFrom(), GetTo()).first() == origin);
    CheckTrace(canvas);
    workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == before);
    CheckTrace(canvas);
    workspace.Redo();
    workspace.Redo();
    CHECK(document.GetDraft(GetRoot()) == edited);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    CHECK(GetRoutes(reopened) == GetRoutes(document));
    REQUIRE(reroutes.BeginGesture(moved) == true);
    REQUIRE(reroutes.EndGesture() == true);
    const uint64 beforeRemove = document.GetRevision();
    QTest::keyClick(&canvas, Qt::Key_Delete);
    CHECK(GetRoutes(document).isEmpty() == true);
    CHECK(document.GetRevision() == beforeRemove + 1);
    CheckTrace(canvas);
    workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == edited);
}

TEST_CASE("wire menus insert points in segment order and remove just one point", "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    SelectTrace(canvas);
    RenderGraphReroutes& reroutes = canvas.GetReroutes();
    reroutes.AddPoint(GetFrom(), GetTo(), 0, QPointF(300, -180));
    const QPainterPath path = GetWire(canvas).path();
    // Two cubic segments; choose a location along the first segment, outside the first card.
    const QPointF position = path.pointAtPercent(0.3);
    ClickMenuAction(QStringLiteral("renderGraphAddReroute"));
    ShowMenu(canvas, position);
    REQUIRE(reroutes.GetPoints(GetFrom(), GetTo()).size() == 2);
    CHECK(reroutes.GetPoints(GetFrom(), GetTo()).last() == QPointF(300, -180));
    const QPointF first = reroutes.GetPoints(GetFrom(), GetTo()).first();
    ClickMenuAction(QStringLiteral("renderGraphRemoveReroute"));
    ShowMenu(canvas, first);
    CHECK(reroutes.GetPoints(GetFrom(), GetTo()) == QVector<QPointF>{QPointF(300, -180)});
    CheckTrace(canvas);
    // Every routed segment still targets the original connection menu.
    ClickMenuAction(QStringLiteral("renderGraphDisconnectAction"));
    ShowMenu(canvas, GetWire(canvas).path().pointAtPercent(0.3));
    CHECK(GetRoutes(workspace.GetDocument()).isEmpty() == true);
    CHECK(canvas.GetEdgeCount() == 3);
    workspace.Undo();
    CHECK(reroutes.GetPoints(GetFrom(), GetTo()) == QVector<QPointF>{QPointF(300, -180)});
}

TEST_CASE("wire previews cancel on escape focus loss and stale documents without history edits",
          "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    SelectTrace(canvas);
    RenderGraphReroutes& reroutes = canvas.GetReroutes();
    reroutes.AddPoint(GetFrom(), GetTo(), 0, QPointF(300, -150));
    const uint64 revision = workspace.GetDocument().GetRevision();
    REQUIRE(reroutes.BeginGesture(QPointF(300, -150)) == true);
    REQUIRE(reroutes.UpdateGesture(QPointF(450, -200)) == true);
    QTest::keyClick(&canvas, Qt::Key_Escape);
    CHECK(reroutes.GetPoints(GetFrom(), GetTo()).first() == QPointF(300, -150));
    CheckTrace(canvas);
    REQUIRE(reroutes.BeginGesture(QPointF(300, -150)) == true);
    REQUIRE(reroutes.UpdateGesture(QPointF(450, -200)) == true);
    QFocusEvent focus(QEvent::FocusOut);
    QCoreApplication::sendEvent(&canvas, &focus);
    CHECK(reroutes.GetPoints(GetFrom(), GetTo()).first() == QPointF(300, -150));
    REQUIRE(reroutes.BeginGesture(QPointF(300, -150)) == true);
    REQUIRE(reroutes.EndGesture() == true);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    REQUIRE(reroutes.BeginGesture(QPointF(300, -150)) == true);
    REQUIRE(workspace.GetDocument().MoveNode(GetRoot(), GetDefault(), QStringLiteral("first"), 20, 20) ==
            RenderGraphDocumentStatus::Ok);
    QSignalSpy rejected(&reroutes, &RenderGraphReroutes::Rejected);
    REQUIRE(reroutes.UpdateGesture(QPointF(450, -200)) == true);
    CHECK(rejected.count() == 1);
    CHECK(reroutes.EndGesture() == false);
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
}

TEST_CASE("wire metadata follows renames and is cleaned atomically with graph edits",
          "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphDocument& document = workspace.GetDocument();
    const QJsonArray routes{MakeRoute(),
                            MakeRoute(QStringLiteral("input.color"), QStringLiteral("first.color")),
                            MakeRoute(QStringLiteral("third.color"), QStringLiteral("#outputs.color"))};
    REQUIRE(document.ReplaceWireRoutes(GetRoot(), GetDefault(), routes) == RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(GetRoot());
    QString error;
    REQUIRE(document.RenameNode(GetRoot(), GetDefault(), QStringLiteral("first"), QStringLiteral("renamed"), error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(GetRoutes(document).at(0).toObject().value(QStringLiteral("from")).toString() ==
          QStringLiteral("renamed.color"));
    CHECK(GetRoutes(document).at(1).toObject().value(QStringLiteral("to")).toString() ==
          QStringLiteral("renamed.color"));
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(GetRoot()) == before);
    REQUIRE(document.ReplaceConnection(GetRoot(), GetDefault(), QStringLiteral("input.color"), GetTo()) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(GetRoutes(document).size() == 2);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(GetRoot()) == before);
    REQUIRE(
        document.Disconnect(GetRoot(), GetDefault(), QStringLiteral("third.color"), QStringLiteral("#outputs.color")) ==
        RenderGraphDocumentStatus::Ok);
    CHECK(GetRoutes(document).size() == 2);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(GetRoot()) == before);
    REQUIRE(document.RemoveNode(GetRoot(), GetDefault(), QStringLiteral("first")) == RenderGraphDocumentStatus::Ok);
    CHECK(GetRoutes(document) == QJsonArray{routes.last()});
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(GetRoot()) == before);
    REQUIRE(document.MoveNodes(GetRoot(), GetDefault(), {{QStringLiteral("first"), 30, 40}}) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(GetRoutes(document) == routes); // Saved points remain pinned when nodes move.
}

TEST_CASE("wire metadata failures preserve source and invalid source routes remain repairable",
          "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray before = document.GetDraft(GetRoot());
    const uint64 revision = document.GetRevision();
    CHECK(
        document.ReplaceWireRoutes(GetRoot(), GetDefault(), {MakeRoute(GetFrom(), QStringLiteral("missing.color"))}) ==
        RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetDraft(GetRoot()) == before);
    CHECK(document.GetRevision() == revision);
    QByteArray source = before;
    source.replace("\"positions\":[",
                   "\"routes\":[{\"from\":\"missing.color\",\"to\":\"second.color\",\"points\":[{\"x\":1,\"y\":2}]}],"
                   "\"positions\":[");
    source.append(" \n\t");
    REQUIRE(source != before);
    REQUIRE(document.SetText(GetRoot(), source) == RenderGraphDocumentStatus::Ok);
    CHECK(document.GetDraft(GetRoot()) == source);
    QSignalSpy rejected(&workspace.GetCanvas().GetReroutes(), &RenderGraphReroutes::Rejected);
    workspace.GetCanvas().Rebuild();
    CHECK(rejected.count() == 1);
    workspace.GetCanvas().GetReroutes().AddPoint(GetFrom(), GetTo(), 0, QPointF(300, -100));
    CHECK(document.GetDraft(GetRoot()) == source);
    CHECK(rejected.count() == 2);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(GetRoot()) == before);
}

TEST_CASE("interface repairs discard obsolete wire routes in the same undo step", "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphDocument& document = workspace.GetDocument();
    REQUIRE(document.ReplaceWireRoutes(GetRoot(),
                                       GetDefault(),
                                       {MakeRoute(),
                                        MakeRoute(QStringLiteral("input.color"), QStringLiteral("first.color")),
                                        MakeRoute(QStringLiteral("third.color"), QStringLiteral("#outputs.color"))}) ==
            RenderGraphDocumentStatus::Ok);
    const QByteArray before = document.GetDraft(GetRoot());
    QString error;
    REQUIRE(document.ReplaceConfigurationInterface(GetRoot(), GetDefault(), {}, {}, error) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(GetRoutes(document) == QJsonArray{MakeRoute()});
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(GetRoot()) == before);
}

TEST_CASE("failed route transactions restore preview and retain node selection and trace",
          "[editor][render-graph][reroutes]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphDocument& document = workspace.GetDocument();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    // An opaque source extension fills the file budget; canonical authoring still retains it.
    QJsonObject graph = QJsonDocument::fromJson(document.GetDraft(GetRoot())).object();
    graph.insert(QStringLiteral("padding"), QString());
    const qsizetype padding = RenderGraphDocument::MAX_FILE_BYTES - EncodeCanonicalJson(graph).size();
    REQUIRE(padding > 0);
    graph.insert(QStringLiteral("padding"), QString(padding, QLatin1Char('a')));
    const QByteArray full = EncodeCanonicalJson(graph);
    REQUIRE(full.size() == RenderGraphDocument::MAX_FILE_BYTES);
    REQUIRE(document.SetText(GetRoot(), full) == RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    SelectTrace(canvas);
    const uint64 revision = document.GetRevision();
    canvas.GetReroutes().AddPoint(GetFrom(), GetTo(), 0, QPointF(300, -150));
    CHECK(document.GetDraft(GetRoot()) == full);
    CHECK(document.GetRevision() == revision);
    CHECK(canvas.GetReroutes().GetPoints(GetFrom(), GetTo()).isEmpty() == true);
    CheckTrace(canvas);
}
