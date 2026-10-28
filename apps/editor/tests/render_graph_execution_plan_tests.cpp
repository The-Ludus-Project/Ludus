#include "internal/render_graph_comments.h"
#include "internal/render_graph_execution_plan.h"
#include "internal/render_graph_live_build.h"
#include "internal/render_graph_navigation.h"
#include "internal/renderer_workspace.h"
#include "render_graph_plan_policy.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

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

QJsonObject OpenWorkspace(RendererWorkspace& inoutWorkspace, const QTemporaryDir& temporary)
{
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("project"));
    QProcess compiler;
    compiler.setWorkingDirectory(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT));
    compiler.start(QStringLiteral(LUDUS_EDITOR_TEST_PYTHON),
                   {QStringLiteral("-c"),
                    QStringLiteral(R"PY(
import sys,json,copy
from pathlib import Path
sys.path.insert(0,'scripts/python')
from ludus_tools import render_graph as graph
root=Path(sys.argv[1]); path=graph.new_workspace(root,output='offscreen')
value=json.loads(path.read_text()); config=value['configurations'][0]
second=copy.deepcopy(next(n for n in config['nodes'] if n['id']=='raster')); second['id']='second'
config['nodes'].append(second)
config['edges'] += [{'from': ('raster.'+edge['to'].split('.')[1] if edge['to'] in ('raster.color','raster.depth') else edge['from']),
                     'to':'second.'+edge['to'].split('.')[1]}
                    for edge in list(config['edges']) if edge['to'].startswith('raster.')]
for edge in config['edges']:
    if edge['to']=='output.texture': edge['from']='second.color'
unused=copy.deepcopy(next(n for n in config['nodes'] if n['id']=='color')); unused['id']='unused'
config['nodes'].append(unused)
dormant=copy.deepcopy(second); dormant['id']='dormant'; config['nodes'].append(dormant)
positions={'scene':(0,0),'color':(300,0),'depth':(300,200),'camera':(600,0),
           'raster':(900,0),'second':(1200,0),'output':(1500,0),'unused':(0,440),'dormant':(900,440)}
config['layout']={'positions':[{'node':n['id'],'x':positions.get(n['id'],(0,200))[0],
                              'y':positions.get(n['id'],(0,200))[1]} for n in config['nodes']]}
path.write_text(json.dumps(value))
print(json.dumps(graph.compile_graph(path)))
)PY"),
                    root});
    REQUIRE(compiler.waitForFinished() == true);
    INFO(compiler.readAllStandardError().constData());
    REQUIRE(compiler.exitCode() == 0);
    const QJsonObject plan = QJsonDocument::fromJson(compiler.readAllStandardOutput()).object();
    REQUIRE(plan.value(QStringLiteral("passes")).toArray().size() == 3);
    inoutWorkspace.SetProject(root, 1);
    REQUIRE(inoutWorkspace.OpenGraph(QDir(root).filePath(GetRoot())) == true);
    inoutWorkspace.SetLivePreview(false);
    inoutWorkspace.resize(1450, 900);
    inoutWorkspace.show();
    QCoreApplication::processEvents();
    inoutWorkspace.SetChromeVisible(false);
    return plan;
}
RenderGraphJobResult MakeResult(RendererWorkspace& inoutWorkspace, const QJsonObject& plan)
{
    RenderGraphJobResult result;
    result.IsSuccessful = true;
    result.Request.ProjectEpoch = 1;
    result.Request.Action = QStringLiteral("dump");
    result.Request.Revision = inoutWorkspace.GetDocument().GetRevision();
    const QComboBox* configurationOrNull =
        inoutWorkspace.findChild<QComboBox*>(QStringLiteral("renderGraphConfiguration"));
    REQUIRE(configurationOrNull != nullptr);
    result.Request.InputIdentity = ComputeRenderGraphBuildIdentity(inoutWorkspace.GetDocument(),
                                                                   configurationOrNull->currentText(),
                                                                   RenderGraphSource::Draft);
    result.Payload.insert(QStringLiteral("plan"), plan);
    return result;
}
void Refresh(RendererWorkspace& inoutWorkspace)
{
    const QComboBox* configurationOrNull =
        inoutWorkspace.findChild<QComboBox*>(QStringLiteral("renderGraphConfiguration"));
    REQUIRE(configurationOrNull != nullptr);
    inoutWorkspace.GetCanvas().GetExecutionPlan().Refresh(configurationOrNull->currentText());
}
QJsonObject MakeFrame(const QString& identity, const QRectF& bounds)
{
    return SetRenderGraphCommentBounds({{QStringLiteral("id"), identity},
                                        {QStringLiteral("title"), identity},
                                        {QStringLiteral("notes"), QStringLiteral("Pipeline stage")},
                                        {QStringLiteral("colour"), QStringLiteral("#547FA3")}},
                                       bounds);
}
RenderGraphViewState SelectTrace(RenderGraphCanvas& inoutCanvas)
{
    RenderGraphViewState state = inoutCanvas.CaptureViewState();
    state.Selection = {QStringLiteral("raster"), QStringLiteral("second")};
    state.Trace = RenderGraphTraceDirection::Upstream;
    state.Zoom = 0.8;
    state.CentreScene = QPointF(700, 200);
    REQUIRE(inoutCanvas.RestoreViewState(state) == true);
    return inoutCanvas.CaptureViewState();
}
void CheckView(const RenderGraphCanvas& canvas, const RenderGraphViewState& expected)
{
    const RenderGraphViewState actual = canvas.CaptureViewState();
    CHECK(actual.Selection == expected.Selection);
    CHECK(actual.Trace == expected.Trace);
    CHECK(actual.Zoom == expected.Zoom);
    CHECK(actual.RevealedFrames == expected.RevealedFrames);
    CHECK(qAbs(actual.CentreScene.x() - expected.CentreScene.x()) < 1);
    CHECK(qAbs(actual.CentreScene.y() - expected.CentreScene.y()) < 1);
}
QGraphicsItem* FindItemOrNull(const RenderGraphCanvas& canvas, const QString& identity)
{
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        if (item->data(0).toString() == identity)
        {
            return item;
        }
    }
    return nullptr;
}
QTreeWidget& GetOperations(RendererWorkspace& inoutWorkspace)
{
    QTreeWidget* treeOrNull = inoutWorkspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphPlanOperations"));
    REQUIRE(treeOrNull != nullptr);
    return *treeOrNull;
}
} // namespace

