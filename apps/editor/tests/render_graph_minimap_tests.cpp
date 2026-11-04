#include "internal/render_graph_canvas.h"
#include "internal/render_graph_comments.h"
#include "internal/render_graph_layout.h"
#include "internal/render_graph_minimap.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPixmap>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QTest>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 CHAIN_COUNT = 8;
constexpr int32 CHAIN_LENGTH = 3;
constexpr int32 SCRAMBLE_MULTIPLIER = 1103515245;
constexpr int32 SCRAMBLE_INCREMENT = 12345;
constexpr int32 SCRAMBLE_RANGE_X = 2400;
constexpr int32 SCRAMBLE_RANGE_Y = 1500;
constexpr int32 CANVAS_WIDTH = 1000;
constexpr int32 CANVAS_HEIGHT = 700;

QString GetRoot()
{
    return QStringLiteral("main.lurender.json");
}

QJsonObject MakeNode(const QString& identity, const QString& kind)
{
    QJsonObject node{{QStringLiteral("id"), identity}, {QStringLiteral("kind"), kind}};
    if (kind == QStringLiteral("Target"))
    {
        node.insert(QStringLiteral("type"), QStringLiteral("ColorTexture"));
        node.insert(QStringLiteral("format"), QStringLiteral("RGBA8Unorm"));
        node.insert(QStringLiteral("extent"), QJsonArray{96, 64});
        node.insert(QStringLiteral("samples"), 1);
        node.insert(QStringLiteral("imported"), false);
        node.insert(QStringLiteral("initialized"), false);
        node.insert(QStringLiteral("presentation"), false);
    }
    else if (kind == QStringLiteral("ToneMap"))
    {
        node.insert(QStringLiteral("exposure"), 1);
    }
    else
    {
        node.insert(QStringLiteral("type"), QStringLiteral("ColorTexture"));
    }
    return node;
}

/// 8 targets, each feeding a chain of three tone maps and two exports (one from the chain's end, one straight
/// from the target): 8 + 24 + 16 = 48 cards. Authored positions are scrambled, like a graph edited for a while.
QJsonObject MakeLargeGraph(bool hasScrambledPositions)
{
    QJsonArray nodes;
    QJsonArray edges;
    QJsonArray positions;
    uint32 state = 7;
    const auto place = [&](const QString& identity) {
        state = state * static_cast<uint32>(SCRAMBLE_MULTIPLIER) + static_cast<uint32>(SCRAMBLE_INCREMENT);
        const int32 x = static_cast<int32>((state >> 8) % static_cast<uint32>(SCRAMBLE_RANGE_X));
        state = state * static_cast<uint32>(SCRAMBLE_MULTIPLIER) + static_cast<uint32>(SCRAMBLE_INCREMENT);
        const int32 y = static_cast<int32>((state >> 8) % static_cast<uint32>(SCRAMBLE_RANGE_Y));
        positions.append(
            QJsonObject{{QStringLiteral("node"), identity}, {QStringLiteral("x"), x}, {QStringLiteral("y"), y}});
    };
    // Scrambled names, so identity order is unrelated to dependency order.
    for (int32 chain = 0; chain < CHAIN_COUNT; ++chain)
    {
        const QString target = QStringLiteral("t%1").arg((chain * 5) % CHAIN_COUNT);
        nodes.append(MakeNode(target, QStringLiteral("Target")));
        place(target);
        QString previous = target + QStringLiteral(".texture");
        for (int32 step = 0; step < CHAIN_LENGTH; ++step)
        {
            const QString tone = QStringLiteral("m%1_%2").arg((chain * 5) % CHAIN_COUNT).arg(CHAIN_LENGTH - step);
            nodes.append(MakeNode(tone, QStringLiteral("ToneMap")));
            place(tone);
            edges.append(QJsonObject{{QStringLiteral("from"), previous},
                                     {QStringLiteral("to"), tone + QStringLiteral(".color")}});
            previous = tone + QStringLiteral(".color");
        }
        const QString finalExport = QStringLiteral("x%1_b").arg((chain * 5) % CHAIN_COUNT);
        const QString previewExport = QStringLiteral("x%1_a").arg((chain * 5) % CHAIN_COUNT);
        nodes.append(MakeNode(finalExport, QStringLiteral("Export")));
        nodes.append(MakeNode(previewExport, QStringLiteral("Export")));
        place(finalExport);
        place(previewExport);
        edges.append(QJsonObject{{QStringLiteral("from"), previous},
                                 {QStringLiteral("to"), finalExport + QStringLiteral(".texture")}});
        edges.append(QJsonObject{{QStringLiteral("from"), target + QStringLiteral(".texture")},
                                 {QStringLiteral("to"), previewExport + QStringLiteral(".texture")}});
    }
    QJsonObject configuration{{QStringLiteral("id"), QStringLiteral("Default")},
                              {QStringLiteral("inputs"), QJsonArray{}},
                              {QStringLiteral("outputs"), QJsonArray{}},
                              {QStringLiteral("nodes"), nodes},
                              {QStringLiteral("edges"), edges}};
    if (hasScrambledPositions == true)
    {
        configuration.insert(QStringLiteral("layout"), QJsonObject{{QStringLiteral("positions"), positions}});
    }
    return QJsonObject{{QStringLiteral("version"), 2},
                       {QStringLiteral("id"), QStringLiteral("Main")},
                       {QStringLiteral("default"), QStringLiteral("Default")},
                       {QStringLiteral("configurations"), QJsonArray{configuration}}};
}

