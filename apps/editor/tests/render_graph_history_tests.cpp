#include "internal/render_graph_history.h"
#include "internal/render_graph_navigation.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

#include <limits>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
QString GetRoot()
{
    return QStringLiteral("main.lurender.json");
}
QString GetChild()
{
    return QStringLiteral("child.lurender.json");
}
QString GetGrandchild()
{
    return QStringLiteral("grandchild.lurender.json");
}
QString GetDefault()
{
    return QStringLiteral("Default");
}

QString MakePackage(const QTemporaryDir& project)
{
    const QByteArray root = QByteArrayLiteral(R"JSON({"version":2,"id":"Main","default":"Default","configurations":[{
      "id":"Default","inputs":[],"outputs":[{"id":"color","type":"ColorTexture","source":"left.color","required":false}],
      "nodes":[{"id":"texture","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm","extent":[96,64],"samples":1,"imported":false,"initialized":false,"presentation":false},
        {"id":"left","kind":"Subgraph","asset":"child.lurender.json","configuration":"Default"},
        {"id":"right","kind":"Subgraph","asset":"child.lurender.json","configuration":"Default"}],
      "edges":[{"from":"texture.texture","to":"left.color"},{"from":"texture.texture","to":"right.color"}]}]})JSON");
    QJsonObject rootGraph = QJsonDocument::fromJson(root).object();
    QJsonArray configurations = rootGraph.value(QStringLiteral("configurations")).toArray();
    QJsonObject other = configurations.first().toObject();
    other.insert(QStringLiteral("id"), QStringLiteral("Other"));
    configurations.append(other);
    rootGraph.insert(QStringLiteral("configurations"), configurations);
    const QByteArray child = QByteArrayLiteral(R"JSON({"version":2,"id":"Child","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"nested.color","required":false}],
      "nodes":[{"id":"tone","kind":"ToneMap","exposure":1},{"id":"nested","kind":"Subgraph","asset":"grandchild.lurender.json","configuration":"Default"}],
      "edges":[{"from":"input.color","to":"tone.color"},{"from":"tone.color","to":"nested.color"}]}]})JSON");
    const QByteArray grandchild =
        QByteArrayLiteral(R"JSON({"version":2,"id":"Grandchild","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"copy.color","required":false}],
      "nodes":[{"id":"copy","kind":"ToneMap","exposure":1}],"edges":[{"from":"input.color","to":"copy.color"}]}]})JSON");
    const QVector<QByteArray> graphs{EncodeCanonicalJson(rootGraph), child, grandchild};
    const QStringList files{GetRoot(), GetChild(), GetGrandchild()};
    for (qsizetype index = 0; index < files.size(); ++index)
    {
        QFile file(QDir(project.path()).filePath(files.at(index)));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(graphs.at(index)) == graphs.at(index).size());
    }
    return QDir(project.path()).filePath(GetRoot());
}

void OpenWorkspace(RendererWorkspace& inoutWorkspace, const QTemporaryDir& project)
{
    const QString root = MakePackage(project);
    inoutWorkspace.SetProject(project.path(), 1);
    REQUIRE(inoutWorkspace.OpenGraph(root) == true);
    inoutWorkspace.SetLivePreview(false);
    inoutWorkspace.resize(1200, 850);
    inoutWorkspace.show();
    inoutWorkspace.activateWindow();
    QCoreApplication::processEvents();
}

RenderGraphViewState SetView(RenderGraphCanvas& inoutCanvas,
                             const QStringList& selection,
                             QPointF centreScene,
                             float64 zoom,
                             RenderGraphTraceDirection trace)
{
    REQUIRE(inoutCanvas.RestoreViewState({selection, centreScene, zoom, trace}) == true);
    QCoreApplication::processEvents();
    return inoutCanvas.CaptureViewState();
}