TEST_CASE("execution overlay uses real compiler activity and exact serial ordinals without changing graph or view",
          "[editor][render-graph][execution-plan]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QJsonObject plan = OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState view = SelectTrace(canvas);
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    const uint64 revision = workspace.GetDocument().GetRevision();
    const int32 visits = canvas.GetHistory().GetEntryCount();
    workspace.AcceptResult(MakeResult(workspace, plan));
    RenderGraphExecutionPlan& overlay = canvas.GetExecutionPlan();
    CHECK(overlay.GetFreshness() == RenderGraphPlanFreshness::Current);
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("raster")).Orders == QVector<int32>{1});
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("second")).Orders == QVector<int32>{2});
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("output")).Orders == QVector<int32>{3});
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("scene")).Label == QStringLiteral("Active value"));
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("unused")).IsPruned == true);
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("dormant")).IsPruned == true);
    CHECK(GetOperations(workspace).topLevelItemCount() == 3);
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("#outputs")).HasActivity == false);
    QSignalSpy refreshed(&overlay, &RenderGraphExecutionPlan::Changed);
    Refresh(workspace);
    Refresh(workspace);
    CHECK(refreshed.isEmpty() == true);
    QSignalSpy selection(&canvas, &RenderGraphCanvas::NodesSelected);
    const QPointF position = canvas.GetNodePosition(QStringLiteral("raster"));
    QAction* toggleOrNull = canvas.findChild<QAction*>(QStringLiteral("renderGraphExecutionPlanOverlay"));
    REQUIRE(toggleOrNull != nullptr);
    toggleOrNull->trigger();
    REQUIRE(FindItemOrNull(canvas, QStringLiteral("#plan-badge:raster")) != nullptr);
    CHECK(FindItemOrNull(canvas, QStringLiteral("#plan-badge:raster"))->isVisible() == true);
    CHECK(FindItemOrNull(canvas, QStringLiteral("unused"))->opacity() < 1);
    overlay.SetEnabled(false);
    CHECK(FindItemOrNull(canvas, QStringLiteral("#plan-badge:raster"))->isVisible() == false);
    CHECK(selection.isEmpty() == true);
    CheckView(canvas, view);
    CHECK(canvas.GetNodePosition(QStringLiteral("raster")) == position);
    CHECK(canvas.GetHistory().GetEntryCount() == visits);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CHECK(workspace.GetDocument().GetRevision() == revision);
}