void WriteGraph(const QTemporaryDir& project, const QJsonObject& graph)
{
    QFile file(QDir(project.path()).filePath(GetRoot()));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    const QByteArray bytes = QJsonDocument(graph).toJson(QJsonDocument::Compact);
    REQUIRE(file.write(bytes) == bytes.size());
}

void ShowCanvas(RenderGraphCanvas& inoutCanvas)
{
    inoutCanvas.resize(CANVAS_WIDTH, CANVAS_HEIGHT);
    inoutCanvas.ShowConfiguration(GetRoot(), QStringLiteral("Default"));
    inoutCanvas.show();
    QCoreApplication::processEvents();
}

/// Proper crossings between wires drawn from a card's right edge to the next card's left edge.
int32 CountCrossings(const QVector<RenderGraphLayoutNode>& nodes,
                     const QVector<RenderGraphLayoutEdge>& edges,
                     const QVector<RenderGraphNodePosition>& positions)
{
    QHash<QString, QRectF> cards;
    for (const RenderGraphLayoutNode& node : nodes)
    {
        QRectF bounds = node.Bounds;
        for (const RenderGraphNodePosition& position : positions)
        {
            if (position.Node == node.Identity)
            {
                bounds.moveTo(position.X, position.Y);
            }
        }
        cards.insert(node.Identity, bounds);
    }
    QVector<QLineF> wires;
    for (const RenderGraphLayoutEdge& edge : edges)
    {
        wires.append(QLineF(QPointF(cards.value(edge.Source).right(), cards.value(edge.Source).center().y()),
                            QPointF(cards.value(edge.Destination).left(), cards.value(edge.Destination).center().y())));
    }
    int32 crossings = 0;
    for (qsizetype first = 0; first < wires.size(); ++first)
    {
        for (qsizetype second = first + 1; second < wires.size(); ++second)
        {
            QPointF at;
            const bool isShared =
                wires.at(first).p1() == wires.at(second).p1() || wires.at(first).p2() == wires.at(second).p2();
            if (isShared == false && wires.at(first).intersects(wires.at(second), &at) == QLineF::BoundedIntersection)
            {
                ++crossings;
            }
        }
    }
    return crossings;
}

void LoadLayoutInputs(const QJsonObject& graph,
                      QVector<RenderGraphLayoutNode>& outNodes,
                      QVector<RenderGraphLayoutEdge>& outEdges)
{
    const QJsonObject configuration = graph.value(QStringLiteral("configurations")).toArray().first().toObject();
    for (const QJsonValueConstRef node : configuration.value(QStringLiteral("nodes")).toArray())
    {
        outNodes.append({node.toObject().value(QStringLiteral("id")).toString(), QRectF(0, 0, 220, 120)});
    }
    for (const QJsonValueConstRef edge : configuration.value(QStringLiteral("edges")).toArray())
    {
        outEdges.append({edge.toObject().value(QStringLiteral("from")).toString().section(QLatin1Char('.'), 0, 0),
                         edge.toObject().value(QStringLiteral("to")).toString().section(QLatin1Char('.'), 0, 0)});
    }
}
} // namespace

