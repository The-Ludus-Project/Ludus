#include "internal/render_graph_canvas.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QFont>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainterPath>
#include <QPen>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <utility>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr QRgb NODE_BACKGROUND_COLOR = 0xFF2E3239;
constexpr QRgb NODE_BORDER_COLOR = 0xFF101215;
constexpr float64 NODE_BORDER_WIDTH = 1.5;
constexpr QRgb SELECTION_COLOR = 0xFFFFD25A;
constexpr QRgb EDITABLE_EDGE_COLOR = 0xFFB4BAC4;
constexpr float64 NODE_WIDTH = 190;
constexpr float64 FRAME_PADDING = 40;
constexpr float64 HEADER_HEIGHT = 26;
constexpr float64 PORT_SPACING = 22;
constexpr float64 PORT_RADIUS = 6;
constexpr float64 PORT_HIT_RADIUS = 11;
constexpr float64 NODE_PADDING = 8;
constexpr float64 COLUMN_SPACING = 250;
constexpr float64 ROW_SPACING = 140;
constexpr float64 EDGE_HIT_WIDTH = 8;
constexpr float64 CURVE_OFFSET = 70;
constexpr float64 ZOOM_STEP = 1.15;
constexpr float64 WHEEL_NOTCH = 120;
constexpr float64 SCROLL_PIXELS_PER_ZOOM_STEP = 40;
constexpr float64 MINIMUM_VIEW_SCALE = 0.01;
constexpr float64 MAXIMUM_VIEW_SCALE = 8;
constexpr int32 LABEL_POINT_SIZE = 9;
constexpr int32 MAX_CYCLE_SEARCH_STEPS = 1024;
/// Reserved display key for configuration outputs; not a valid authored identifier.
const QString OUTPUTS_NODE = QStringLiteral("#outputs");
const QString INPUT_NODE = QStringLiteral("input");

QJsonObject FindConfiguration(const QJsonObject& graph, const QString& configuration)
{
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (const QJsonValueConstRef value : configurations)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == configuration)
        {
            return value.toObject();
        }
    }
    return {};
}

QJsonObject FindNode(const QJsonObject& configuration, const QString& node)
{
    const QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef value : nodes)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == node)
        {
            return value.toObject();
        }
    }
    return {};
}

QVector<RenderGraphPort> ReadInterface(const QJsonArray& ports)
{
    QVector<RenderGraphPort> result;
    for (const QJsonValueConstRef port : ports)
    {
        result.append({port.toObject().value(QStringLiteral("id")).toString(),
                       port.toObject().value(QStringLiteral("type")).toString()});
    }
    return result;
}

QString FindType(const QVector<RenderGraphPort>& ports, const QString& port)
{
    for (const RenderGraphPort& candidate : ports)
    {
        if (candidate.Id == port)
        {
            return candidate.Type;
        }
    }
    return {};
}

QString GetEndpointNode(const QString& endpoint)
{
    return endpoint.section(QLatin1Char('.'), 0, 0);
}

QString GetEndpointPort(const QString& endpoint)
{
    return endpoint.section(QLatin1Char('.'), 1);
}

QColor GetDomainColor(RenderGraphDomain domain)
{
    switch (domain)
    {
        case RenderGraphDomain::SceneView:
            return {58, 110, 165};
        case RenderGraphDomain::Execution:
            return {172, 92, 46};
        case RenderGraphDomain::Resource:
            return {70, 132, 84};
        case RenderGraphDomain::Subgraph:
            return {122, 84, 160};
        case RenderGraphDomain::Interface:
            return {96, 96, 96};
    }
    return {96, 96, 96};
}

QColor GetTypeColor(const QString& type)
{
    if (type == QStringLiteral("Scene") || type == QStringLiteral("View"))
    {
        return {120, 170, 230};
    }
    if (type == QStringLiteral("DepthTexture"))
    {
        return {200, 200, 200};
    }
    return {120, 210, 140};
}

QString DescribeDomain(RenderGraphDomain domain)
{
    switch (domain)
    {
        case RenderGraphDomain::SceneView:
            return QStringLiteral("scene/view");
        case RenderGraphDomain::Execution:
            return QStringLiteral("execution");
        case RenderGraphDomain::Resource:
            return QStringLiteral("resource");
        case RenderGraphDomain::Subgraph:
            return QStringLiteral("subgraph");
        case RenderGraphDomain::Interface:
            return QStringLiteral("interface");
    }
    return {};
}

QPainterPath MakeCurve(QPointF from, QPointF to)
{
    QPainterPath path(from);
    path.cubicTo(from + QPointF(CURVE_OFFSET, 0), to - QPointF(CURVE_OFFSET, 0), to);
    return path;
}

int32 ClampCoordinate(float64 value)
{
    const float64 rounded = std::round(value);
    if (rounded > RenderGraphCanvas::MAX_COORDINATE)
    {
        return RenderGraphCanvas::MAX_COORDINATE;
    }
    if (rounded < -RenderGraphCanvas::MAX_COORDINATE)
    {
        return -RenderGraphCanvas::MAX_COORDINATE;
    }
    return static_cast<int32>(rounded);
}
} // namespace