TEST_CASE("execution overlay summarizes collapsed stages and retains saved bounds and tracing through undo",
          "[editor][render-graph][execution-plan]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QJsonObject plan = OpenWorkspace(workspace, project);
    REQUIRE(workspace.GetDocument().ReplaceCommentFrames(
                GetRoot(),
                GetDefault(),
                {MakeFrame(QStringLiteral("Rendering"), QRectF(850, -100, 1000, 400)),
                 MakeFrame(QStringLiteral("Unused"), QRectF(-40, 360, 1200, 330))}) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    canvas.GetComments().SetCollapsed(QStringLiteral("Rendering"), true);
    canvas.GetComments().SetCollapsed(QStringLiteral("Unused"), true);
    const RenderGraphViewState view = SelectTrace(canvas);
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    workspace.AcceptResult(MakeResult(workspace, plan));
    canvas.GetExecutionPlan().SetEnabled(true);
    const RenderGraphPlanAnnotation active = canvas.GetExecutionPlan().GetStageAnnotation(
        {QStringLiteral("raster"), QStringLiteral("second"), QStringLiteral("output")});
    CHECK(active.Orders == QVector<int32>{1, 2, 3});
    CHECK(active.Label.contains(QStringLiteral("3 passes")) == true);
    CHECK(
        canvas.GetExecutionPlan().GetStageAnnotation({QStringLiteral("unused"), QStringLiteral("dormant")}).IsPruned ==
        true);
    CHECK(FindItemOrNull(canvas, QStringLiteral("raster"))->isVisible() == false);
    CHECK(FindItemOrNull(canvas, QStringLiteral("#comment:Unused"))->opacity() < 1);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CheckView(canvas, view);
    canvas.GetComments().SetCollapsed(QStringLiteral("Rendering"), false);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CHECK(FindItemOrNull(canvas, QStringLiteral("raster"))->isVisible() == false);
    CHECK(canvas.GetExecutionPlan().GetFreshness() == RenderGraphPlanFreshness::Current);
    CheckView(canvas, view);
    const QString captures = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_PLAN_CAPTURE");
    if (captures.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(captures) == true);
        canvas.FitGraph();
        QCoreApplication::processEvents();
        REQUIRE(canvas.grab().save(QDir(captures).filePath(QStringLiteral("execution-plan-stages.png"))) == true);
    }
}

TEST_CASE("Plan navigation resolves repeated library instances and restores views with Back and Forward",
          "[editor][render-graph][execution-plan]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QJsonObject plan = OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState root = SelectTrace(canvas);
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    workspace.AcceptResult(MakeResult(workspace, plan));
    canvas.GetExecutionPlan().SetEnabled(true);
    REQUIRE(canvas.GetExecutionPlan().RevealOperation(1) == true);
    CHECK(canvas.GetFile() == QStringLiteral("rasterize.lurender.json"));
    CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("raster"));
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("draw")});
    CHECK(canvas.GetExecutionPlan().GetNodeAnnotation(QStringLiteral("draw")).Orders == QVector<int32>{1});
    CHECK(canvas.CaptureViewState().Trace == root.Trace);
    REQUIRE(canvas.GetExecutionPlan().RevealOperation(2) == true);
    CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("second"));
    CHECK(canvas.GetExecutionPlan().GetNodeAnnotation(QStringLiteral("draw")).Orders == QVector<int32>{2});
    REQUIRE(canvas.GetHistory().Back() == true);
    CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("raster"));
    CHECK(canvas.GetExecutionPlan().GetNodeAnnotation(QStringLiteral("draw")).Orders == QVector<int32>{1});
    REQUIRE(canvas.GetHistory().Back() == true);
    CheckView(canvas, root);
    REQUIRE(canvas.GetHistory().Forward() == true);
    CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("raster"));
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
}

