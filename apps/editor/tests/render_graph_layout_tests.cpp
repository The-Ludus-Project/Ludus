#include "internal/render_graph_layout.h"
#include "internal/render_graph_navigation.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QPixmap>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <limits>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
QString GetRoot()
{
    return QStringLiteral("main.lurender.json");
}

QPoint GetPosition(const QVector<RenderGraphNodePosition>& positions, const QString& identity)
{
    for (const RenderGraphNodePosition& position : positions)
    {
        if (position.Node == identity)
        {
            return QPoint(position.X, position.Y);
        }
    }
    FAIL("Missing computed position");
    return {};
}

void OpenWorkspace(RendererWorkspace& inoutWorkspace, const QTemporaryDir& project)
{
    const QByteArray bytes = QByteArrayLiteral(R"JSON({"version":2,"id":"Main","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"output.color","required":false}],
      "nodes":[{"id":"first","kind":"ToneMap","exposure":1},
               {"id":"second","kind":"ToneMap","exposure":1},
               {"id":"third","kind":"ToneMap","exposure":1},
               {"id":"output","kind":"ToneMap","exposure":1}],
      "edges":[{"from":"input.color","to":"first.color"},{"from":"first.color","to":"second.color"},
               {"from":"second.color","to":"third.color"},{"from":"third.color","to":"output.color"}],
      "layout":{"positions":[{"node":"first","x":300,"y":220},{"node":"second","x":100,"y":50},
                                 {"node":"third","x":700,"y":420},{"node":"output","x":-500,"y":-200}]}}]})JSON");
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

void SelectTrace(RenderGraphCanvas& inoutCanvas)
{
    REQUIRE(inoutCanvas.RestoreViewState({{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third")},
                                          QPointF(250, 200),
                                          0.8,
                                          RenderGraphTraceDirection::Downstream,
                                          {}}) == true);
}
} // namespace

TEST_CASE("graph arrangement handles cycles, disconnected cards, varied heights and stable ordering",
          "[editor][render-graph][layout]")
{
    QVector<RenderGraphLayoutNode> nodes{{QStringLiteral("c"), QRectF(0, 0, 100, 120)},
                                         {QStringLiteral("b"), QRectF(0, 0, 180, 340)},
                                         {QStringLiteral("a"), QRectF(0, 0, 100, 80)},
                                         {QStringLiteral("d"), QRectF(0, 0, 100, 90)},
                                         {QStringLiteral("e"), QRectF(0, 0, 100, 90)}};
    QVector<RenderGraphLayoutEdge> edges{{QStringLiteral("a"), QStringLiteral("b")},
                                         {QStringLiteral("b"), QStringLiteral("a")},
                                         {QStringLiteral("b"), QStringLiteral("c")},
                                         {QStringLiteral("c"), QStringLiteral("d")},
                                         {QStringLiteral("missing"), QStringLiteral("a")},
                                         {QStringLiteral("e"), QStringLiteral("e")}};
    QVector<RenderGraphNodePosition> positions;
    REQUIRE(ComputeRenderGraphLayout(nodes, edges, RenderGraphLayoutOperation::Arrange, positions) == true);
    CHECK(GetPosition(positions, QStringLiteral("a")).x() == GetPosition(positions, QStringLiteral("b")).x());
    CHECK(GetPosition(positions, QStringLiteral("c")).x() > GetPosition(positions, QStringLiteral("b")).x() + 180);
    CHECK(GetPosition(positions, QStringLiteral("d")).x() > GetPosition(positions, QStringLiteral("c")).x());
    for (qsizetype first = 0; first < nodes.size(); ++first)
    {
        for (qsizetype second = first + 1; second < nodes.size(); ++second)
        {
            const QRectF firstCard(GetPosition(positions, nodes.at(first).Identity), nodes.at(first).Bounds.size());
            const QRectF secondCard(GetPosition(positions, nodes.at(second).Identity), nodes.at(second).Bounds.size());
            CHECK(firstCard.intersects(secondCard) == false);
        }
    }
    nodes.swapItemsAt(0, 2);
    edges.swapItemsAt(0, 3);
    QVector<RenderGraphNodePosition> reordered;
    REQUIRE(ComputeRenderGraphLayout(nodes, edges, RenderGraphLayoutOperation::Arrange, reordered) == true);
    for (const RenderGraphLayoutNode& node : nodes)
    {
        CHECK(GetPosition(reordered, node.Identity) == GetPosition(positions, node.Identity));
    }
}