RenderGraphDomain GetRenderGraphDomain(const QString& kind)
{
    if (kind == QStringLiteral("Scene") || kind == QStringLiteral("Camera"))
    {
        return RenderGraphDomain::SceneView;
    }
    if (kind == QStringLiteral("Target") || kind == QStringLiteral("Export"))
    {
        return RenderGraphDomain::Resource;
    }
    if (kind == QStringLiteral("Subgraph"))
    {
        return RenderGraphDomain::Subgraph;
    }
    return RenderGraphDomain::Execution;
}

bool GetRenderGraphNodePorts(const RenderGraphDocument& document,
                             const QJsonObject& node,
                             RenderGraphNodePorts& outPorts)
{
    const QString kind = node.value(QStringLiteral("kind")).toString();
    RenderGraphNodePorts ports;
    if (kind == QStringLiteral("Scene"))
    {
        ports.Outputs = {{QStringLiteral("scene"), QStringLiteral("Scene")}};
    }
    else if (kind == QStringLiteral("Camera"))
    {
        ports.Inputs = {{QStringLiteral("target"), node.value(QStringLiteral("target_type")).toString()}};
        ports.Outputs = {{QStringLiteral("view"), QStringLiteral("View")}};
    }
    else if (kind == QStringLiteral("Target"))
    {
        ports.Outputs = {{QStringLiteral("texture"), node.value(QStringLiteral("type")).toString()}};
    }
    else if (kind == QStringLiteral("Raster"))
    {
        ports.Inputs = {{QStringLiteral("scene"), QStringLiteral("Scene")},
                        {QStringLiteral("view"), QStringLiteral("View")},
                        {QStringLiteral("depth"), QStringLiteral("DepthTexture")}};
        ports.Outputs = {{QStringLiteral("depth"), QStringLiteral("DepthTexture")}};
        if (node.value(QStringLiteral("mode")).toString() == QStringLiteral("ColorAndDepth"))
        {
            ports.Inputs.append({QStringLiteral("color"), QStringLiteral("ColorTexture")});
            ports.Outputs.append({QStringLiteral("color"), QStringLiteral("ColorTexture")});
        }
    }
    else if (kind == QStringLiteral("Subgraph"))
    {
        // Assets are package-relative, exactly as the shared validator resolves them.
        QJsonObject graph;
        if (document.GetGraph(node.value(QStringLiteral("asset")).toString(), graph) == false)
        {
            return false;
        }
        const QJsonObject selected = FindConfiguration(graph, node.value(QStringLiteral("configuration")).toString());
        if (selected.isEmpty() == true)
        {
            return false;
        }
        ports.Inputs = ReadInterface(selected.value(QStringLiteral("inputs")).toArray());
        ports.Outputs = ReadInterface(selected.value(QStringLiteral("outputs")).toArray());
    }
    else if (kind == QStringLiteral("Export"))
    {
        ports.Inputs = {{QStringLiteral("texture"), node.value(QStringLiteral("type")).toString()}};
    }
    else if (kind == QStringLiteral("Present"))
    {
        ports.Inputs = {{QStringLiteral("color"), QStringLiteral("ColorTexture")}};
    }
    outPorts = ports;
    return true;
}

