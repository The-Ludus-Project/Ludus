#include "internal/render_graph_live_build.h"
#include "internal/render_graph_navigation.h"
#include "internal/render_graph_reroutes.h"
#include "internal/render_graph_resource_inspection.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
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
QByteArray MakeGraph()
{
    return QByteArrayLiteral(R"JSON({"version":2,"id":"Main","default":"Default","configurations":[{
      "id":"Default","inputs":[],"outputs":[{"id":"color","type":"ColorTexture","source":"tone.color","required":false}],
      "nodes":[{"id":"color","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","extent":[320,200],"samples":1,"imported":false,"presentation":false},
               {"id":"other","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","extent":[640,400],"samples":1,"imported":false,"presentation":false},
               {"id":"depth","kind":"Target","type":"DepthTexture","format":"Depth32Float","extent":[320,200],"samples":1,"imported":false,"presentation":false},
               {"id":"draw","kind":"Raster","mode":"ColorAndDepth"},
               {"id":"tone","kind":"ToneMap","exposure":1},
               {"id":"export","kind":"Export","type":"ColorTexture"},
               {"id":"storage","kind":"Buffer","bytes":256},
               {"id":"scene","kind":"Scene"}],
      "edges":[{"from":"color.texture","to":"draw.color"},{"from":"depth.texture","to":"draw.depth"},
               {"from":"draw.color","to":"tone.color"},{"from":"tone.color","to":"export.texture"}],
      "layout":{"positions":[{"node":"color","x":0,"y":0},{"node":"draw","x":400,"y":0},{"node":"tone","x":800,"y":0}]}}]})JSON");
}
void WriteFile(const QTemporaryDir& project, const QString& name, const QByteArray& bytes)
{
    QFile file(QDir(project.path()).filePath(name));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}