void CheckView(const RenderGraphCanvas& canvas, const RenderGraphViewState& expected)
{
    const RenderGraphViewState actual = canvas.CaptureViewState();
    INFO(canvas.GetFile().toUtf8().constData());
    INFO(expected.CentreScene.x());
    INFO(expected.CentreScene.y());
    INFO(actual.CentreScene.x());
    INFO(actual.CentreScene.y());
    INFO(canvas.viewport()->width());
    INFO(canvas.viewport()->height());
    CHECK(actual.Selection == expected.Selection);
    CHECK(qAbs(actual.Zoom - expected.Zoom) < 0.000001);
    // Centre preservation is checked in viewport pixels, including Qt scroll-bar rounding.
    CHECK((actual.CentreScene - expected.CentreScene).manhattanLength() * expected.Zoom < 2);
    CHECK(actual.Trace == expected.Trace);
}

void Enter(RendererWorkspace& inoutWorkspace, const QString& instance)
{
    REQUIRE(inoutWorkspace.GetCanvas().ActivateNode(instance) == true);
    QCoreApplication::processEvents();
}

QAction& GetAction(RenderGraphHistory& inoutHistory, const QString& name)
{
    QAction* actionOrNull = inoutHistory.findChild<QAction*>(name);
    REQUIRE(actionOrNull != nullptr);
    return *actionOrNull;
}
} // namespace

TEST_CASE("graph Back and Forward restore nested view snapshots and synchronize workspace selection",
          "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphHistory& history = canvas.GetHistory();
    REQUIRE(history.GetEntryCount() == 1);
    const QByteArray root = workspace.GetDocument().GetDraft(GetRoot());
    const RenderGraphViewState parent = SetView(canvas,
                                                {QStringLiteral("texture"), QStringLiteral("left")},
                                                QPointF(240, 70),
                                                1.3,
                                                RenderGraphTraceDirection::Downstream);
    Enter(workspace, QStringLiteral("left"));
    REQUIRE(canvas.GetFile() == GetChild());
    CHECK((history.GetPath().first().View.CentreScene - parent.CentreScene).manhattanLength() * parent.Zoom < 2);
    const RenderGraphViewState child =
        SetView(canvas, {QStringLiteral("tone")}, QPointF(180, 110), 0.7, RenderGraphTraceDirection::Upstream);
    Enter(workspace, QStringLiteral("nested"));
    REQUIRE(canvas.GetFile() == GetGrandchild());
    const RenderGraphViewState grandchild =
        SetView(canvas, {QStringLiteral("copy")}, QPointF(30, 40), 1.65, RenderGraphTraceDirection::Downstream);
    REQUIRE(history.GetPath().size() == 3);
    CHECK(history.GetPath().at(1).Instance == QStringLiteral("left"));
    CHECK(history.GetPath().at(2).Instance == QStringLiteral("nested"));
    REQUIRE(history.Back() == true);
    QCoreApplication::processEvents();
    CheckView(canvas, child);
    CHECK(canvas.GetFile() == GetChild());
    CHECK(workspace.GetSelection() == GetChild() + QStringLiteral("#Default/tone"));
    QComboBox* sourceOrNull = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    REQUIRE(sourceOrNull != nullptr);
    CHECK(sourceOrNull->currentText() == GetChild());
    REQUIRE(history.Back() == true);
    QCoreApplication::processEvents();
    CheckView(canvas, parent);
    CHECK(canvas.GetFile() == GetRoot());
    REQUIRE(history.Forward() == true);
    QCoreApplication::processEvents();
    CheckView(canvas, child);
    REQUIRE(history.Forward() == true);
    QCoreApplication::processEvents();
    CheckView(canvas, grandchild);
    CHECK(history.Forward() == false);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == root);
    CHECK(workspace.GetDocument().GetRevision() == 0);
    CHECK(workspace.GetDocument().CanUndo() == false);
}