// File, configuration and endpoints are distinct named identities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
RenderGraphLinkCheck CheckRenderGraphLink(const RenderGraphDocument& document,
                                          const QString& file,
                                          const QString& configuration,
                                          const QString& from,
                                          const QString& to)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    QJsonObject graph;
    if (document.GetGraph(file, graph) == false)
    {
        return {QStringLiteral("Schema"), QStringLiteral("the draft graph is not a JSON object")};
    }
    const QJsonObject record = FindConfiguration(graph, configuration);
    if (record.isEmpty() == true)
    {
        return {QStringLiteral("Configuration"), QStringLiteral("configuration %1 is not declared").arg(configuration)};
    }
    const QString source = GetEndpointNode(from);
    const QString sink = GetEndpointNode(to);
    if (from.count(QLatin1Char('.')) != 1 || to.count(QLatin1Char('.')) != 1)
    {
        return {QStringLiteral("Schema"), QStringLiteral("endpoints must be node.port")};
    }
    // Resolve the producing output: configuration inputs appear as `input.port` sources.
    QString sourceType;
    if (source == INPUT_NODE)
    {
        sourceType = FindType(ReadInterface(record.value(QStringLiteral("inputs")).toArray()), GetEndpointPort(from));
    }
    else
    {
        RenderGraphNodePorts ports;
        const QJsonObject node = FindNode(record, source);
        if (node.isEmpty() == false && GetRenderGraphNodePorts(document, node, ports) == true)
        {
            sourceType = FindType(ports.Outputs, GetEndpointPort(from));
            if (sourceType.isEmpty() == true && FindType(ports.Inputs, GetEndpointPort(from)).isEmpty() == false)
            {
                return {QStringLiteral("Port"),
                        QStringLiteral("%1 is an input; drag a link from an output to an input").arg(from)};
            }
        }
    }
    if (sourceType.isEmpty() == true)
    {
        return {QStringLiteral("Port"), QStringLiteral("unknown output %1").arg(from)};
    }
    RenderGraphNodePorts sinkPorts;
    const QJsonObject sinkNode = FindNode(record, sink);
    if (sinkNode.isEmpty() == true || GetRenderGraphNodePorts(document, sinkNode, sinkPorts) == false)
    {
        return {QStringLiteral("Port"), QStringLiteral("unknown input %1").arg(to)};
    }
    const QString sinkType = FindType(sinkPorts.Inputs, GetEndpointPort(to));
    if (sinkType.isEmpty() == true)
    {
        if (FindType(sinkPorts.Outputs, GetEndpointPort(to)).isEmpty() == false)
        {
            return {QStringLiteral("Port"), QStringLiteral("%1 is an output; links end at an input").arg(to)};
        }
        return {QStringLiteral("Port"), QStringLiteral("unknown input %1").arg(to)};
    }
    const QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef edge : edges)
    {
        if (edge.toObject().value(QStringLiteral("to")).toString() == to)
        {
            return {QStringLiteral("MultipleWriters"),
                    QStringLiteral("input %1 already has a connection from %2; disconnect it first")
                        .arg(to, edge.toObject().value(QStringLiteral("from")).toString())};
        }
    }
    if (sourceType != sinkType)
    {
        return {QStringLiteral("TypeMismatch"),
                QStringLiteral("%1 (%2) cannot connect to %3 (%4)").arg(from, sourceType, to, sinkType)};
    }
    // The new edge makes `sink` depend on `source`; a path sink -> ... -> source closes a cycle.
    QStringList pending{sink};
    QSet<QString> visited;
    for (int32 step = 0; pending.isEmpty() == false && step < MAX_CYCLE_SEARCH_STEPS; ++step)
    {
        const QString current = pending.takeFirst();
        if (current == source)
        {
            return {QStringLiteral("Cycle"),
                    QStringLiteral("connecting %1 to %2 would create a within-frame dependency cycle").arg(from, to)};
        }
        if (visited.contains(current) == true)
        {
            continue;
        }
        visited.insert(current);
        for (const QJsonValueConstRef edge : edges)
        {
            if (GetEndpointNode(edge.toObject().value(QStringLiteral("from")).toString()) == current)
            {
                pending.append(GetEndpointNode(edge.toObject().value(QStringLiteral("to")).toString()));
            }
        }
    }
    return {};
}

struct RenderGraphCanvas::NodeView final
{
    explicit NodeView(QGraphicsRectItem* frame) noexcept : Frame(frame) {}
    QString Id;
    QString Kind;
    RenderGraphDomain Domain = RenderGraphDomain::Execution;
    RenderGraphNodePorts Ports;
    /// Scene-owned frame item; nonnull for the view's lifetime (until the next Rebuild).
    QGraphicsRectItem* Frame;
    QHash<QString, QGraphicsEllipseItem*> InputItems;
    QHash<QString, QGraphicsEllipseItem*> OutputItems;
    bool IsMovable = true;
};

namespace
{
/// Authored or interface connection before its scene item exists.
struct CanvasLink final
{
    QString From;
    QString To;
    bool IsEditable = true;
};
} // namespace

struct RenderGraphCanvas::EdgeView final
{
    EdgeView(const CanvasLink& link, QGraphicsPathItem* path)
        : From(link.From), To(link.To), Path(path), IsEditable(link.IsEditable)
    {
    }
    QString From;
    QString To;
    /// Scene-owned path item; nonnull for the view's lifetime (until the next Rebuild).
    QGraphicsPathItem* Path;
    bool IsEditable = true;
};

RenderGraphCanvas::RenderGraphCanvas(const RenderGraphDocument& document, QWidget* parentOrNull)
    : QGraphicsView(parentOrNull), mDocument(document), mScene(new QGraphicsScene(this))
{
    setObjectName(QStringLiteral("renderGraphCanvas"));
    setAccessibleName(QStringLiteral("Render graph canvas"));
    setScene(mScene);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setBackgroundBrush(QColor(28, 31, 36));
    setToolTip(QStringLiteral("Drag nodes to arrange them (one undo step per drag) · drag from an output port to an "
                              "input port to connect · Delete removes the selected node or link · double-click a "
                              "Subgraph to open its source · pinch or scroll to zoom around the pointer"));
}

RenderGraphCanvas::~RenderGraphCanvas()
{
    qDeleteAll(mNodes);
    qDeleteAll(mEdges);
}