void LoadDocument(RenderGraphDocument& inoutDocument, const QTemporaryDir& project)
{
    WriteFile(project, GetRoot(), MakeGraph());
    REQUIRE(inoutDocument.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
}
RenderGraphResourceRequest MakeRequest(const QString& endpoint = QStringLiteral("draw.color"), bool isOutput = true)
{
    return {GetRoot(), GetDefault(), endpoint, isOutput, {{GetRoot(), GetDefault(), {}, {}, false}}};
}
RenderGraphResourcePreviewSnapshot MakeSnapshot(const RenderGraphDocument& document)
{
    RenderGraphResourcePreviewSnapshot snapshot;
    snapshot.IsAttached = true;
    snapshot.HasNames = true;
    snapshot.HasPresentedCatalogue = true;
    snapshot.ActiveGeneration = 1;
    snapshot.PresentedGeneration = 1;
    snapshot.Configuration = GetDefault();
    snapshot.RootFile = GetRoot();
    snapshot.InputIdentity = ComputeRenderGraphBuildIdentity(document, GetDefault());
    snapshot.Targets = {{QStringLiteral("Main/color"), QStringLiteral("320 × 200 · RGBA8Unorm · exported"), true},
                        {QStringLiteral("Main/depth"), QStringLiteral("320 × 200 · Depth32Float"), true},
                        {QStringLiteral("Main/storage"), QStringLiteral("Buffer · no image"), false}};
    return snapshot;
}
void OpenWorkspace(RendererWorkspace& inoutWorkspace, const QTemporaryDir& project)
{
    WriteFile(project, GetRoot(), MakeGraph());
    inoutWorkspace.SetProject(project.path(), 1);
    REQUIRE(inoutWorkspace.OpenGraph(QDir(project.path()).filePath(GetRoot())) == true);
    inoutWorkspace.SetLivePreview(false);
    inoutWorkspace.resize(1200, 850);
    inoutWorkspace.show();
    QCoreApplication::processEvents();
    inoutWorkspace.SetChromeVisible(false);
    // Settle the canvas at its chrome-free size, so view states are captured after the resize.
    QCoreApplication::processEvents();
}
void ScheduleInspectMenu(bool shouldChangeDocument, RendererWorkspace& inoutWorkspace)
{
    QTimer::singleShot(0, [shouldChangeDocument, &inoutWorkspace]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphInspectResource"));
        REQUIRE(actionOrNull != nullptr);
        if (shouldChangeDocument == true)
        {
            REQUIRE(inoutWorkspace.GetDocument().MoveNode(GetRoot(), GetDefault(), QStringLiteral("color"), 10, 10) ==
                    RenderGraphDocumentStatus::Ok);
        }
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

TEST_CASE("resource inspection resolves exact output input and configuration-output connections",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    LoadDocument(document, project);
    const QByteArray before = document.GetDraft(GetRoot());
    for (const RenderGraphResourceRequest& request : {MakeRequest(),
                                                      MakeRequest(QStringLiteral("tone.color"), false),
                                                      MakeRequest(QStringLiteral("#outputs.color"), false)})
    {
        const RenderGraphResourceDetails details = ResolveRenderGraphResourceInspection(document, request, {});
        CHECK(details.ResourceName == QStringLiteral("Main/color"));
        CHECK(details.Type == QStringLiteral("ColorTexture"));
        CHECK(details.Description.contains(QStringLiteral("320 × 200")) == true);
        CHECK(details.Problem.isEmpty() == true);
    }
    const RenderGraphResourceDetails color =
        ResolveRenderGraphResourceInspection(document, MakeRequest(QStringLiteral("color.texture")), {});
    CHECK(color.Consumers == QStringList{QStringLiteral("draw.color")});
    const RenderGraphResourceDetails depth =
        ResolveRenderGraphResourceInspection(document, MakeRequest(QStringLiteral("draw.depth")), {});
    CHECK(depth.ResourceName == QStringLiteral("Main/depth"));
    CHECK(depth.Description.contains(QStringLiteral("Depth32Float")) == true);
    CHECK(document.GetDraft(GetRoot()) == before);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("resource identity uses caller breadcrumbs and never guesses among repeated subgraph instances",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    WriteFile(project, GetRoot(), MakeGraph());
    WriteFile(project,
              QStringLiteral("child.lurender.json"),
              QByteArrayLiteral(R"JSON({"version":2,"id":"Shared","default":"Default","configurations":[{"id":"Default",
      "inputs":[{"id":"color","type":"ColorTexture"}],"outputs":[{"id":"local","type":"ColorTexture","source":"local.texture","required":false},{"id":"borrowed","type":"ColorTexture","source":"input.color","required":false}],
      "nodes":[{"id":"local","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","extent":[100,80],"samples":1,"imported":false,"presentation":false}],"edges":[]}]})JSON"));
    RenderGraphDocument document;
    REQUIRE(document.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    for (const QString& instance : {QStringLiteral("left"), QStringLiteral("right")})
    {
        REQUIRE(document.AddNode(GetRoot(),
                                 GetDefault(),
                                 {{QStringLiteral("id"), instance},
                                  {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                  {QStringLiteral("asset"), QStringLiteral("child.lurender.json")},
                                  {QStringLiteral("configuration"), GetDefault()}}) == RenderGraphDocumentStatus::Ok);
        REQUIRE(document.Connect(GetRoot(),
                                 GetDefault(),
                                 instance == QStringLiteral("left") ? QStringLiteral("color.texture")
                                                                    : QStringLiteral("other.texture"),
                                 instance + QStringLiteral(".color")) == RenderGraphDocumentStatus::Ok);
        RenderGraphResourceRequest request{
            QStringLiteral("child.lurender.json"),
            GetDefault(),
            QStringLiteral("local.texture"),
            true,
            {{GetRoot(), GetDefault(), {}, {}, false},
             {QStringLiteral("child.lurender.json"), GetDefault(), instance, {}, false}}};
        CHECK(ResolveRenderGraphResourceInspection(document, request, {}).ResourceName ==
              QStringLiteral("Main/") + instance + QStringLiteral("/local"));
        request.Endpoint = QStringLiteral("input.color");
        CHECK(ResolveRenderGraphResourceInspection(document, request, {}).ResourceName ==
              (instance == QStringLiteral("left") ? QStringLiteral("Main/color") : QStringLiteral("Main/other")));
        CHECK(ResolveRenderGraphResourceInspection(document, MakeRequest(instance + QStringLiteral(".local")), {})
                  .ResourceName == QStringLiteral("Main/") + instance + QStringLiteral("/local"));
        request.Path.clear();
        CHECK(ResolveRenderGraphResourceInspection(document, request, {}).ResourceName.isEmpty() == true);
        CHECK(ResolveRenderGraphResourceInspection(document, request, {}).Problem.contains(QStringLiteral("caller")) ==
              true);
    }
}

TEST_CASE("resource resolution reports missing cyclic unconnected and non-image endpoints",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    LoadDocument(document, project);
    CHECK(ResolveRenderGraphResourceInspection(document, MakeRequest(QStringLiteral("draw.view"), false), {})
              .Problem.contains(QStringLiteral("Unconnected")) == true);
    CHECK(ResolveRenderGraphResourceInspection(document, MakeRequest(QStringLiteral("draw.missing")), {})
              .Problem.isEmpty() == false);
    CHECK(ResolveRenderGraphResourceInspection(document, MakeRequest(QStringLiteral("scene.scene")), {})
              .Problem.contains(QStringLiteral("graph value")) == true);
    const RenderGraphResourceDetails buffer =
        ResolveRenderGraphResourceInspection(document, MakeRequest(QStringLiteral("storage.buffer")), {});
    CHECK(buffer.ResourceName == QStringLiteral("Main/storage"));
    CHECK(buffer.Type == QStringLiteral("Buffer"));
    REQUIRE(document.ReplaceConnection(GetRoot(),
                                       GetDefault(),
                                       QStringLiteral("tone.color"),
                                       QStringLiteral("tone.color")) == RenderGraphDocumentStatus::Ok);
    CHECK(ResolveRenderGraphResourceInspection(document, MakeRequest(QStringLiteral("tone.color")), {})
              .ResourceName.isEmpty() == true);
}

TEST_CASE("resource inspector separates current stale unverified pending and unavailable generation metadata",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    LoadDocument(document, project);
    QWidget parent;
    RenderGraphResourceInspector inspector(document, parent);
    inspector.Inspect(MakeRequest());
    CHECK(inspector.GetStatusText().startsWith(QStringLiteral("Unavailable")) == true);
    const RenderGraphResourcePreviewSnapshot current = MakeSnapshot(document);
    inspector.SetPreviewSnapshot(current);
    CHECK(inspector.GetStatusText().startsWith(QStringLiteral("Current")) == true);
    CHECK(inspector.CanOpenInTextures() == true);
    REQUIRE(document.MoveNode(GetRoot(), GetDefault(), QStringLiteral("color"), 10, 10) ==
            RenderGraphDocumentStatus::Ok);
    inspector.Refresh();
    CHECK(inspector.GetStatusText().startsWith(QStringLiteral("Current")) == true); // Layout is not semantic.
    QJsonObject graph;
    REQUIRE(document.GetGraph(GetRoot(), graph) == true);
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject configuration = configurations.first().toObject();
    QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    QJsonObject target = nodes.first().toObject();
    target.insert(QStringLiteral("extent"), QJsonArray{640, 480});
    nodes.replace(0, target);
    configuration.insert(QStringLiteral("nodes"), nodes);
    configurations.replace(0, configuration);
    graph.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(GetRoot(), EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    inspector.Refresh();
    CHECK(inspector.GetStatusText().startsWith(QStringLiteral("Stale")) == true);
    CHECK(inspector.GetDetailsText().contains(QStringLiteral("640 × 480")) == true);
    CHECK(inspector.GetDetailsText().contains(QStringLiteral("320 × 200 · RGBA8Unorm")) == true);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_RESOURCE_INSPECTION_CAPTURE");
    if (capture.isEmpty() == false)
    {
        QCoreApplication::processEvents();
        REQUIRE(inspector.grab().save(capture) == true);
    }
    RenderGraphResourcePreviewSnapshot snapshot = current;
    snapshot.InputIdentity.clear();
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.GetStatusText().startsWith(QStringLiteral("Unverified")) == true);
    snapshot = current;
    snapshot.ActiveGeneration = 2;
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.GetStatusText().contains(QStringLiteral("awaiting")) == true);
    CHECK(inspector.CanOpenInTextures() == false);
    snapshot = current;
    snapshot.RootFile = QStringLiteral("other-root.lurender.json");
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.GetStatusText().contains(QStringLiteral("another root")) == true);
    CHECK(inspector.CanOpenInTextures() == false);
    snapshot = current;
    snapshot.HasPresentedCatalogue = false; // Same generation serial, but a new drawable extent/session.
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.CanOpenInTextures() == false);
    snapshot = current;
    snapshot.HasNames = false;
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.GetStatusText().contains(QStringLiteral("catalogue")) == true);
    snapshot = current;
    snapshot.Targets.clear();
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.GetStatusText().contains(QStringLiteral("absent")) == true);
    snapshot = current;
    snapshot.Targets.first().CanOpen = false;
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.GetStatusText().contains(QStringLiteral("Unavailable in Textures")) == true);
    CHECK(inspector.CanOpenInTextures() == false);
    snapshot = current;
    snapshot.ActiveGeneration = 0;
    snapshot.Problem = QStringLiteral("candidate preparation failed");
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.GetStatusText().contains(snapshot.Problem) == true);
    snapshot = current;
    snapshot.IsAttached = false;
    inspector.SetPreviewSnapshot(snapshot);
    CHECK(inspector.CanOpenInTextures() == false);
}