TEST_CASE("breadcrumbs restore ancestors and distinguish instances of a shared Subgraph",
          "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphHistory& history = canvas.GetHistory();
    const RenderGraphViewState parent =
        SetView(canvas, {QStringLiteral("left")}, QPointF(200, 120), 0.85, RenderGraphTraceDirection::Upstream);
    Enter(workspace, QStringLiteral("left"));
    const RenderGraphViewState left =
        SetView(canvas, {QStringLiteral("tone")}, QPointF(75, 60), 1.7, RenderGraphTraceDirection::Downstream);
    Enter(workspace, QStringLiteral("nested"));
    GetAction(history, QStringLiteral("renderGraphBreadcrumb1")).trigger();
    QCoreApplication::processEvents();
    CHECK(canvas.GetFile() == GetChild());
    CheckView(canvas, left);
    CHECK(history.GetPath().size() == 2);
    CHECK(history.Forward() == false);
    GetAction(history, QStringLiteral("renderGraphBreadcrumb0")).trigger();
    QCoreApplication::processEvents();
    CheckView(canvas, parent);
    Enter(workspace, QStringLiteral("right"));
    const RenderGraphViewState right =
        SetView(canvas, {QStringLiteral("nested")}, QPointF(330, 140), 0.6, RenderGraphTraceDirection::Upstream);
    CHECK(history.GetPath().at(1).Instance == QStringLiteral("right"));
    const int32 entries = history.GetEntryCount();
    QAction& current = GetAction(history, QStringLiteral("renderGraphBreadcrumb1"));
    CHECK(current.isChecked() == true);
    current.trigger();
    CHECK(current.isChecked() == true);
    CHECK(history.GetEntryCount() == entries);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_HISTORY_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(capture) == true);
        REQUIRE(canvas.grab().save(QDir(capture).filePath(QStringLiteral("shared-instance.png"))) == true);
    }
    REQUIRE(history.Back() == true);
    REQUIRE(history.Back() == true);
    QCoreApplication::processEvents();
    CHECK(history.GetPath().at(1).Instance == QStringLiteral("left"));
    CheckView(canvas, left);
    REQUIRE(history.Forward() == true);
    REQUIRE(history.Forward() == true);
    QCoreApplication::processEvents();
    CheckView(canvas, right);
    CHECK(history.GetPath().at(1).Instance == QStringLiteral("right"));
    CHECK(workspace.GetDocument().CanUndo() == false);
}

TEST_CASE("navigation survives viewport focus, resize and pending auto-fit callbacks",
          "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState parent =
        SetView(canvas, {QStringLiteral("texture")}, QPointF(180, 150), 0.55, RenderGraphTraceDirection::Downstream);
    Enter(workspace, QStringLiteral("left"));
    workspace.SetChromeVisible(false);
    workspace.resize(1400, 900);
    QCoreApplication::processEvents();
    CHECK(canvas.GetHistory().isVisible() == true);
    canvas.setFocus();
    QTest::keySequence(&canvas, QKeySequence::Back);
    QCoreApplication::processEvents();
    CHECK(canvas.GetFile() == GetRoot());
    CheckView(canvas, parent);
    QTest::keySequence(&canvas, QKeySequence::Forward);
    QCoreApplication::processEvents();
    CHECK(canvas.GetFile() == GetChild());
    CHECK(workspace.GetDocument().GetRevision() == 0);
}