TEST_CASE("graph alignment uses outer bounds and distribution uses centres with fixed endpoints",
          "[editor][render-graph][layout]")
{
    const QVector<RenderGraphLayoutNode> nodes{{QStringLiteral("a"), QRectF(-20, 10, 80, 100)},
                                               {QStringLiteral("b"), QRectF(100, 100, 120, 200)},
                                               {QStringLiteral("c"), QRectF(480, 510, 100, 80)}};
    const QVector<RenderGraphLayoutOperation> operations{RenderGraphLayoutOperation::AlignLeft,
                                                         RenderGraphLayoutOperation::AlignHorizontalCentre,
                                                         RenderGraphLayoutOperation::AlignRight,
                                                         RenderGraphLayoutOperation::AlignTop,
                                                         RenderGraphLayoutOperation::AlignVerticalCentre,
                                                         RenderGraphLayoutOperation::AlignBottom,
                                                         RenderGraphLayoutOperation::DistributeHorizontal,
                                                         RenderGraphLayoutOperation::DistributeVertical};
    for (const RenderGraphLayoutOperation operation : operations)
    {
        QVector<RenderGraphNodePosition> positions;
        REQUIRE(ComputeRenderGraphLayout(nodes, {}, operation, positions) == true);
        for (const RenderGraphLayoutNode& node : nodes)
        {
            const QPoint position = GetPosition(positions, node.Identity);
            const QRectF card(position, node.Bounds.size());
            switch (operation)
            {
                case RenderGraphLayoutOperation::AlignLeft:
                    CHECK(card.left() == -20);
                    CHECK(card.y() == node.Bounds.y());
                    break;
                case RenderGraphLayoutOperation::AlignHorizontalCentre:
                    CHECK(card.center().x() == 280);
                    break;
                case RenderGraphLayoutOperation::AlignRight:
                    CHECK(card.right() == 580);
                    break;
                case RenderGraphLayoutOperation::AlignTop:
                    CHECK(card.top() == 10);
                    CHECK(card.x() == node.Bounds.x());
                    break;
                case RenderGraphLayoutOperation::AlignVerticalCentre:
                    CHECK(card.center().y() == 300);
                    break;
                case RenderGraphLayoutOperation::AlignBottom:
                    CHECK(card.bottom() == 590);
                    break;
                case RenderGraphLayoutOperation::DistributeHorizontal:
                    CHECK(card.y() == node.Bounds.y());
                    CHECK(card.center().x() == (node.Identity == QStringLiteral("a")   ? 20
                                                : node.Identity == QStringLiteral("b") ? 275
                                                                                       : 530));
                    break;
                case RenderGraphLayoutOperation::DistributeVertical:
                    CHECK(card.x() == node.Bounds.x());
                    CHECK(card.center().y() == (node.Identity == QStringLiteral("a")   ? 60
                                                : node.Identity == QStringLiteral("b") ? 305
                                                                                       : 550));
                    break;
                default:
                    FAIL("Unexpected operation");
            }
        }
    }
}