TEST_CASE("pinning keeps resource identity across hover navigation rebuilds and connection replacement",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    LoadDocument(document, project);
    QWidget parent;
    RenderGraphResourceInspector inspector(document, parent);
    inspector.Inspect(MakeRequest());
    inspector.SetPreviewSnapshot(MakeSnapshot(document));
    inspector.SetPinned(true);
    inspector.Follow(MakeRequest(QStringLiteral("depth.texture")));
    CHECK(inspector.GetDetailsText().contains(QStringLiteral("Expanded resource: Main/color")) == true);
    REQUIRE(document.ReplaceConnection(GetRoot(),
                                       GetDefault(),
                                       QStringLiteral("other.texture"),
                                       QStringLiteral("draw.color")) == RenderGraphDocumentStatus::Ok);
    inspector.Refresh();
    CHECK(inspector.GetStatusText().contains(QStringLiteral("pinned resource Main/color")) == true);
    CHECK(inspector.CanOpenInTextures() == false);
    REQUIRE(document.Undo() == true);
    inspector.Refresh();
    CHECK(inspector.CanOpenInTextures() == true);
    inspector.SetPinned(false);
    inspector.Follow(MakeRequest(QStringLiteral("depth.texture")));
    CHECK(inspector.GetDetailsText().contains(QStringLiteral("Expanded resource: Main/depth")) == true);
    inspector.SetPinned(true);
    inspector.Inspect(MakeRequest()); // An explicit request replaces the pin deliberately.
    CHECK(inspector.GetDetailsText().contains(QStringLiteral("Expanded resource: Main/color")) == true);
    inspector.close();
    CHECK(inspector.IsPinned() == false);
    inspector.Inspect(MakeRequest());
    inspector.SetPinned(true);
    QTest::keyClick(&inspector, Qt::Key_Escape);
    CHECK(inspector.IsPinned() == false);
    inspector.Reset();
    CHECK(inspector.isVisible() == false);
    CHECK(inspector.CanOpenInTextures() == false);
}