void RenderGraphCanvas::ShowConfiguration(const QString& file, const QString& configuration)
{
    const bool isSame = file == mFile && configuration == mConfiguration;
    mFile = file;
    mConfiguration = configuration;
    Rebuild();
    if (isSame == false)
    {
        // A different configuration starts with no selection and fits its contents.
        SelectNode({});
        mShouldFitOnShow = true;
        if (isVisible() == true)
        {
            fitGraph();
        }
    }
}

void RenderGraphCanvas::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
    // A new graph can be loaded while the empty-state page still hides this view.
    // Fit only that pending configuration after the splitter has laid out its viewport.
    QTimer::singleShot(0, this, [this]() {
        if (mShouldFitOnShow == true)
        {
            fitGraph();
        }
    });
}

void RenderGraphCanvas::fitGraph()
{
    fitInView(mScene->itemsBoundingRect().adjusted(-FRAME_PADDING, -FRAME_PADDING, FRAME_PADDING, FRAME_PADDING),
              Qt::KeepAspectRatio);
    mShouldFitOnShow = false;
}

QJsonObject RenderGraphCanvas::configuration() const
{
    QJsonObject graph;
    if (mFile.isEmpty() == true || mDocument.GetGraph(mFile, graph) == false)
    {
        return {};
    }
    return FindConfiguration(graph, mConfiguration);
}