TEST_CASE("Plan page activation reveals a collapsed node through shared canvas history with Properties hidden",
          "[editor][render-graph][execution-plan]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QJsonObject plan = OpenWorkspace(workspace, project);
    REQUIRE(workspace.GetDocument().ReplaceCommentFrames(
                GetRoot(),
                GetDefault(),
                {MakeFrame(QStringLiteral("Rendering"), QRectF(850, -100, 1000, 400)),
                 MakeFrame(QStringLiteral("Unused"), QRectF(-40, 360, 1200, 330))}) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    canvas.GetComments().SetCollapsed(QStringLiteral("Rendering"), true);
    canvas.GetComments().SetCollapsed(QStringLiteral("Unused"), true);
    canvas.GetComments().RevealNode(QStringLiteral("unused"));
    const RenderGraphViewState before = SelectTrace(canvas);
    workspace.AcceptResult(MakeResult(workspace, plan));
    QTabWidget* viewsOrNull = workspace.findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
    REQUIRE(viewsOrNull != nullptr);
    viewsOrNull->setCurrentWidget(canvas.GetExecutionPlan().GetPage());
    QTreeWidget& operations = GetOperations(workspace);
    operations.setCurrentItem(operations.topLevelItem(2));
    QPushButton* revealOrNull = workspace.findChild<QPushButton*>(QStringLiteral("renderGraphRevealPlanOperation"));
    REQUIRE(revealOrNull != nullptr);
    REQUIRE(revealOrNull->isEnabled() == true);
    revealOrNull->click();
    CHECK(viewsOrNull->currentWidget() == &canvas);
    CHECK(FindItemOrNull(canvas, QStringLiteral("output"))->isVisible() == true);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("output")});
    CHECK(canvas.CaptureViewState().RevealedFrames ==
          QStringList{QStringLiteral("Unused"), QStringLiteral("Rendering")});
    REQUIRE(canvas.GetHistory().Back() == true);
    CheckView(canvas, before);
    CHECK(FindItemOrNull(canvas, QStringLiteral("output"))->isVisible() == false);
}

TEST_CASE("plan freshness excludes layout and survives failed jobs with unverified and unavailable labels",
          "[editor][render-graph][execution-plan]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QJsonObject plan = OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphExecutionPlan& overlay = canvas.GetExecutionPlan();
    CHECK(overlay.GetFreshness() == RenderGraphPlanFreshness::Unavailable);
    const RenderGraphJobResult accepted = MakeResult(workspace, plan);
    workspace.AcceptResult(accepted);
    REQUIRE(workspace.GetDocument().MoveNodes(GetRoot(), GetDefault(), {{QStringLiteral("raster"), 950, 20}}) ==
            RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    Refresh(workspace);
    CHECK(overlay.GetFreshness() == RenderGraphPlanFreshness::Current);
    QString error;
    REQUIRE(workspace.GetDocument().RenameNode(GetRoot(),
                                               GetDefault(),
                                               QStringLiteral("unused"),
                                               QStringLiteral("new_unused"),
                                               error) == RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    Refresh(workspace);
    CHECK(overlay.GetFreshness() == RenderGraphPlanFreshness::Stale);
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("new_unused")).IsPruned == false);
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("new_unused")).Label.contains(QStringLiteral("stale")) == true);
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("raster")).Orders == QVector<int32>{1});
    RenderGraphJobResult failed = accepted;
    failed.IsSuccessful = false;
    failed.Code = QStringLiteral("InvalidGraph");
    workspace.AcceptResult(failed);
    CHECK(GetOperations(workspace).topLevelItemCount() == 3);
    failed.IsCancelled = true;
    workspace.AcceptResult(failed);
    CHECK(GetOperations(workspace).topLevelItemCount() == 3);
    RenderGraphJobResult unverified = accepted;
    unverified.Request.InputIdentity.clear();
    workspace.AcceptResult(unverified);
    CHECK(overlay.GetFreshness() == RenderGraphPlanFreshness::Unverified);
    CHECK(overlay.GetNodeAnnotation(QStringLiteral("new_unused")).HasActivity == false);
    overlay.Reset();
    CHECK(overlay.GetFreshness() == RenderGraphPlanFreshness::Unavailable);
}