TEST_CASE("Open in Textures requests an exact resource and presented generation without document edits",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    LoadDocument(document, project);
    QWidget parent;
    RenderGraphResourceInspector inspector(document, parent);
    inspector.Inspect(MakeRequest());
    inspector.SetPreviewSnapshot(MakeSnapshot(document));
    QSignalSpy open(&inspector, &RenderGraphResourceInspector::OpenInTexturesRequested);
    QPushButton* buttonOrNull = inspector.findChild<QPushButton*>(QStringLiteral("renderGraphOpenInTextures"));
    REQUIRE(buttonOrNull != nullptr);
    buttonOrNull->click();
    REQUIRE(open.count() == 1);
    CHECK(open.first().at(0).toString() == QStringLiteral("Main/color"));
    CHECK(open.first().at(1).toULongLong() == 1);
    QObject::connect(&inspector, &RenderGraphResourceInspector::PreviewRequested, &inspector, [&inspector]() {
        inspector.SetPreviewSnapshot({});
    });
    buttonOrNull->click();
    CHECK(open.count() == 1); // Generation changed between rendering the button and activating it.
    CHECK(document.IsDirty() == false);
    CHECK(document.CanUndo() == false);
}

TEST_CASE("canvas port wire and reroute inspection preserve selection trace view and history with Properties hidden",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState state{{QStringLiteral("draw")},
                                     QPointF(400, 150),
                                     1,
                                     RenderGraphTraceDirection::Downstream,
                                     {}};
    REQUIRE(canvas.RestoreViewState(state) == true);
    const RenderGraphViewState expected = canvas.CaptureViewState();
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    const uint64 revision = workspace.GetDocument().GetRevision();
    const int32 visits = canvas.GetHistory().GetEntryCount();
    RenderGraphResourceInspector* inspectorOrNull = workspace.findChild<RenderGraphResourceInspector*>();
    REQUIRE(inspectorOrNull != nullptr);
    for (bool isOutput : {true, false})
    {
        ScheduleInspectMenu(false, workspace);
        ShowMenu(canvas, canvas.GetPortPosition(QStringLiteral("draw.color"), isOutput));
        CHECK(inspectorOrNull->isVisible() == true);
        CHECK(inspectorOrNull->GetDetailsText().contains(QStringLiteral("Main/color")) == true);
        CHECK(canvas.GetSelectedNodes() == state.Selection);
        CHECK(canvas.CaptureViewState().Trace == state.Trace);
        CHECK(canvas.CaptureViewState().Zoom == state.Zoom);
        CHECK(canvas.CaptureViewState().CentreScene == expected.CentreScene);
        CHECK(canvas.GetHistory().GetEntryCount() == visits);
        inspectorOrNull->close();
    }
    canvas.GetReroutes().AddPoint(QStringLiteral("color.texture"), QStringLiteral("draw.color"), 0, QPointF(300, -130));
    ScheduleInspectMenu(false, workspace);
    ShowMenu(canvas, QPointF(300, -130));
    CHECK(inspectorOrNull->isVisible() == true);
    inspectorOrNull->close();
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        QGraphicsPathItem* wireOrNull = qgraphicsitem_cast<QGraphicsPathItem*>(item);
        if (wireOrNull != nullptr &&
            wireOrNull->toolTip().startsWith(QStringLiteral("color.texture → draw.color")) == true)
        {
            const QPointF wirePosition = wireOrNull->path().pointAtPercent(0.3);
            ScheduleInspectMenu(false, workspace);
            ShowMenu(canvas, wirePosition);
            break;
        }
    }
    CHECK(inspectorOrNull->isVisible() == true);
    CHECK(canvas.GetSelectedNodes() == state.Selection);
    CHECK(canvas.CaptureViewState().Trace == state.Trace);
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    inspectorOrNull->close();
    ScheduleInspectMenu(true, workspace);
    ShowMenu(canvas, canvas.GetPortPosition(QStringLiteral("draw.color"), true));
    CHECK(inspectorOrNull->isVisible() == false); // A stale menu cannot retarget after a graph edit.
}