void RenderGraphCanvas::Rebuild()
{
    const QStringList selected = GetSelectedNodes();
    cancelLink();
    mIsDragging = false;
    mDragNodes.clear();
    qDeleteAll(mNodes);
    mNodes.clear();
    qDeleteAll(mEdges);
    mEdges.clear();
    mScene->clear();
    const QJsonObject record = configuration();
    if (record.isEmpty() == true)
    {
        return;
    }
    QHash<QString, QPointF> authored;
    const QJsonArray positions =
        record.value(QStringLiteral("layout")).toObject().value(QStringLiteral("positions")).toArray();
    for (const QJsonValueConstRef position : positions)
    {
        const QJsonObject value = position.toObject();
        authored.insert(value.value(QStringLiteral("node")).toString(),
                        QPointF(value.value(QStringLiteral("x")).toInt(), value.value(QStringLiteral("y")).toInt()));
    }
    const QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    const QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    // Display-only placement for unplaced nodes: column = longest dependency path.
    QHash<QString, int32> columns;
    for (const QJsonValueConstRef node : nodes)
    {
        columns.insert(node.toObject().value(QStringLiteral("id")).toString(), 0);
    }
    for (int32 pass = 0; pass < nodes.size(); ++pass)
    {
        bool hasChanged = false;
        for (const QJsonValueConstRef edge : edges)
        {
            const QString source = GetEndpointNode(edge.toObject().value(QStringLiteral("from")).toString());
            const QString sink = GetEndpointNode(edge.toObject().value(QStringLiteral("to")).toString());
            if (columns.contains(source) == true && columns.contains(sink) == true &&
                columns.value(sink) <= columns.value(source))
            {
                columns.insert(sink, columns.value(source) + 1);
                hasChanged = true;
            }
        }
        if (hasChanged == false)
        {
            break;
        }
    }
    QHash<int32, int32> rows;
    int32 lastColumn = 0;
    // Identity and display title are distinct named strings.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    const auto addNode = [this](const QString& identity,
                                const QString& title,
                                RenderGraphDomain domain,
                                const RenderGraphNodePorts& ports,
                                QPointF position) {
        // NOLINTEND(bugprone-easily-swappable-parameters)
        const qsizetype rowCount = qMax(ports.Inputs.size(), ports.Outputs.size());
        const float64 height = HEADER_HEIGHT + static_cast<float64>(rowCount) * PORT_SPACING + NODE_PADDING;
        NodeView* view = new NodeView(mScene->addRect(QRectF(0, 0, NODE_WIDTH, height),
                                                      QPen(QColor::fromRgba(NODE_BORDER_COLOR), NODE_BORDER_WIDTH),
                                                      QColor::fromRgba(NODE_BACKGROUND_COLOR)));
        view->Id = identity;
        view->Domain = domain;
        view->Ports = ports;
        view->Frame->setPos(position);
        view->Frame->setData(0, identity);
        view->Frame->setFlag(QGraphicsItem::ItemIsSelectable);
        view->Frame->setToolTip(QStringLiteral("%1 (%2 domain)").arg(title, DescribeDomain(domain)));
        QGraphicsRectItem* header = new QGraphicsRectItem(QRectF(0, 0, NODE_WIDTH, HEADER_HEIGHT), view->Frame);
        header->setBrush(GetDomainColor(domain));
        header->setPen(Qt::NoPen);
        QGraphicsSimpleTextItem* label = new QGraphicsSimpleTextItem(title, header);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        label->setBrush(Qt::white);
        label->setPos(NODE_PADDING, (HEADER_HEIGHT - label->boundingRect().height()) / 2);
        const auto addPort = [&view](const RenderGraphPort& port, qsizetype row, bool isOutput) {
            const float64 y = HEADER_HEIGHT + (static_cast<float64>(row) + 0.5) * PORT_SPACING;
            const float64 x = isOutput == true ? NODE_WIDTH : 0;
            QGraphicsEllipseItem* connector =
                new QGraphicsEllipseItem(QRectF(x - PORT_RADIUS, y - PORT_RADIUS, 2 * PORT_RADIUS, 2 * PORT_RADIUS),
                                         view->Frame);
            connector->setBrush(GetTypeColor(port.Type));
            connector->setPen(QPen(QColor(16, 18, 21), 1));
            connector->setToolTip(QStringLiteral("%1.%2 : %3").arg(view->Id, port.Id, port.Type));
            QGraphicsSimpleTextItem* name = new QGraphicsSimpleTextItem(port.Id, view->Frame);
            QFont small = name->font();
            small.setPointSize(LABEL_POINT_SIZE);
            name->setFont(small);
            name->setBrush(QColor(214, 218, 224));
            const float64 labelX =
                isOutput == true ? NODE_WIDTH - PORT_RADIUS - 4 - name->boundingRect().width() : PORT_RADIUS + 4;
            name->setPos(labelX, y - name->boundingRect().height() / 2);
            (isOutput == true ? view->OutputItems : view->InputItems).insert(port.Id, connector);
        };
        for (qsizetype i = 0; i < ports.Inputs.size(); ++i)
        {
            addPort(ports.Inputs.at(i), i, false);
        }
        for (qsizetype i = 0; i < ports.Outputs.size(); ++i)
        {
            addPort(ports.Outputs.at(i), i, true);
        }
        mNodes.insert(identity, view);
        return view;
    };
    for (const QJsonValueConstRef value : nodes)
    {
        const QJsonObject node = value.toObject();
        const QString identity = node.value(QStringLiteral("id")).toString();
        const QString kind = node.value(QStringLiteral("kind")).toString();
        RenderGraphNodePorts ports;
        const bool hasPorts = GetRenderGraphNodePorts(mDocument, node, ports);
        const int32 column = columns.value(identity);
        lastColumn = qMax(lastColumn, column);
        const int32 row = rows.value(column);
        rows.insert(column, row + 1);
        const QPointF position = authored.contains(identity) == true
                                     ? authored.value(identity)
                                     : QPointF(column * COLUMN_SPACING, row * ROW_SPACING);
        NodeView* view = addNode(identity,
                                 identity + QStringLiteral(" : ") + kind +
                                     (hasPorts == true ? QString() : QStringLiteral(" (missing source)")),
                                 GetRenderGraphDomain(kind),
                                 ports,
                                 position);
        view->Kind = kind;
    }
    const QVector<RenderGraphPort> inputs = ReadInterface(record.value(QStringLiteral("inputs")).toArray());
    if (inputs.isEmpty() == false)
    {
        NodeView* view = addNode(INPUT_NODE,
                                 QStringLiteral("input (configuration)"),
                                 RenderGraphDomain::Interface,
                                 {{}, inputs},
                                 QPointF(-COLUMN_SPACING, 0));
        view->IsMovable = false;
    }
    QVector<CanvasLink> links;
    const QJsonArray outputs = record.value(QStringLiteral("outputs")).toArray();
    if (outputs.isEmpty() == false)
    {
        NodeView* view = addNode(OUTPUTS_NODE,
                                 QStringLiteral("outputs (configuration)"),
                                 RenderGraphDomain::Interface,
                                 {ReadInterface(outputs), {}},
                                 QPointF((lastColumn + 1) * COLUMN_SPACING, 0));
        view->IsMovable = false;
        for (const QJsonValueConstRef output : outputs)
        {
            links.append({output.toObject().value(QStringLiteral("source")).toString(),
                          OUTPUTS_NODE + QLatin1Char('.') + output.toObject().value(QStringLiteral("id")).toString(),
                          false});
        }
    }
    for (const QJsonValueConstRef value : edges)
    {
        links.append({value.toObject().value(QStringLiteral("from")).toString(),
                      value.toObject().value(QStringLiteral("to")).toString(),
                      true});
    }
    for (const CanvasLink& link : std::as_const(links))
    {
        mEdges.append(
            new EdgeView(link, mScene->addPath(QPainterPath(), QPen(QColor::fromRgba(EDITABLE_EDGE_COLOR), 2))));
    }
    for (EdgeView* edge : std::as_const(mEdges))
    {
        edge->Path->setZValue(-1);
        if (edge->IsEditable == false)
        {
            edge->Path->setPen(QPen(QColor(150, 150, 150), 1.5, Qt::DashLine));
            edge->Path->setToolTip(QStringLiteral("Configuration output %1 (edit outputs in the Source tab)")
                                       .arg(GetEndpointPort(edge->To)));
        }
        else
        {
            edge->Path->setToolTip(
                QStringLiteral("%1 → %2 (select and press Delete to disconnect)").arg(edge->From, edge->To));
        }
    }
    updateEdges();
    for (const QString& node : selected)
    {
        if (mNodes.contains(node) == true)
        {
            mNodes.value(node)->Frame->setSelected(true);
        }
    }
    updateSelection();
    mScene->setSceneRect(mScene->itemsBoundingRect().adjusted(-400, -400, 400, 400));
}