TEST_CASE("graph layout rejects invalid and oversized input without publishing partial positions",
          "[editor][render-graph][layout]")
{
    QVector<RenderGraphNodePosition> positions{{QStringLiteral("sentinel"), 1, 2}};
    const QVector<RenderGraphLayoutNode> one{{QStringLiteral("a"), QRectF(0, 0, 100, 100)}};
    CHECK(ComputeRenderGraphLayout({}, {}, RenderGraphLayoutOperation::Arrange, positions) == false);
    CHECK(ComputeRenderGraphLayout(one, {}, RenderGraphLayoutOperation::AlignLeft, positions) == false);
    QVector<RenderGraphLayoutNode> two = one;
    two.append({QStringLiteral("b"), QRectF(0, 0, 100, 100)});
    CHECK(ComputeRenderGraphLayout(two, {}, RenderGraphLayoutOperation::DistributeHorizontal, positions) == false);
    two.last().Identity = QStringLiteral("a");
    CHECK(ComputeRenderGraphLayout(two, {}, RenderGraphLayoutOperation::Arrange, positions) == false);
    two.last().Identity = QStringLiteral("b");
    two.last().Bounds.setX(std::numeric_limits<float64>::quiet_NaN());
    CHECK(ComputeRenderGraphLayout(two, {}, RenderGraphLayoutOperation::Arrange, positions) == false);
    QVector<RenderGraphLayoutNode> oversized;
    for (int32 index = 0; index < RenderGraphDocument::MAX_NODES; ++index)
    {
        oversized.append({QString::number(index), QRectF(0, 0, 100, 1000)});
    }
    CHECK(ComputeRenderGraphLayout(oversized, {}, RenderGraphLayoutOperation::Arrange, positions) == false);
    REQUIRE(positions.size() == 1);
    CHECK(positions.first().Node == QStringLiteral("sentinel"));
    const QVector<RenderGraphLayoutNode> alignmentBoundary{{QStringLiteral("a"), QRectF(0, 32760, 100, 100)},
                                                           {QStringLiteral("b"), QRectF(0, 0, 100, 10)}};
    CHECK(ComputeRenderGraphLayout(alignmentBoundary, {}, RenderGraphLayoutOperation::AlignBottom, positions) == false);
    CHECK(positions.first().Node == QStringLiteral("sentinel"));
    QVector<RenderGraphLayoutNode> boundary{{QStringLiteral("a"), QRectF(32760, 32760, 100, 100)},
                                            {QStringLiteral("b"), QRectF(32760, 32760, 100, 100)}};
    REQUIRE(ComputeRenderGraphLayout(boundary, {}, RenderGraphLayoutOperation::Arrange, positions) == true);
    CHECK(GetPosition(positions, QStringLiteral("b")).y() == RenderGraphDocument::MAX_COORDINATE);
    CHECK(GetPosition(positions, QStringLiteral("b")).y() - GetPosition(positions, QStringLiteral("a")).y() == 128);
}