TEST_CASE("auto layout untangles a 48-card graph into dependency-ordered columns without crossings",
          "[editor][render-graph][layout][minimap]")
{
    QVector<RenderGraphLayoutNode> nodes;
    QVector<RenderGraphLayoutEdge> edges;
    LoadLayoutInputs(MakeLargeGraph(false), nodes, edges);
    REQUIRE(nodes.size() == 48);
    QVector<RenderGraphNodePosition> positions;
    REQUIRE(ComputeRenderGraphLayout(nodes, edges, RenderGraphLayoutOperation::Arrange, positions) == true);
    // Every edge runs left to right.
    QHash<QString, int32> columns;
    for (const RenderGraphNodePosition& position : positions)
    {
        columns.insert(position.Node, position.X);
    }
    for (const RenderGraphLayoutEdge& edge : edges)
    {
        CHECK(columns.value(edge.Source) < columns.value(edge.Destination));
    }
    // Eight independent chains: a good layout has none crossing.
    CHECK(CountCrossings(nodes, edges, positions) == 0);
    // Same input in another order gives the same answer.
    QVector<RenderGraphLayoutNode> reversedNodes;
    QVector<RenderGraphLayoutEdge> reversedEdges;
    for (qsizetype index = nodes.size() - 1; index >= 0; --index)
    {
        reversedNodes.append(nodes.at(index));
    }
    for (qsizetype index = edges.size() - 1; index >= 0; --index)
    {
        reversedEdges.append(edges.at(index));
    }
    QVector<RenderGraphNodePosition> reversed;
    REQUIRE(ComputeRenderGraphLayout(reversedNodes, reversedEdges, RenderGraphLayoutOperation::Arrange, reversed) ==
            true);
    for (const RenderGraphNodePosition& position : positions)
    {
        CHECK(QPoint(position.X, position.Y) == [&]() {
            for (const RenderGraphNodePosition& other : reversed)
            {
                if (other.Node == position.Node)
                {
                    return QPoint(other.X, other.Y);
                }
            }
            return QPoint(-1, -1);
        }());
    }
}

TEST_CASE("auto layout reduces crossings on a hub graph and places an unfed hub beside its first consumer",
          "[editor][render-graph][layout][minimap]")
{
    // Two hubs feed twelve passes in scrambled order; passes feed a shared sink chain.
    QVector<RenderGraphLayoutNode> nodes;
    QVector<RenderGraphLayoutEdge> edges;
    nodes.append({QStringLiteral("scene"), QRectF(0, 0, 220, 120)});
    nodes.append({QStringLiteral("camera"), QRectF(0, 0, 220, 120)});
    nodes.append({QStringLiteral("zsink"), QRectF(0, 0, 220, 120)});
    for (int32 index = 0; index < 12; ++index)
    {
        const QString pass = QStringLiteral("pass%1").arg((index * 7) % 12, 2, 10, QLatin1Char('0'));
        nodes.append({pass, QRectF(0, 0, 220, 120)});
        edges.append({QStringLiteral("scene"), pass});
        edges.append({pass, QStringLiteral("zsink")});
        if (index % 3 == 0)
        {
            edges.append({QStringLiteral("camera"), pass});
        }
    }
    edges.append({QStringLiteral("camera"), QStringLiteral("zsink")});
    QVector<RenderGraphNodePosition> positions;
    REQUIRE(ComputeRenderGraphLayout(nodes, edges, RenderGraphLayoutOperation::Arrange, positions) == true);
    // Baseline: the same columns with rows in identity order.
    QVector<RenderGraphNodePosition> identityRows;
    QHash<int32, int32> nextRow;
    QVector<RenderGraphNodePosition> byIdentity = positions;
    std::sort(byIdentity.begin(),
              byIdentity.end(),
              [](const RenderGraphNodePosition& left, const RenderGraphNodePosition& right) {
                  return left.Node < right.Node;
              });
    for (const RenderGraphNodePosition& position : byIdentity)
    {
        identityRows.append({position.Node, position.X, nextRow.value(position.X)});
        nextRow[position.X] += 148;
    }
    CHECK(CountCrossings(nodes, edges, positions) <= CountCrossings(nodes, edges, identityRows));
    // The camera feeds pass00 directly, and nothing feeds the camera: it sits one column before its
    // first consumer, not at the far left.
    QHash<QString, int32> columns;
    for (const RenderGraphNodePosition& position : positions)
    {
        columns.insert(position.Node, position.X);
    }
    CHECK(columns.value(QStringLiteral("camera")) < columns.value(QStringLiteral("pass00")));
    CHECK(columns.value(QStringLiteral("scene")) < columns.value(QStringLiteral("pass00")));
}