void RenderGraphCanvas::updateEdges()
{
    for (EdgeView* edge : std::as_const(mEdges))
    {
        const QPointF from = GetPortPosition(edge->From, true);
        const QPointF to = GetPortPosition(edge->To, false);
        if (from.isNull() == true || to.isNull() == true)
        {
            // A dangling endpoint stays visible as a short stub; validation names it.
            edge->Path->setPath(QPainterPath());
            continue;
        }
        edge->Path->setPath(MakeCurve(from, to));
    }
}

int32 RenderGraphCanvas::GetNodeCount() const
{
    int32 count = 0;
    for (const NodeView* node : std::as_const(mNodes))
    {
        count += node->Domain == RenderGraphDomain::Interface ? 0 : 1;
    }
    return count;
}

int32 RenderGraphCanvas::GetEdgeCount() const
{
    int32 count = 0;
    for (const EdgeView* edge : std::as_const(mEdges))
    {
        count += edge->IsEditable == true ? 1 : 0;
    }
    return count;
}

bool RenderGraphCanvas::HasNode(const QString& node) const
{
    return mNodes.contains(node);
}

QPointF RenderGraphCanvas::GetNodePosition(const QString& node) const
{
    const NodeView* viewOrNull = mNodes.value(node, nullptr);
    return viewOrNull == nullptr ? QPointF() : viewOrNull->Frame->pos();
}

QPointF RenderGraphCanvas::GetPortPosition(const QString& endpoint, bool isOutput) const
{
    const NodeView* viewOrNull = mNodes.value(GetEndpointNode(endpoint), nullptr);
    if (viewOrNull == nullptr)
    {
        return {};
    }
    const QGraphicsEllipseItem* connectorOrNull =
        (isOutput == true ? viewOrNull->OutputItems : viewOrNull->InputItems).value(GetEndpointPort(endpoint), nullptr);
    return connectorOrNull == nullptr ? QPointF() : connectorOrNull->mapToScene(connectorOrNull->rect().center());
}

RenderGraphDomain RenderGraphCanvas::GetNodeDomain(const QString& node) const
{
    const NodeView* viewOrNull = mNodes.value(node, nullptr);
    return viewOrNull == nullptr ? RenderGraphDomain::Interface : viewOrNull->Domain;
}

void RenderGraphCanvas::SelectNode(const QString& node)
{
    for (NodeView* view : std::as_const(mNodes))
    {
        view->Frame->setSelected(view->Id == node);
    }
    updateSelection();
}

void RenderGraphCanvas::RevealNode(const QString& node)
{
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Id == node)
        {
            ensureVisible(view->Frame);
            return;
        }
    }
}

void RenderGraphCanvas::updateSelection()
{
    for (NodeView* view : std::as_const(mNodes))
    {
        view->Frame->setPen(QPen(view->Frame->isSelected() == true ? QColor::fromRgba(SELECTION_COLOR)
                                                                   : QColor::fromRgba(NODE_BORDER_COLOR),
                                 NODE_BORDER_WIDTH));
    }
}

QStringList RenderGraphCanvas::GetSelectedNodes() const
{
    QStringList selected;
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Frame->isSelected() == true)
        {
            selected.append(view->Id);
        }
    }
    selected.sort();
    return selected;
}

void RenderGraphCanvas::BeginDrag(const QStringList& nodes)
{
    mDragNodes.clear();
    mDragOrigins.clear();
    for (const QString& node : nodes)
    {
        const NodeView* viewOrNull = mNodes.value(node, nullptr);
        if (viewOrNull != nullptr && viewOrNull->IsMovable == true)
        {
            mDragNodes.append(node);
            mDragOrigins.insert(node, viewOrNull->Frame->pos());
        }
    }
    mDragOffset = {};
    mIsDragging = mDragNodes.isEmpty() == false;
}

void RenderGraphCanvas::UpdateDrag(QPointF delta)
{
    if (mIsDragging == false)
    {
        return;
    }
    // Positions move live but nothing reaches the document until the gesture ends.
    mDragOffset = delta;
    for (const QString& node : std::as_const(mDragNodes))
    {
        const QPointF origin = mDragOrigins.value(node);
        mNodes.value(node)->Frame->setPos(ClampCoordinate(origin.x() + delta.x()),
                                          ClampCoordinate(origin.y() + delta.y()));
    }
    updateEdges();
}