TEST_CASE("history skips deleted configurations and detaches rebound instance paths", "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    RenderGraphHistory& history = canvas.GetHistory();
    SECTION("deleted ancestor configuration")
    {
        REQUIRE(workspace.SelectNode(GetRoot(), QStringLiteral("Other"), QStringLiteral("left")) == true);
        Enter(workspace, QStringLiteral("left"));
        QString error;
        REQUIRE(document.RemoveConfiguration(GetRoot(), QStringLiteral("Other"), error) ==
                RenderGraphDocumentStatus::Ok);
        REQUIRE(workspace.SelectNode(GetChild(), GetDefault(), QStringLiteral("tone")) == true);
        CHECK(history.GetPath().size() == 1);
        const uint64 revision = document.GetRevision();
        REQUIRE(history.Back() == true);
        CHECK(canvas.GetFile() == GetRoot());
        CHECK(canvas.GetConfiguration() == GetDefault());
        CHECK(document.GetRevision() == revision);
    }
    SECTION("instance now references a different asset")
    {
        Enter(workspace, QStringLiteral("left"));
        QJsonObject graph;
        REQUIRE(document.GetGraph(GetRoot(), graph) == true);
        QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
        QJsonObject record = configurations.first().toObject();
        QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
        QJsonObject node = nodes.at(1).toObject();
        node.insert(QStringLiteral("asset"), GetGrandchild());
        nodes.replace(1, node);
        record.insert(QStringLiteral("nodes"), nodes);
        configurations.replace(0, record);
        graph.insert(QStringLiteral("configurations"), configurations);
        REQUIRE(document.SetText(GetRoot(), EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
        REQUIRE(workspace.SelectNode(GetChild(), GetDefault(), QStringLiteral("tone")) == true);
        CHECK(history.GetPath().size() == 1);
        CHECK(history.GetPath().first().Instance.isEmpty() == true);
        const uint64 revision = document.GetRevision();
        REQUIRE(history.Back() == true);
        REQUIRE(history.Forward() == true);
        CHECK(canvas.GetFile() == GetChild());
        CHECK(document.GetRevision() == revision);
    }
}

TEST_CASE("breadcrumbs repair unambiguous instance renames and avoid guessing ambiguous ones",
          "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    static_cast<void>(
        SetView(canvas, {QStringLiteral("left")}, QPointF(200, 80), 1.1, RenderGraphTraceDirection::Downstream));
    Enter(workspace, QStringLiteral("left"));
    bool hasUniqueBinding = false;
    SECTION("one remaining binding")
    {
        REQUIRE(document.RemoveNode(GetRoot(), GetDefault(), QStringLiteral("right")) == RenderGraphDocumentStatus::Ok);
        hasUniqueBinding = true;
    }
    SECTION("shared asset still has two bindings") {}
    QString error;
    REQUIRE(document.RenameNode(GetRoot(), GetDefault(), QStringLiteral("left"), QStringLiteral("renamed"), error) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.SelectNode(GetChild(), GetDefault(), QStringLiteral("tone")) == true);
    if (hasUniqueBinding == true)
    {
        REQUIRE(canvas.GetHistory().GetPath().size() == 2);
        CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("renamed"));
        REQUIRE(canvas.GetHistory().VisitAncestor(0) == true);
        CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("renamed")});
    }
    else
    {
        CHECK(canvas.GetHistory().GetPath().size() == 1);
        REQUIRE(canvas.GetHistory().Back() == true);
        CHECK(canvas.GetSelectedNodes().isEmpty() == true);
    }
}

TEST_CASE("history bounds visits, clears Forward on a new jump and resets for a new package",
          "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphHistory& history = workspace.GetCanvas().GetHistory();
    for (int32 index = 0; index < RenderGraphHistory::MAX_ENTRIES + 2; ++index)
    {
        REQUIRE(workspace.SelectNode(GetRoot(), index % 2 == 0 ? QStringLiteral("Other") : GetDefault(), {}) == true);
        QCoreApplication::processEvents();
    }
    CHECK(history.GetEntryCount() == RenderGraphHistory::MAX_ENTRIES);
    REQUIRE(history.Back() == true);
    Enter(workspace, QStringLiteral("left"));
    CHECK(history.Forward() == false);
    QTemporaryDir replacement;
    const QString root = MakePackage(replacement);
    workspace.SetProject(replacement.path(), 2);
    REQUIRE(workspace.OpenGraph(root) == true);
    QCoreApplication::processEvents();
    CHECK(history.GetEntryCount() == 1);
    CHECK(history.Back() == false);
    CHECK(history.Forward() == false);
    CHECK(workspace.GetDocument().GetRevision() == 0);
}