TEST_CASE("canvas layout commits one undoable saved edit and retains selection and dependency trace",
          "[editor][render-graph][layout]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    SelectTrace(canvas);
    const RenderGraphViewState initialView = canvas.CaptureViewState();
    const RenderGraphNavigation* navigationOrNull = canvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigationOrNull != nullptr);
    const RenderGraphDependencyTrace initialTrace = navigationOrNull->GetTrace();
    const QByteArray initial = document.GetDraft(GetRoot());
    const QPointF unselected = canvas.GetNodePosition(QStringLiteral("output"));
    const uint64 revision = document.GetRevision();
    QSignalSpy edits(&canvas, &RenderGraphCanvas::MoveRequested);
    REQUIRE(canvas.ApplyLayout(RenderGraphLayoutOperation::Arrange) == true);
    CHECK(edits.count() == 1);
    CHECK(document.GetRevision() == revision + 1);
    CHECK(canvas.GetNodePosition(QStringLiteral("output")) == unselected);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")).x() < canvas.GetNodePosition(QStringLiteral("second")).x());
    CHECK(canvas.GetNodePosition(QStringLiteral("second")).x() < canvas.GetNodePosition(QStringLiteral("third")).x());
    CHECK(canvas.GetSelectedNodes() == initialView.Selection);
    CHECK(canvas.CaptureViewState().Trace == initialView.Trace);
    CHECK(navigationOrNull->GetTrace().Nodes == initialTrace.Nodes);
    CHECK(navigationOrNull->GetTrace().Edges == initialTrace.Edges);
    const QString captureDirectory = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_LAYOUT_CAPTURE");
    if (captureDirectory.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(captureDirectory) == true);
        QCoreApplication::processEvents();
        REQUIRE(canvas.grab().save(QDir(captureDirectory).filePath(QStringLiteral("arranged-selection.png"))) == true);
    }
    const QByteArray arranged = document.GetDraft(GetRoot());
    QJsonObject before = QJsonDocument::fromJson(initial).object();
    QJsonObject after = QJsonDocument::fromJson(arranged).object();
    QJsonArray beforeConfigurations = before.value(QStringLiteral("configurations")).toArray();
    QJsonArray afterConfigurations = after.value(QStringLiteral("configurations")).toArray();
    QJsonObject beforeConfiguration = beforeConfigurations.first().toObject();
    QJsonObject afterConfiguration = afterConfigurations.first().toObject();
    beforeConfiguration.remove(QStringLiteral("layout"));
    afterConfiguration.remove(QStringLiteral("layout"));
    CHECK(beforeConfiguration == afterConfiguration);
    workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == initial);
    CHECK(canvas.GetSelectedNodes() == initialView.Selection);
    CHECK(canvas.CaptureViewState().Trace == initialView.Trace);
    CHECK(navigationOrNull->GetTrace().Nodes == initialTrace.Nodes);
    CHECK(navigationOrNull->GetTrace().Edges == initialTrace.Edges);
    workspace.Redo();
    CHECK(document.GetDraft(GetRoot()) == arranged);
    CHECK(canvas.GetSelectedNodes() == initialView.Selection);
    CHECK(canvas.CaptureViewState().Trace == initialView.Trace);
    CHECK(navigationOrNull->GetTrace().Nodes == initialTrace.Nodes);
    CHECK(navigationOrNull->GetTrace().Edges == initialTrace.Edges);
    const uint64 beforeNoop = document.GetRevision();
    REQUIRE(canvas.ApplyLayout(RenderGraphLayoutOperation::Arrange) == true);
    CHECK(document.GetRevision() == beforeNoop);
    CHECK(edits.count() == 1);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(GetRoot()) == arranged);
    canvas.BeginDrag(initialView.Selection);
    CHECK(canvas.ApplyLayout(RenderGraphLayoutOperation::AlignLeft) == false);
    canvas.EndDrag();
    CHECK(document.GetRevision() == beforeNoop);
}

TEST_CASE("canvas context Layout actions work with Properties hidden and preserve group selection",
          "[editor][render-graph][layout]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    SelectTrace(canvas);
    const QStringList selection = canvas.GetSelectedNodes();
    const QPointF unselected = canvas.GetNodePosition(QStringLiteral("output"));
    const QVector<QString> actions{QStringLiteral("renderGraphAlignLeft"),
                                   QStringLiteral("renderGraphAlignHorizontalCentre"),
                                   QStringLiteral("renderGraphAlignRight"),
                                   QStringLiteral("renderGraphAlignTop"),
                                   QStringLiteral("renderGraphAlignVerticalCentre"),
                                   QStringLiteral("renderGraphAlignBottom"),
                                   QStringLiteral("renderGraphDistributeHorizontal"),
                                   QStringLiteral("renderGraphDistributeVertical"),
                                   QStringLiteral("renderGraphArrangeSelection"),
                                   QStringLiteral("renderGraphArrangeAll")};
    for (const QString& name : actions)
    {
        SelectTrace(canvas);
        const uint64 revision = workspace.GetDocument().GetRevision();
        const RenderGraphViewState beforeView = canvas.CaptureViewState();
        QTimer::singleShot(0, &workspace, [&workspace, name]() {
            QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("renderGraphBackgroundMenu"));
            REQUIRE(menuOrNull != nullptr);
            QAction* actionOrNull = menuOrNull->findChild<QAction*>(name);
            REQUIRE(actionOrNull != nullptr);
            REQUIRE(actionOrNull->isEnabled() == true);
            menuOrNull->close();
            actionOrNull->trigger();
        });
        const QPoint point = canvas.mapFromScene(QPointF(-800, -500));
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QCoreApplication::sendEvent(canvas.viewport(), &event);
        CHECK(canvas.GetSelectedNodes() == selection);
        CHECK(canvas.CaptureViewState().Trace == RenderGraphTraceDirection::Downstream);
        CHECK(workspace.GetDocument().GetRevision() == revision + 1);
        if (name != QStringLiteral("renderGraphArrangeAll"))
        {
            CHECK(canvas.GetNodePosition(QStringLiteral("output")) == unselected);
        }
        if (name.startsWith(QStringLiteral("renderGraphArrange")) == false)
        {
            const RenderGraphViewState afterView = canvas.CaptureViewState();
            CHECK(afterView.Zoom == beforeView.Zoom);
            CHECK((afterView.CentreScene - beforeView.CentreScene).manhattanLength() * beforeView.Zoom < 2);
        }
        workspace.Undo();
    }
    canvas.SelectNodes({});
    QMenu empty;
    canvas.AddLayoutMenu(empty);
    CHECK(empty.findChild<QAction*>(QStringLiteral("renderGraphArrangeSelection"))->isEnabled() == false);
    CHECK(empty.findChild<QAction*>(QStringLiteral("renderGraphArrangeAll"))->isEnabled() == true);
    canvas.SelectNodes({QStringLiteral("first"), QStringLiteral("second")});
    QMenu pair;
    canvas.AddLayoutMenu(pair);
    CHECK(pair.findChild<QAction*>(QStringLiteral("renderGraphAlignLeft"))->isEnabled() == true);
    CHECK(pair.findChild<QAction*>(QStringLiteral("renderGraphDistributeHorizontal"))->isEnabled() == false);
}