TEST_CASE("resource menus reject a changed caller instance even when file configuration and revision match",
          "[editor][render-graph][resource-inspection]")
{
    QTemporaryDir project;
    WriteFile(
        project,
        QStringLiteral("child.lurender.json"),
        QByteArrayLiteral(
            R"JSON({"version":2,"id":"Shared","default":"Default","configurations":[{"id":"Default","inputs":[],"outputs":[],
      "nodes":[{"id":"local","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","extent":[100,80],"samples":1,"imported":false,"presentation":false}],"edges":[]}]})JSON"));
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    for (const QString& instance : {QStringLiteral("left"), QStringLiteral("right")})
    {
        REQUIRE(workspace.GetDocument().AddNode(GetRoot(),
                                                GetDefault(),
                                                {{QStringLiteral("id"), instance},
                                                 {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                                 {QStringLiteral("asset"), QStringLiteral("child.lurender.json")},
                                                 {QStringLiteral("configuration"), GetDefault()}}) ==
                RenderGraphDocumentStatus::Ok);
    }
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    REQUIRE(canvas.ActivateNode(QStringLiteral("left")) == true);
    REQUIRE(canvas.GetFile() == QStringLiteral("child.lurender.json"));
    const uint64 revision = workspace.GetDocument().GetRevision();
    QTimer::singleShot(0, [&canvas]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphInspectResource"));
        REQUIRE(actionOrNull != nullptr);
        REQUIRE(canvas.GetHistory().Back() == true);
        REQUIRE(canvas.ActivateNode(QStringLiteral("right")) == true);
        QTest::mouseClick(menuOrNull,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          menuOrNull->actionGeometry(actionOrNull).center());
    });
    ShowMenu(canvas, canvas.GetPortPosition(QStringLiteral("local.texture"), true));
    CHECK(workspace.GetDocument().GetRevision() == revision);
    CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("right"));
    const RenderGraphResourceInspector* inspectorOrNull = workspace.findChild<RenderGraphResourceInspector*>();
    REQUIRE(inspectorOrNull != nullptr);
    CHECK(inspectorOrNull->isVisible() == false);
}