void RenderGraphCanvas::EndDrag()
{
    if (mIsDragging == false)
    {
        return;
    }
    mIsDragging = false;
    QVector<RenderGraphNodePosition> positions;
    for (const QString& node : std::as_const(mDragNodes))
    {
        const QPointF position = mNodes.value(node)->Frame->pos();
        if (position != mDragOrigins.value(node))
        {
            positions.append({node, ClampCoordinate(position.x()), ClampCoordinate(position.y())});
        }
    }
    mDragNodes.clear();
    mDragOrigins.clear();
    if (positions.isEmpty() == false)
    {
        Q_EMIT MoveRequested(positions);
    }
}

bool RenderGraphCanvas::RequestLink(const QString& from, const QString& to)
{
    const RenderGraphLinkCheck check = CheckRenderGraphLink(mDocument, mFile, mConfiguration, from, to);
    if (check.IsLegal() == false)
    {
        Q_EMIT LinkRejected(QStringLiteral("%1: %2").arg(check.Code, check.Reason));
        return false;
    }
    Q_EMIT ConnectRequested(from, to);
    return true;
}

bool RenderGraphCanvas::ActivateNode(const QString& node)
{
    const QJsonObject record = FindNode(configuration(), node);
    if (record.value(QStringLiteral("kind")).toString() == QStringLiteral("Raster"))
    {
        Q_EMIT ShaderSourcesRequested(node);
        return true;
    }
    if (record.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph"))
    {
        return false;
    }
    Q_EMIT SubgraphActivated(record.value(QStringLiteral("asset")).toString(),
                             record.value(QStringLiteral("configuration")).toString());
    return true;
}

QString RenderGraphCanvas::nodeAt(QPointF position) const
{
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Frame->sceneBoundingRect().contains(position) == true)
        {
            return view->Id;
        }
    }
    return {};
}

QString RenderGraphCanvas::portAt(QPointF position, bool& outIsOutput) const
{
    for (const NodeView* view : std::as_const(mNodes))
    {
        for (const bool isOutput : {true, false})
        {
            const QHash<QString, QGraphicsEllipseItem*>& items =
                isOutput == true ? view->OutputItems : view->InputItems;
            for (QHash<QString, QGraphicsEllipseItem*>::const_iterator item = items.constBegin();
                 item != items.constEnd();
                 ++item)
            {
                const QPointF center = item.value()->mapToScene(item.value()->rect().center());
                const QPointF offset = center - position;
                if (offset.x() * offset.x() + offset.y() * offset.y() <= PORT_HIT_RADIUS * PORT_HIT_RADIUS)
                {
                    outIsOutput = isOutput;
                    return view->Id + QLatin1Char('.') + item.key();
                }
            }
        }
    }
    return {};
}

QString RenderGraphCanvas::edgeAt(QPointF position, QString& outTo) const
{
    QPainterPathStroker stroker;
    stroker.setWidth(EDGE_HIT_WIDTH);
    for (const EdgeView* edge : std::as_const(mEdges))
    {
        if (edge->IsEditable == true && stroker.createStroke(edge->Path->path()).contains(position) == true)
        {
            outTo = edge->To;
            return edge->From;
        }
    }
    return {};
}

void RenderGraphCanvas::cancelLink()
{
    mLinkFrom.clear();
    if (mLinkPreviewOrNull != nullptr)
    {
        mScene->removeItem(mLinkPreviewOrNull);
        delete mLinkPreviewOrNull;
        mLinkPreviewOrNull = nullptr;
    }
}

void RenderGraphCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    const QString node = nodeAt(mapToScene(event->pos()));
    const bool isAuthoredNode = FindNode(configuration(), node).isEmpty() == false;
    Q_EMIT ContextMenuRequested(isAuthoredNode == true ? node : QString(), event->globalPos());
    event->accept();
}

void RenderGraphCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        QGraphicsView::mousePressEvent(event);
        return;
    }
    const QPointF position = mapToScene(event->position().toPoint());
    mPressPosition = position;
    bool isOutput = false;
    const QString port = portAt(position, isOutput);
    if (port.isEmpty() == false && isOutput == true)
    {
        mLinkFrom = port;
        mLinkPreviewOrNull =
            mScene->addPath(MakeCurve(position, position), QPen(QColor::fromRgba(SELECTION_COLOR), 2, Qt::DashLine));
        event->accept();
        return;
    }
    const QString node = nodeAt(position);
    if (node.isEmpty() == false)
    {
        NodeView* view = mNodes.value(node);
        if (event->modifiers().testFlag(Qt::ControlModifier) == true)
        {
            view->Frame->setSelected(view->Frame->isSelected() == false);
            updateSelection();
        }
        else if (event->modifiers().testFlag(Qt::ShiftModifier) == true)
        {
            view->Frame->setSelected(true);
            updateSelection();
        }
        else if (view->Frame->isSelected() == false)
        {
            SelectNode(node);
        }
        Q_EMIT NodeSelected(node);
        BeginDrag(GetSelectedNodes());
        event->accept();
        return;
    }
    QString to;
    const QString from = edgeAt(position, to);
    for (EdgeView* edge : std::as_const(mEdges))
    {
        const bool isSelected = edge->From == from && edge->To == to && from.isEmpty() == false;
        edge->Path->setSelected(isSelected);
        if (edge->IsEditable == true)
        {
            edge->Path->setPen(
                QPen(isSelected == true ? QColor::fromRgba(SELECTION_COLOR) : QColor::fromRgba(EDITABLE_EDGE_COLOR),
                     2));
        }
        edge->Path->setData(0, isSelected);
    }
    if (from.isEmpty() == true)
    {
        SelectNode({});
        QGraphicsView::mousePressEvent(event);
    }
}