TEST_CASE("minimap projection fits content inside its target and round-trips points", "[editor][render-graph][minimap]")
{
    const RenderGraphMinimapProjection projection =
        MakeRenderGraphMinimapProjection(QRectF(-500, -100, 4000, 1000), QRectF(8, 8, 176, 112));
    CHECK(projection.Scale == 176.0 / 4000.0);
    const QRectF mapped = projection.ToMap(QRectF(-500, -100, 4000, 1000));
    CHECK(QRectF(8, 8, 176, 112).contains(mapped) == true);
    CHECK(mapped.center().x() == 96.0);
    CHECK(mapped.center().y() == 64.0);
    const QPointF scenePoint(1234, 321);
    CHECK((projection.ToScene(projection.ToMap(scenePoint)) - scenePoint).manhattanLength() < 1e-9);
    const RenderGraphMinimapProjection empty = MakeRenderGraphMinimapProjection(QRectF(), QRectF(8, 8, 176, 112));
    CHECK(empty.Scale == 1.0);
}

TEST_CASE("the minimap shows the whole graph, follows the view and moves it on click and drag",
          "[editor][render-graph][minimap]")
{
    QTemporaryDir project;
    WriteGraph(project, MakeLargeGraph(true));
    RenderGraphDocument document;
    REQUIRE(document.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    RenderGraphMinimap& minimap = canvas.GetMinimap();
    CHECK(minimap.isVisible() == true);
    CHECK(canvas.IsMinimapVisible() == true);
    CHECK(canvas.GetMinimapCards().size() == 48);
    // Inside the canvas viewport, not clipped.
    CHECK(canvas.viewport()->rect().contains(minimap.geometry()) == true);
    // Zoom in so the view is a small part of the graph.
    canvas.SelectNodes({});
    REQUIRE(canvas.RestoreViewState({{}, QPointF(0, 0), 1.0, RenderGraphTraceDirection::None, {}}) == true);
    const QRectF viewBefore = canvas.GetVisibleSceneRectangle();
    const RenderGraphMinimapProjection projection = minimap.GetProjection();
    CHECK(projection.Content.contains(viewBefore) == true);
    const QRectF mappedView = projection.ToMap(viewBefore);
    CHECK(mappedView.width() < static_cast<float64>(minimap.width()));
    // Click a spot in the lower right of the map: the view centre follows (within a pixel of map resolution).
    const QPoint target(minimap.width() * 3 / 4, minimap.height() * 3 / 4);
    QTest::mouseClick(&minimap, Qt::LeftButton, Qt::NoModifier, target);
    const QPointF expected = projection.ToScene(QPointF(target));
    const QPointF centre = canvas.GetVisibleSceneRectangle().center();
    CHECK((centre - expected).manhattanLength() < 2.0 / projection.Scale);
    // Drag back toward the upper left: the view centre follows the pointer.
    const QPoint start(minimap.width() / 4, minimap.height() / 4);
    const RenderGraphMinimapProjection dragProjection = minimap.GetProjection();
    QTest::mousePress(&minimap, Qt::LeftButton, Qt::NoModifier, target);
    CHECK(minimap.IsDragging() == true);
    QMouseEvent move(QEvent::MouseMove, QPointF(start), QPointF(start), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&minimap, &move);
    QTest::mouseRelease(&minimap, Qt::LeftButton, Qt::NoModifier, start);
    CHECK(minimap.IsDragging() == false);
    const QPointF dragged = canvas.GetVisibleSceneRectangle().center();
    CHECK((dragged - dragProjection.ToScene(QPointF(start))).manhattanLength() < 2.0 / dragProjection.Scale);
    CHECK(dragged.x() < centre.x());
    CHECK(dragged.y() < centre.y());
    // Moving the view never edits the draft or the selection.
    CHECK(document.IsDirty() == false);
}

TEST_CASE("the minimap can be hidden and stays out of an empty canvas", "[editor][render-graph][minimap]")
{
    QTemporaryDir project;
    WriteGraph(project, MakeLargeGraph(true));
    RenderGraphDocument document;
    REQUIRE(document.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    ShowCanvas(canvas);
    QAction* toggle = canvas.findChild<QAction*>(QStringLiteral("renderGraphToggleMinimap"));
    REQUIRE(toggle != nullptr);
    toggle->trigger();
    CHECK(canvas.GetMinimap().isVisible() == false);
    CHECK(canvas.IsMinimapVisible() == false);
    toggle->trigger();
    CHECK(canvas.GetMinimap().isVisible() == true);
    canvas.ShowConfiguration(QString(), QString());
    QCoreApplication::processEvents();
    CHECK(canvas.GetMinimap().isVisible() == false);
}

TEST_CASE("Auto Layout saves one undoable edit of positions and the minimap follows",
          "[editor][render-graph][layout][minimap]")
{
    QTemporaryDir project;
    WriteGraph(project, MakeLargeGraph(true));
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(project.path()).filePath(GetRoot())) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1400, 900);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray initial = document.GetDraft(GetRoot());
    const uint64 revision = document.GetRevision();
    QAction* autoLayout = canvas.findChild<QAction*>(QStringLiteral("renderGraphAutoLayout"));
    REQUIRE(autoLayout != nullptr);
    REQUIRE(autoLayout->isEnabled() == true);
    canvas.SelectNodes({});
    autoLayout->trigger();
    CHECK(document.GetRevision() == revision + 1);
    const QByteArray arranged = document.GetDraft(GetRoot());
    CHECK(arranged != initial);
    // Dependency order: each chain's cards run left to right.
    for (int32 chain = 0; chain < CHAIN_COUNT; ++chain)
    {
        const QString suffix = QString::number(chain);
        CHECK(canvas.GetNodePosition(QStringLiteral("t") + suffix).x() <
              canvas.GetNodePosition(QStringLiteral("m%1_3").arg(chain)).x());
        CHECK(canvas.GetNodePosition(QStringLiteral("m%1_3").arg(chain)).x() <
              canvas.GetNodePosition(QStringLiteral("m%1_1").arg(chain)).x());
    }
    // Arranging again changes nothing.
    autoLayout->trigger();
    CHECK(document.GetRevision() == revision + 1);
    workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == initial);
    workspace.Redo();
    CHECK(document.GetDraft(GetRoot()) == arranged);
    // Saved with the document.
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(GetRoot()) == arranged);
}