TEST_CASE("all-node arrangement preserves empty selection and excludes interface positions",
          "[editor][render-graph][layout]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.SelectNodes({});
    const QPointF input = canvas.GetNodePosition(QStringLiteral("input"));
    const uint64 revision = workspace.GetDocument().GetRevision();
    REQUIRE(canvas.ApplyLayout(RenderGraphLayoutOperation::Arrange, true) == true);
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    CHECK(canvas.GetSelectedNodes().isEmpty() == true);
    CHECK(canvas.CaptureViewState().Trace == RenderGraphTraceDirection::None);
    CHECK(canvas.GetNodePosition(QStringLiteral("input")) == input);
    const QJsonArray positions = QJsonDocument::fromJson(workspace.GetDocument().GetDraft(GetRoot()))
                                     .object()
                                     .value(QStringLiteral("configurations"))
                                     .toArray()
                                     .first()
                                     .toObject()
                                     .value(QStringLiteral("layout"))
                                     .toObject()
                                     .value(QStringLiteral("positions"))
                                     .toArray();
    REQUIRE(positions.size() == 4);
    for (const QJsonValueConstRef position : positions)
    {
        CHECK(position.toObject().value(QStringLiteral("node")).toString() != QStringLiteral("input"));
        CHECK(position.toObject().value(QStringLiteral("node")).toString() != QStringLiteral("#outputs"));
    }
    workspace.Undo();
    CHECK(workspace.GetDocument().IsDirty() == false);
}

TEST_CASE("bounded arrangement handles a maximum-size cycle and rejects excess cards", "[editor][render-graph][layout]")
{
    QVector<RenderGraphLayoutNode> nodes;
    QVector<RenderGraphLayoutEdge> edges;
    for (int32 index = 0; index < RenderGraphDocument::MAX_NODES; ++index)
    {
        nodes.append({QString::number(index), QRectF(0, 0, 100, 100)});
        edges.append({QString::number(index), QString::number((index + 1) % RenderGraphDocument::MAX_NODES)});
    }
    QVector<RenderGraphNodePosition> positions;
    REQUIRE(ComputeRenderGraphLayout(nodes, edges, RenderGraphLayoutOperation::Arrange, positions) == true);
    CHECK(positions.size() == RenderGraphDocument::MAX_NODES);
    for (const RenderGraphNodePosition& position : positions)
    {
        CHECK(position.X == 0);
        CHECK(position.Y >= 0);
        CHECK(position.Y <= RenderGraphDocument::MAX_COORDINATE);
    }
    nodes.append({QStringLiteral("excess"), QRectF(0, 0, 100, 100)});
    CHECK(ComputeRenderGraphLayout(nodes, edges, RenderGraphLayoutOperation::Arrange, positions) == false);
    CHECK(positions.size() == RenderGraphDocument::MAX_NODES);
}