void RenderGraphCanvas::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF position = mapToScene(event->position().toPoint());
    if (mLinkPreviewOrNull != nullptr)
    {
        mLinkPreviewOrNull->setPath(MakeCurve(GetPortPosition(mLinkFrom, true), position));
        event->accept();
        return;
    }
    if (mIsDragging == true)
    {
        UpdateDrag(position - mPressPosition);
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void RenderGraphCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (mLinkPreviewOrNull != nullptr)
    {
        const QString from = mLinkFrom;
        cancelLink();
        bool isOutput = false;
        const QString to = portAt(mapToScene(event->position().toPoint()), isOutput);
        if (to.isEmpty() == false)
        {
            static_cast<void>(RequestLink(from, to));
        }
        event->accept();
        return;
    }
    if (mIsDragging == true)
    {
        EndDrag();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void RenderGraphCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }
    const QString node = nodeAt(mapToScene(event->position().toPoint()));
    if (node.isEmpty() == false && ActivateNode(node) == true)
    {
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void RenderGraphCanvas::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::SelectAll) == true)
    {
        for (NodeView* view : std::as_const(mNodes))
        {
            view->Frame->setSelected(true);
        }
        updateSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape)
    {
        cancelLink();
        UpdateDrag({});
        mIsDragging = false;
        mDragNodes.clear();
        mDragOrigins.clear();
        mScene->clearSelection();
        for (EdgeView* edge : std::as_const(mEdges))
        {
            edge->Path->setData(0, false);
            if (edge->IsEditable == true)
            {
                edge->Path->setPen(QPen(QColor::fromRgba(EDITABLE_EDGE_COLOR), 2));
            }
        }
        updateSelection();
        event->accept();
        return;
    }
    if (event->key() != Qt::Key_Delete && event->key() != Qt::Key_Backspace)
    {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    for (const EdgeView* edge : std::as_const(mEdges))
    {
        if (edge->IsEditable == true && edge->Path->data(0).toBool() == true)
        {
            Q_EMIT DisconnectRequested(edge->From, edge->To);
            event->accept();
            return;
        }
    }
    for (const QString& node : GetSelectedNodes())
    {
        if (mNodes.value(node)->IsMovable == true)
        {
            Q_EMIT RemoveRequested(node);
        }
    }
    event->accept();
}

void RenderGraphCanvas::wheelEvent(QWheelEvent* event)
{
    const float64 steps = event->pixelDelta().isNull() == false ? event->pixelDelta().y() / SCROLL_PIXELS_PER_ZOOM_STEP
                                                                : event->angleDelta().y() / WHEEL_NOTCH;
    zoomAt(math::Pow(ZOOM_STEP, steps), event->position());
    event->accept();
}

bool RenderGraphCanvas::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::NativeGesture)
    {
        QNativeGestureEvent* gesture = static_cast<QNativeGestureEvent*>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture)
        {
            // Thanks to Qt Group, QNativeGestureEvent, "Detailed Description" and
            // constructor: macOS/Wayland pinch values are incremental magnification,
            // applied as 1 + value. Two-finger scrolling remains a separate wheel event.
            // https://doc.qt.io/qt-6/qnativegestureevent.html
            zoomAt(1 + gesture->value(), gesture->position());
            event->accept();
            return true;
        }
        if (gesture->gestureType() == Qt::BeginNativeGesture || gesture->gestureType() == Qt::EndNativeGesture)
        {
            event->accept();
            return true;
        }
    }
    return QGraphicsView::viewportEvent(event);
}

void RenderGraphCanvas::zoomAt(float64 factor, QPointF position)
{
    if (math::IsFinite(factor) == false || factor <= 0 || factor == 1)
    {
        return;
    }
    const float64 current = transform().m11();
    if (math::IsFinite(current) == false || current <= 0)
    {
        return;
    }
    const float64 scaled = current * factor;
    const float64 target = math::IsFinite(scaled) == true ? math::Clamp(scaled, MINIMUM_VIEW_SCALE, MAXIMUM_VIEW_SCALE)
                                                          : MAXIMUM_VIEW_SCALE;
    const QPointF before = viewportTransform().inverted().map(position);
    scale(target / current, target / current);
    const QPointF after = viewportTransform().inverted().map(position);
    const QPointF offset = after - before;
    translate(offset.x(), offset.y());
}
} // namespace ludus::editor