TEST_CASE("Auto Layout with a selection of two or more cards leaves other cards alone",
          "[editor][render-graph][layout][minimap]")
{
    QTemporaryDir project;
    WriteGraph(project, MakeLargeGraph(true));
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(project.path()).filePath(GetRoot())) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1400, 900);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPointF other = canvas.GetNodePosition(QStringLiteral("t0"));
    canvas.SelectNodes({QStringLiteral("t1"), QStringLiteral("m1_3"), QStringLiteral("m1_2")});
    REQUIRE(canvas.AutoLayout() == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("t0")) == other);
}

namespace
{
QJsonObject MakeTwoClusterGraph()
{
    QJsonArray nodes;
    QJsonArray edges;
    QJsonArray positions;
    const auto addChain = [&](const QStringList& identities, int32 left) {
        for (qsizetype index = 0; index < identities.size(); ++index)
        {
            nodes.append(MakeNode(identities.at(index), QStringLiteral("ToneMap")));
            positions.append(QJsonObject{{QStringLiteral("node"), identities.at(index)},
                                         {QStringLiteral("x"), left + static_cast<int32>(index) * 300},
                                         {QStringLiteral("y"), 0}});
            if (index > 0)
            {
                edges.append(QJsonObject{{QStringLiteral("from"), identities.at(index - 1) + QStringLiteral(".color")},
                                         {QStringLiteral("to"), identities.at(index) + QStringLiteral(".color")}});
            }
        }
    };
    addChain({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}, 0);
    addChain({QStringLiteral("d"), QStringLiteral("e"), QStringLiteral("f")}, 3000);
    return QJsonObject{
        {QStringLiteral("version"), 2},
        {QStringLiteral("id"), QStringLiteral("Main")},
        {QStringLiteral("default"), QStringLiteral("Default")},
        {QStringLiteral("configurations"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("Default")},
                                {QStringLiteral("inputs"), QJsonArray{}},
                                {QStringLiteral("outputs"), QJsonArray{}},
                                {QStringLiteral("nodes"), nodes},
                                {QStringLiteral("edges"), edges},
                                {QStringLiteral("layout"), QJsonObject{{QStringLiteral("positions"), positions}}}}}}};
}
} // namespace