TEST_CASE("missing provenance and malformed plans never replace the last accepted model or navigate changed owners",
          "[editor][render-graph][execution-plan]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QJsonObject plan = OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    workspace.AcceptResult(MakeResult(workspace, plan));
    for (const QString& key : {QStringLiteral("activity"), QStringLiteral("source"), QStringLiteral("version")})
    {
        QJsonObject malformed = plan;
        malformed.remove(key);
        workspace.AcceptResult(MakeResult(workspace, malformed));
        CHECK(GetOperations(workspace).topLevelItemCount() == 3);
        CHECK(canvas.GetExecutionPlan().GetNodeAnnotation(QStringLiteral("raster")).Orders == QVector<int32>{1});
    }
    QJsonArray tooMany;
    const QJsonValue operation = plan.value(QStringLiteral("passes")).toArray().first();
    for (int32 index = 0; index <= plan_policy::MAX_PASSES; ++index)
    {
        tooMany.append(operation);
    }
    QJsonObject oversized = plan;
    oversized.insert(QStringLiteral("passes"), tooMany);
    workspace.AcceptResult(MakeResult(workspace, oversized));
    CHECK(GetOperations(workspace).topLevelItemCount() == 3);
    const RenderGraphViewState before = SelectTrace(canvas);
    const int32 visits = canvas.GetHistory().GetEntryCount();
    QString error;
    REQUIRE(workspace.GetDocument().RenameNode(GetRoot(),
                                               GetDefault(),
                                               QStringLiteral("raster"),
                                               QStringLiteral("renamed"),
                                               error) == RenderGraphDocumentStatus::Ok);
    Refresh(workspace);
    CHECK(canvas.GetExecutionPlan().RevealOperation(1) == false);
    CHECK(canvas.GetHistory().GetEntryCount() == visits);
    CheckView(canvas, before);
    CHECK(canvas.GetExecutionPlan().RevealOperation(0) == false);
    CHECK(canvas.GetExecutionPlan().RevealOperation(4) == false);
}

TEST_CASE("direct library views cannot borrow a caller's execution order and package results select one root plan",
          "[editor][render-graph][execution-plan]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    const QJsonObject plan = OpenWorkspace(workspace, project);
    RenderGraphJobResult result = MakeResult(workspace, plan);
    result.Payload = {{QStringLiteral("plans"), QJsonArray{plan}}};
    workspace.AcceptResult(result);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    REQUIRE(canvas.GetExecutionPlan().RevealOperation(1) == true);
    canvas.ShowConfiguration(GetRoot(), GetDefault());
    canvas.ShowConfiguration(QStringLiteral("rasterize.lurender.json"), QStringLiteral("ColorAndDepth"));
    CHECK(canvas.GetExecutionPlan().GetNodeAnnotation(QStringLiteral("draw")).HasActivity == false);
    CHECK(canvas.GetExecutionPlan().GetNodeAnnotation(QStringLiteral("draw")).Label ==
          QStringLiteral("Plan unavailable"));
    REQUIRE(canvas.GetExecutionPlan().RevealOperation(2) == true);
    CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("second"));
}