TEST_CASE("view snapshots reject invalid values and discard stale selection IDs", "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState original =
        SetView(canvas, {QStringLiteral("left")}, QPointF(120, 140), 1.2, RenderGraphTraceDirection::Upstream);
    for (const float64 zoom :
         {0.0, -1.0, 100.0, std::numeric_limits<float64>::quiet_NaN(), std::numeric_limits<float64>::infinity()})
    {
        RenderGraphViewState invalid = original;
        invalid.Zoom = zoom;
        CHECK(canvas.RestoreViewState(invalid) == false);
        CheckView(canvas, original);
    }
    RenderGraphViewState invalid = original;
    invalid.CentreScene.setX(std::numeric_limits<float64>::infinity());
    CHECK(canvas.RestoreViewState(invalid) == false);
    invalid.CentreScene = QPointF(1e100, 0);
    CHECK(canvas.RestoreViewState(invalid) == false);
    invalid = original;
    invalid.Trace = static_cast<RenderGraphTraceDirection>(255);
    CHECK(canvas.RestoreViewState(invalid) == false);
    CheckView(canvas, original);
    RenderGraphViewState missing = original;
    missing.Selection = {QStringLiteral("deleted")};
    REQUIRE(canvas.RestoreViewState(missing) == true);
    CHECK(canvas.GetSelectedNodes().isEmpty() == true);
    CHECK(canvas.CaptureViewState().Trace == RenderGraphTraceDirection::None);
    const RenderGraphViewState expanded =
        SetView(canvas, {QStringLiteral("left")}, QPointF(-500, -300), 0.2, RenderGraphTraceDirection::Upstream);
    canvas.Rebuild();
    QCoreApplication::processEvents();
    CheckView(canvas, expanded);
    CHECK(workspace.GetDocument().GetRevision() == 0);
}

TEST_CASE("retired breadcrumb actions cannot navigate after the path changes", "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    Enter(workspace, QStringLiteral("left"));
    RenderGraphHistory& history = workspace.GetCanvas().GetHistory();
    QPointer<QAction> oldActionOrNull = &GetAction(history, QStringLiteral("renderGraphBreadcrumb0"));
    REQUIRE(workspace.SelectNode(GetChild(), GetDefault(), QStringLiteral("tone")) == true);
    REQUIRE(oldActionOrNull.isNull() == false);
    oldActionOrNull->trigger();
    CHECK(workspace.GetCanvas().GetFile() == GetChild());
    CHECK(history.GetPath().size() == 2);
}

TEST_CASE("recursive Subgraph navigation has a bounded breadcrumb path", "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphDocument& document = workspace.GetDocument();
    QJsonObject graph;
    REQUIRE(document.GetGraph(GetChild(), graph) == true);
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject record = configurations.first().toObject();
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    QJsonObject nested = nodes.at(1).toObject();
    nested.insert(QStringLiteral("asset"), GetChild());
    nodes.replace(1, nested);
    record.insert(QStringLiteral("nodes"), nodes);
    configurations.replace(0, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(document.SetText(GetChild(), EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    Enter(workspace, QStringLiteral("left"));
    for (int32 depth = 2; depth < RenderGraphHistory::MAX_PATH_DEPTH; ++depth)
    {
        Enter(workspace, QStringLiteral("nested"));
    }
    RenderGraphHistory& history = workspace.GetCanvas().GetHistory();
    REQUIRE(history.GetPath().size() == RenderGraphHistory::MAX_PATH_DEPTH);
    const uint64 revision = document.GetRevision();
    QSignalSpy failures(&history, &RenderGraphHistory::NavigationFailed);
    Enter(workspace, QStringLiteral("nested"));
    CHECK(history.GetPath().size() == RenderGraphHistory::MAX_PATH_DEPTH);
    CHECK(failures.count() == 1);
    CHECK(document.GetRevision() == revision);
}