TEST_CASE("groups: a selection becomes a group, unselected cards are never swept in, and groups collapse together",
          "[editor][render-graph][minimap][groups]")
{
    QTemporaryDir project;
    WriteGraph(project, MakeTwoClusterGraph());
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(project.path()).filePath(GetRoot())) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1400, 900);
    workspace.show();
    QCoreApplication::processEvents();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphComments& comments = canvas.GetComments();
    QSignalSpy rejections(&comments, &RenderGraphComments::Rejected);
    canvas.SelectNodes({});
    CHECK(comments.GroupSelection() == false);
    CHECK(rejections.count() == 1);
    // Skipping the middle card would sweep it into the group.
    canvas.SelectNodes({QStringLiteral("a"), QStringLiteral("c")});
    CHECK(comments.GroupSelection() == false);
    REQUIRE(rejections.count() == 2);
    CHECK(rejections.last().at(0).toString().contains(QStringLiteral("'b'")) == true);
    CHECK(comments.HasFrames() == false);
    canvas.SelectNodes({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
    const uint64 revision = workspace.GetDocument().GetRevision();
    REQUIRE(comments.GroupSelection() == true);
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    CHECK(comments.HasFrames() == true);
    CHECK(comments.GetFrameBounds(QStringLiteral("group1")).isEmpty() == false);
    canvas.SelectNodes({QStringLiteral("d"), QStringLiteral("e"), QStringLiteral("f")});
    REQUIRE(comments.GroupSelection() == true);
    CHECK(comments.GetFrameBounds(QStringLiteral("group2")).isEmpty() == false);
    // One edit collapses both; one edit expands both.
    const uint64 grouped = workspace.GetDocument().GetRevision();
    REQUIRE(comments.SetAllCollapsed(true) == true);
    CHECK(workspace.GetDocument().GetRevision() == grouped + 1);
    CHECK(comments.GetCollapsedFrame(QStringLiteral("b")) == QStringLiteral("group1"));
    CHECK(comments.GetCollapsedFrame(QStringLiteral("e")) == QStringLiteral("group2"));
    CHECK(comments.SetAllCollapsed(true) == false);
    REQUIRE(comments.SetAllCollapsed(false) == true);
    CHECK(comments.GetCollapsedFrame(QStringLiteral("b")).isEmpty() == true);
    // Auto Layout keeps each group around its own cards, in the same edit.
    canvas.SelectNodes({});
    const uint64 beforeLayout = workspace.GetDocument().GetRevision();
    REQUIRE(canvas.AutoLayout() == true);
    CHECK(workspace.GetDocument().GetRevision() == beforeLayout + 1);
    const QVector<RenderGraphLayoutNode> cards = canvas.GetAuthoredNodeBounds();
    for (const RenderGraphLayoutNode& card : cards)
    {
        const QString group = card.Identity < QStringLiteral("d") ? QStringLiteral("group1") : QStringLiteral("group2");
        CHECK(comments.GetFrameBounds(group).contains(card.Bounds) == true);
    }
    workspace.Undo();
    CHECK(canvas.GetNodePosition(QStringLiteral("d")).x() == 3000);
}
