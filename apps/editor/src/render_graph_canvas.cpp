#include "internal/render_graph_canvas.h"
#include "internal/render_graph_canvas_feedback.h"
#include "internal/render_graph_comments.h"
#include "internal/render_graph_diagnostics.h"
#include "internal/render_graph_history.h"
#include "internal/render_graph_navigation.h"
#include "internal/render_graph_node_ports.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_reroutes.h"
#include "internal/render_graph_resource_inspection.h"
#include "internal/render_graph_resources.h"
#include "internal/render_graph_scene_bundle.h"
#include "internal/render_graph_upscale_node.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsTextItem>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>
#include <QPointingDevice>
#include <QResizeEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QToolBar>
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
constexpr float64 FOCUSED_EDGE_WIDTH = 3;
constexpr float64 UNRELATED_EDGE_OPACITY = 0.18;
constexpr float64 UNRELATED_NODE_OPACITY = 0.25;
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
constexpr float64 PAN_PIXELS_PER_WHEEL_NOTCH = 60;
constexpr float64 MINIMUM_VIEW_SCALE = 0.01;
constexpr float64 MAXIMUM_VIEW_SCALE = 8;
// Keep restored centres representable by Qt's signed 32-bit scroll bars at every supported zoom.
constexpr float64 MAX_RESTORED_CENTRE_COORDINATE = static_cast<float64>(INT32_MAX) / MAXIMUM_VIEW_SCALE;
constexpr float64 DETAIL_VIEW_SCALE = 0.75;
constexpr float64 PORT_LABEL_VIEW_SCALE = 0.45;
constexpr float64 MINIMUM_TEXT_CARD_WIDTH = 60;
constexpr float64 MINIMUM_STATUS_CARD_HEIGHT = 38;
constexpr float64 MINIMUM_TITLE_PIXEL_HEIGHT = 14;
constexpr float64 MINIMUM_LABEL_PIXEL_HEIGHT = 12;
constexpr float64 PERCENT_SCALE = 100;
constexpr float64 SCENE_SCROLL_PADDING = 400;
constexpr float64 PORT_LABEL_GAP = 4;
constexpr int32 LABEL_POINT_SIZE = 9;
constexpr int32 MAX_CYCLE_SEARCH_STEPS = 1024;
constexpr float64 INACTIVE_OPACITY = 0.55;
constexpr float64 FEEDBACK_HEIGHT = 40;
constexpr float64 LINK_HINT_WIDTH = 340;
constexpr float64 LINK_HINT_OFFSET = 16;
constexpr QRgb LINK_ACCEPT_COLOR = 0xFF79D69E;
constexpr QRgb LINK_REJECT_COLOR = 0xFFFF9A88;
constexpr QRgb WARNING_COLOR = 0xFFFFC857;
constexpr QRgb LABEL_COLOR = 0xFFD6DAE0;
constexpr float64 NODE_ROW_GAP = 30;
// Clicking an output offers Quick Add for a node one ordinary column gap to its right.
constexpr float64 QUICK_ADD_PORT_GAP = COLUMN_SPACING - NODE_WIDTH;
/// Reserved display key for configuration outputs; not a valid authored identifier.
const QString OUTPUTS_NODE = QStringLiteral("#outputs");
const QString INPUT_NODE = QStringLiteral("input");

enum class CanvasLabelRole : uint8
{
    Title,
    Annotation,
};

// Compensate small labels in screen pixels while keeping their node-local geometry stable.
void SetScaledLabel(QGraphicsSimpleTextItem& inoutLabel,
                    const QString& text,
                    float64 viewScale,
                    CanvasLabelRole role,
                    float64 availableWidth)
{
    const float64 minimumPixelHeight =
        role == CanvasLabelRole::Title ? MINIMUM_TITLE_PIXEL_HEIGHT : MINIMUM_LABEL_PIXEL_HEIGHT;
    const float64 labelScale =
        math::Max(1.0, minimumPixelHeight / (QFontMetricsF(inoutLabel.font()).height() * viewScale));
    inoutLabel.setScale(labelScale);
    inoutLabel.setText(QFontMetricsF(inoutLabel.font()).elidedText(text, Qt::ElideRight, availableWidth / labelScale));
}

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

/// Hide derived Scene bundle members of a collapsed node that nothing connects, as the canvas shows them.
void HideUnusedSceneMembers(const QJsonObject& record, const QJsonObject& node, RenderGraphNodePorts& inoutPorts)
{
    const QString identity = node.value(QStringLiteral("id")).toString();
    const QString kind = node.value(QStringLiteral("kind")).toString();
    const QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    const auto hide = [&identity, &kind, &node, &edges, &record](QVector<RenderGraphPort>& inoutList, bool isOutput) {
        for (qsizetype index = inoutList.size(); index > 0; --index)
        {
            const RenderGraphPort& port = inoutList.at(index - 1);
            if (IsSceneBundleMember(port) == false)
            {
                continue;
            }
            const bool isDerivedMember =
                kind == QStringLiteral("Scene") || GetSceneBundlePortType(node, port.Id).isEmpty() == false;
            if (isDerivedMember == false)
            {
                continue;
            }
            bool isConnected = false;
            for (const QJsonValueConstRef edge : edges)
            {
                isConnected = isConnected == true ||
                              edge.toObject()
                                      .value(isOutput == true ? QStringLiteral("from") : QStringLiteral("to"))
                                      .toString() == identity + QLatin1Char('.') + port.Id;
            }
            if (isOutput == true)
            {
                for (const QJsonValueConstRef output : record.value(QStringLiteral("outputs")).toArray())
                {
                    isConnected = isConnected == true || output.toObject().value(QStringLiteral("source")).toString() ==
                                                             identity + QLatin1Char('.') + port.Id;
                }
            }
            if (isConnected == false)
            {
                inoutList.removeAt(index - 1);
            }
        }
    };
    hide(inoutPorts.Inputs, false);
    hide(inoutPorts.Outputs, true);
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
    if (type.startsWith(QStringLiteral("Scene")) == true || type == QStringLiteral("View"))
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
    // Keep forward control points ordered across short gaps; backward links retain their routing.
    const float64 horizontalGap = to.x() - from.x();
    const float64 curveOffset = horizontalGap >= 0 ? math::Min(CURVE_OFFSET, horizontalGap * 0.5) : CURVE_OFFSET;
    QPainterPath path(from);
    path.cubicTo(from + QPointF(curveOffset, 0), to - QPointF(curveOffset, 0), to);
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
    if (kind == QStringLiteral("StructuredBuffer") || kind == QStringLiteral("Target") ||
        kind == QStringLiteral("Export") || kind == QStringLiteral("RenderSize") ||
        kind == QStringLiteral("OutputSize"))
    {
        return RenderGraphDomain::Resource;
    }
    if (kind == QStringLiteral("Subgraph"))
    {
        return RenderGraphDomain::Subgraph;
    }
    return RenderGraphDomain::Execution;
}

bool CanDisableRenderGraphNode(const QString& kind)
{
    return kind == QStringLiteral("Accumulate") || kind == QStringLiteral("Denoise") ||
           kind == QStringLiteral("ToneMap") || kind == QStringLiteral("Export");
}

bool IsRenderGraphNodeEnabled(const QJsonObject& node)
{
    return CanDisableRenderGraphNode(node.value(QStringLiteral("kind")).toString()) == false ||
           node.value(QStringLiteral("enabled")) != QJsonValue(false);
}

namespace
{
/// CheckRenderGraphLink against one configuration record, which may hold a node that is not in the draft yet.
// Configuration and endpoints are distinct named identities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
RenderGraphLinkCheck CheckLinkInRecord(const RenderGraphDocument& document,
                                       const QJsonObject& record,
                                       const QString& configuration,
                                       const QString& from,
                                       const QString& to,
                                       bool shouldReplaceExisting)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
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
        sourceType = FindType(ReadRenderGraphInterfacePorts(record.value(QStringLiteral("inputs")).toArray()),
                              GetEndpointPort(from));
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
    if (sink == OUTPUTS_NODE)
    {
        const QJsonArray outputs = record.value(QStringLiteral("outputs")).toArray();
        const QString sinkType = FindType(ReadRenderGraphInterfacePorts(outputs), GetEndpointPort(to));
        if (sinkType.isEmpty() == true)
        {
            return {QStringLiteral("Port"), QStringLiteral("unknown configuration output %1").arg(to)};
        }
        if (sinkType != sourceType)
        {
            return {QStringLiteral("TypeMismatch"),
                    QStringLiteral("%1 (%2) cannot connect to %3 (%4)").arg(from, sourceType, to, sinkType)};
        }
        for (const QJsonValueConstRef value : outputs)
        {
            const QJsonObject output = value.toObject();
            if (output.value(QStringLiteral("id")).toString() == GetEndpointPort(to) &&
                output.value(QStringLiteral("source")).toString().isEmpty() == false && shouldReplaceExisting == false)
            {
                return {QStringLiteral("MultipleWriters"),
                        QStringLiteral("Configuration output %1 is connected; disconnect it first.").arg(to)};
            }
        }
        return {};
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
        if (edge.toObject().value(QStringLiteral("to")).toString() == to && shouldReplaceExisting == false)
        {
            return {QStringLiteral("MultipleWriters"),
                    QStringLiteral("input %1 already has a connection from %2; disconnect it first")
                        .arg(to, edge.toObject().value(QStringLiteral("from")).toString())};
        }
    }
    if (sourceType != sinkType &&
        (sinkType != QStringLiteral("Texture") ||
         (sourceType != QStringLiteral("ColorTexture") && sourceType != QStringLiteral("DepthTexture"))))
    {
        return {QStringLiteral("TypeMismatch"),
                QStringLiteral("%1 (%2) cannot connect to %3 (%4)").arg(from, sourceType, to, sinkType)};
    }
    for (const RenderGraphPort& port : sinkPorts.Inputs)
    {
        if (port.Type != QStringLiteral("Scene"))
        {
            continue;
        }
        const QString bundle = sink + QLatin1Char('.') + port.Id;
        const QString memberPrefix = bundle + QLatin1Char('_');
        for (const QJsonValueConstRef value : edges)
        {
            const QString target = value.toObject().value(QStringLiteral("to")).toString();
            if ((to == bundle && target.startsWith(memberPrefix) == true) ||
                (to.startsWith(memberPrefix) == true && target == bundle))
            {
                return {QStringLiteral("MultipleWriters"),
                        QStringLiteral("Expand the Scene bundle before connecting its members; disconnect the member "
                                       "wires before reconnecting the full bundle.")};
            }
        }
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
    return CheckRenderGraphTextureContracts(document, record, configuration, from, to);
}

/// The displayed configuration record, or a failed check explaining why there is none.
// File and configuration are distinct named identities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
RenderGraphLinkCheck ReadConfigurationRecord(const RenderGraphDocument& document,
                                             const QString& file,
                                             const QString& configuration,
                                             QJsonObject& outRecord)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    QJsonObject graph;
    if (document.GetGraph(file, graph) == false)
    {
        return {QStringLiteral("Schema"), QStringLiteral("the draft graph is not a JSON object")};
    }
    outRecord = FindConfiguration(graph, configuration);
    if (outRecord.isEmpty() == true)
    {
        return {QStringLiteral("Configuration"), QStringLiteral("configuration %1 is not declared").arg(configuration)};
    }
    return {};
}
} // namespace

// File, configuration and endpoints are distinct named identities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
RenderGraphLinkCheck CheckRenderGraphLink(const RenderGraphDocument& document,
                                          const QString& file,
                                          const QString& configuration,
                                          const QString& from,
                                          const QString& to,
                                          bool shouldReplaceExisting)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    QJsonObject record;
    RenderGraphLinkCheck readCheck = ReadConfigurationRecord(document, file, configuration, record);
    if (readCheck.IsLegal() == false)
    {
        return readCheck;
    }
    return CheckLinkInRecord(document, record, configuration, from, to, shouldReplaceExisting);
}

// File, configuration and endpoints are distinct named identities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
RenderGraphLinkCheck CheckRenderGraphLinkToNewNode(const RenderGraphDocument& document,
                                                   const QString& file,
                                                   const QString& configuration,
                                                   const QJsonObject& node,
                                                   const QString& from,
                                                   const QString& to)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    QJsonObject record;
    RenderGraphLinkCheck readCheck = ReadConfigurationRecord(document, file, configuration, record);
    if (readCheck.IsLegal() == false)
    {
        return readCheck;
    }
    const QString identity = node.value(QStringLiteral("id")).toString();
    if (identity.isEmpty() == true || identity == INPUT_NODE || FindNode(record, identity).isEmpty() == false)
    {
        return {QStringLiteral("Port"), QStringLiteral("%1 is not a new node identity").arg(identity)};
    }
    if (GetEndpointNode(to) != identity)
    {
        return {QStringLiteral("Port"), QStringLiteral("%1 is not an input of the new node %2").arg(to, identity)};
    }
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    nodes.append(node);
    record.insert(QStringLiteral("nodes"), nodes);
    return CheckLinkInRecord(document, record, configuration, from, to, false);
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
    QGraphicsSimpleTextItem* TitleLabelOrNull = nullptr;
    QString Title;
    QHash<QString, QGraphicsSimpleTextItem*> InputLabels;
    QHash<QString, QGraphicsSimpleTextItem*> OutputLabels;
    QGraphicsSimpleTextItem* ActivityLabelOrNull = nullptr;
    QGraphicsSimpleTextItem* SizeLabelOrNull = nullptr;
    QString ActivityText;
    QString CompactActivityText;
    float64 ActivityOpacity = 1;
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
    : QGraphicsView(parentOrNull), mDocument(document), mScene(new QGraphicsScene(this)),
      mFramingToolbar(new QToolBar(this)), mFitGraphAction(new QAction(QStringLiteral("Fit Graph"), this)),
      mFitSelectionAction(new QAction(QStringLiteral("Fit Selection"), this)), mZoomLabel(new QLabel(this)),
      mSelectionBox(new QRubberBand(QRubberBand::Rectangle, viewport()))
{
    setObjectName(QStringLiteral("renderGraphCanvas"));
    setAccessibleName(QStringLiteral("Render graph canvas"));
    setScene(mScene);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::NoDrag);
    setMouseTracking(true);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setBackgroundBrush(QColor(28, 31, 36));
    mFramingToolbar->setObjectName(QStringLiteral("renderGraphFramingToolbar"));
    mFramingToolbar->setAccessibleName(QStringLiteral("Canvas framing"));
    mFramingToolbar->setMovable(false);
    mFramingToolbar->setFloatable(false);
    mFramingToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mFitGraphAction->setObjectName(QStringLiteral("renderGraphFitGraph"));
    mFitSelectionAction->setObjectName(QStringLiteral("renderGraphFitSelection"));
    mFitGraphAction->setShortcut(QKeySequence(Qt::Key_Home));
    mFitSelectionAction->setShortcut(QKeySequence(Qt::Key_F));
    QAction* resetZoom = new QAction(QStringLiteral("100%"), this);
    resetZoom->setObjectName(QStringLiteral("renderGraphResetZoom"));
    resetZoom->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    mFitGraphAction->setToolTip(QStringLiteral("Frame all nodes (Home)"));
    mFitSelectionAction->setToolTip(QStringLiteral("Frame selected nodes (F)"));
    resetZoom->setToolTip(
        QStringLiteral("Restore 100% zoom (%1)").arg(resetZoom->shortcut().toString(QKeySequence::NativeText)));
    for (QAction* action : {mFitGraphAction, mFitSelectionAction, resetZoom})
    {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        mFramingToolbar->addAction(action);
    }
    mFramingToolbar->addSeparator();
    mZoomLabel->setObjectName(QStringLiteral("renderGraphZoomLabel"));
    mZoomLabel->setAccessibleName(QStringLiteral("Canvas zoom"));
    mFramingToolbar->addWidget(mZoomLabel);
    connect(mFitGraphAction, &QAction::triggered, this, &RenderGraphCanvas::FitGraph);
    connect(mFitSelectionAction, &QAction::triggered, this, &RenderGraphCanvas::FitSelection);
    connect(resetZoom, &QAction::triggered, this, &RenderGraphCanvas::ResetZoom);
    mNavigation = new RenderGraphNavigation(*mFramingToolbar, *this);
    mDiagnostics = new RenderGraphDiagnostics(mDocument, *mFramingToolbar, *this);
    mHistoryOrNull = new RenderGraphHistory(mDocument, *this);
    mCommentsOrNull = new RenderGraphComments(*this, *mScene);
    mReroutesOrNull = new RenderGraphReroutes(*this);
    connect(mDiagnostics, &RenderGraphDiagnostics::Changed, mCommentsOrNull, &RenderGraphComments::RefreshCounts);
    connect(mReroutesOrNull, &RenderGraphReroutes::PreviewChanged, this, &RenderGraphCanvas::updateEdges);
    setViewportMargins(0, mFramingToolbar->sizeHint().height() + mHistoryOrNull->sizeHint().height(), 0, 0);
    connect(mNavigation, &RenderGraphNavigation::TraceChanged, this, &RenderGraphCanvas::updateConnectionHighlight);
    connect(mNavigation, &RenderGraphNavigation::NodeRequested, this, [this](const QString& identity) {
        const NodeView* viewOrNull = mNodes.value(identity, nullptr);
        if (viewOrNull == nullptr || viewOrNull->IsMovable == false)
        {
            return;
        }
        mHoveredPort.clear();
        mCommentsOrNull->RevealNode(identity);
        SelectNode(identity);
        notifySelection();
        Q_EMIT NodeSelected(identity);
        FitSelection();
    });
    updateFramingControls();
    updateSemanticZoom();
    setToolTip(QStringLiteral(
        "Left-drag empty space to box-select · Shift adds to selection · middle-drag or "
        "Space+left-drag pans (two-finger scroll on a touchpad) · Copy/Paste reuses node groups "
        "· drag nodes to arrange them (one "
        "undo step per drag) · drag from an output port to an "
        "input port to connect or replace · right-click nodes, wires or ports to edit · "
        "double-click a wire to add a reroute point · drag points to move them · "
        "Delete removes the selected point, node or link · double-click a "
        "Subgraph to enter it, or any other node to show details · pinch, wheel or Ctrl+scroll "
        "zooms around the pointer · "
        "Home or two-finger double-tap frames the graph · F frames selection · Ctrl/Command+0 resets zoom"));
}

RenderGraphCanvas::~RenderGraphCanvas()
{
    delete mReroutesOrNull;
    delete mCommentsOrNull;
    qDeleteAll(mNodes);
    qDeleteAll(mEdges);
}

RenderGraphReroutes& RenderGraphCanvas::GetReroutes() noexcept
{
    return *mReroutesOrNull;
}
RenderGraphComments& RenderGraphCanvas::GetComments() noexcept
{
    return *mCommentsOrNull;
}
RenderGraphDiagnostics& RenderGraphCanvas::GetDiagnostics() noexcept
{
    return *mDiagnostics;
}
uint64 RenderGraphCanvas::GetDocumentRevision() const noexcept
{
    return mDocument.GetRevision();
}
QVector<RenderGraphLayoutNode> RenderGraphCanvas::GetAuthoredNodeBounds() const
{
    QVector<RenderGraphLayoutNode> nodes;
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->IsMovable == true)
        {
            nodes.append({view->Id, QRectF(view->Frame->pos(), view->Frame->rect().size())});
        }
    }
    return nodes;
}
void RenderGraphCanvas::PreviewNodePositions(const QVector<RenderGraphNodePosition>& positions)
{
    for (const RenderGraphNodePosition& position : positions)
    {
        NodeView* viewOrNull = mNodes.value(position.Node, nullptr);
        if (viewOrNull != nullptr && viewOrNull->IsMovable == true)
        {
            viewOrNull->Frame->setPos(ClampCoordinate(position.X), ClampCoordinate(position.Y));
        }
    }
    updateEdges();
}

void RenderGraphCanvas::ShowConfiguration(const QString& file, const QString& configuration)
{
    mHistoryOrNull->BeforeShow(file, configuration);
    const bool isSame = file == mFile && configuration == mConfiguration;
    if (isSame == false)
    {
        mCommentsOrNull->SetRevealedFrames({});
        mNavigation->SetTraceDirection(RenderGraphTraceDirection::None);
    }
    mFile = file;
    mConfiguration = configuration;
    const bool wasEmpty = mNodes.isEmpty();
    Rebuild();
    if (isSame == false || (wasEmpty == true && mNodes.isEmpty() == false))
    {
        // A different configuration starts with no selection and fits its contents.
        SelectNode({});
        mShouldFitOnShow = true;
        if (isVisible() == true)
        {
            FitGraph();
        }
    }
    mHistoryOrNull->AfterShow();
}

void RenderGraphCanvas::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
    // A new graph can be loaded while the empty-state page still hides this view.
    // Fit only that pending configuration after the splitter has laid out its viewport.
    QTimer::singleShot(0, this, [this]() {
        if (mShouldFitOnShow == true)
        {
            FitGraph();
        }
    });
}

void RenderGraphCanvas::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (mHistoryOrNull == nullptr)
    {
        return;
    }
    const int32 historyHeight = mHistoryOrNull->sizeHint().height();
    const int32 framingHeight = mFramingToolbar->sizeHint().height();
    const int32 height = historyHeight + framingHeight;
    if (viewportMargins().top() != height)
    {
        setViewportMargins(0, height, 0, 0);
    }
    const int32 toolbarWidth = width() - 2 * frameWidth();
    mHistoryOrNull->setGeometry(frameWidth(), frameWidth(), toolbarWidth, historyHeight);
    mFramingToolbar->setGeometry(frameWidth(), frameWidth() + historyHeight, toolbarWidth, framingHeight);
}

void RenderGraphCanvas::paintEvent(QPaintEvent* event)
{
    // Also handles a transform applied by a host/test through QGraphicsView's API.
    updateSemanticZoom();
    QGraphicsView::paintEvent(event);
}

void RenderGraphCanvas::FitGraph()
{
    QRectF bounds;
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Frame->isVisible() == true)
        {
            bounds = bounds.united(view->Frame->sceneBoundingRect());
        }
    }
    bounds = bounds.united(mCommentsOrNull->GetBounds()).united(mReroutesOrNull->GetBounds());
    fitBounds(bounds);
    mShouldFitOnShow = false;
}

void RenderGraphCanvas::FitSelection()
{
    QRectF bounds;
    for (const QString& identity : GetSelectedNodes())
    {
        const QString stage = mCommentsOrNull->GetCollapsedFrame(identity);
        bounds = bounds.united(stage.isEmpty() == true ? mNodes.value(identity)->Frame->sceneBoundingRect()
                                                       : mCommentsOrNull->GetFrameBounds(stage));
    }
    fitBounds(bounds);
}

void RenderGraphCanvas::fitBounds(const QRectF& bounds)
{
    if (bounds.isEmpty() == true || viewport()->width() <= 1 || viewport()->height() <= 1)
    {
        return;
    }
    fitInView(bounds.adjusted(-FRAME_PADDING, -FRAME_PADDING, FRAME_PADDING, FRAME_PADDING), Qt::KeepAspectRatio);
    const float64 current = transform().m11();
    const float64 target = math::Clamp(current, MINIMUM_VIEW_SCALE, MAXIMUM_VIEW_SCALE);
    scale(target / current, target / current);
    centerOn(bounds.center());
    updateSemanticZoom();
}

void RenderGraphCanvas::ResetZoom()
{
    const QPointF position = viewport()->rect().center();
    const QPointF before = viewportTransform().inverted().map(position);
    resetTransform();
    const QPointF after = viewportTransform().inverted().map(position);
    const QPointF offset = after - before;
    translate(offset.x(), offset.y());
    updateSemanticZoom();
}

RenderGraphViewState RenderGraphCanvas::CaptureViewState() const
{
    return
    {
        .Selection = GetSelectedNodes(),
        .CentreScene =
            viewportTransform().inverted().map(QPointF(viewport()->width() * 0.5, viewport()->height() * 0.5)),
        .Zoom = math::Clamp(transform().m11(), MINIMUM_VIEW_SCALE, MAXIMUM_VIEW_SCALE),
        .Trace = mNavigation->GetTraceDirection(),
        .RevealedFrames = mCommentsOrNull->GetRevealedFrames(),
    };
}

bool RenderGraphCanvas::RestoreViewState(const RenderGraphViewState& state)
{
    if (math::IsFinite(state.Zoom) == false || state.Zoom < MINIMUM_VIEW_SCALE || state.Zoom > MAXIMUM_VIEW_SCALE ||
        math::IsFinite(state.CentreScene.x()) == false || math::IsFinite(state.CentreScene.y()) == false ||
        math::Abs(state.CentreScene.x()) > MAX_RESTORED_CENTRE_COORDINATE ||
        math::Abs(state.CentreScene.y()) > MAX_RESTORED_CENTRE_COORDINATE ||
        (state.Trace != RenderGraphTraceDirection::None && state.Trace != RenderGraphTraceDirection::Upstream &&
         state.Trace != RenderGraphTraceDirection::Downstream))
    {
        return false;
    }
    mShouldFitOnShow = false;
    mHoveredPort.clear();
    mCommentsOrNull->SetRevealedFrames(state.RevealedFrames);
    SelectNodes(state.Selection);
    mNavigation->SetTraceDirection(state.Trace);
    applyViewTransform(state);
    notifySelection();
    const QStringList selected = GetSelectedNodes();
    if (selected.isEmpty() == false)
    {
        Q_EMIT NodeSelected(selected.first());
    }
    return true;
}

void RenderGraphCanvas::applyViewTransform(const RenderGraphViewState& state)
{
    if (math::IsFinite(state.Zoom) == false || state.Zoom <= 0 || math::IsFinite(state.CentreScene.x()) == false ||
        math::IsFinite(state.CentreScene.y()) == false)
    {
        return;
    }
    // Qt clamps panning to the scene rectangle. Include the restored viewport so its centre
    // remains reachable even after a resize or a graph edit that shrinks the node bounds.
    const float64 halfWidthScene = static_cast<float64>(viewport()->width()) / state.Zoom / 2 + FRAME_PADDING;
    const float64 halfHeightScene = static_cast<float64>(viewport()->height()) / state.Zoom / 2 + FRAME_PADDING;
    const QRectF visibleScene(state.CentreScene.x() - halfWidthScene,
                              state.CentreScene.y() - halfHeightScene,
                              2 * halfWidthScene,
                              2 * halfHeightScene);
    mScene->setSceneRect(mScene->sceneRect().united(visibleScene));
    setTransform(QTransform::fromScale(state.Zoom, state.Zoom));
    centerOn(state.CentreScene);
    const QPointF centreScene =
        viewportTransform().inverted().map(QPointF(viewport()->width() * 0.5, viewport()->height() * 0.5));
    const QPointF offsetScene = centreScene - state.CentreScene;
    translate(offsetScene.x(), offsetScene.y());
    updateSemanticZoom();
}

RenderGraphHistory& RenderGraphCanvas::GetHistory() noexcept
{
    return *mHistoryOrNull;
}

void RenderGraphCanvas::updateFramingControls()
{
    mFitGraphAction->setEnabled(mNodes.isEmpty() == false);
    mFitSelectionAction->setEnabled(GetSelectedNodes().isEmpty() == false);
}

void RenderGraphCanvas::updateSemanticZoom(bool shouldForce)
{
    const float64 viewScale = transform().m11();
    if (math::IsFinite(viewScale) == false || viewScale <= 0 || (shouldForce == false && viewScale == mPresentedScale))
    {
        return;
    }
    mPresentedScale = viewScale;
    mZoomLabel->setText(QStringLiteral("%1%").arg(viewScale * PERCENT_SCALE, 0, 'f', 0));
    const bool hasDetails = viewScale >= DETAIL_VIEW_SCALE;
    const bool hasPortLabels = viewScale >= PORT_LABEL_VIEW_SCALE;
    const bool hasReadableCards = NODE_WIDTH * viewScale >= MINIMUM_TEXT_CARD_WIDTH;
    mZoomLabel->setToolTip(hasDetails == true ? QStringLiteral("Detailed view: ports, activity and resource metadata")
                           : hasPortLabels == true
                               ? QStringLiteral("Working view: ports and compact activity")
                               : QStringLiteral("Overview: node identities and compact activity; hover for details"));
    for (NodeView* view : std::as_const(mNodes))
    {
        if (view->TitleLabelOrNull != nullptr)
        {
            QGraphicsSimpleTextItem& title = *view->TitleLabelOrNull;
            title.setVisible(hasReadableCards);
            SetScaledLabel(title,
                           hasDetails == true ? view->Title
                                              : (view->Id == OUTPUTS_NODE ? QStringLiteral("outputs") : view->Id),
                           viewScale,
                           CanvasLabelRole::Title,
                           NODE_WIDTH - 2 * NODE_PADDING);
            title.setPos(NODE_PADDING,
                         math::Max(0.0, (HEADER_HEIGHT - title.boundingRect().height() * title.scale()) / 2));
        }
        for (bool isOutput : {false, true})
        {
            const QHash<QString, QGraphicsSimpleTextItem*>& labels =
                isOutput == true ? view->OutputLabels : view->InputLabels;
            const QHash<QString, QGraphicsEllipseItem*>& connectors =
                isOutput == true ? view->OutputItems : view->InputItems;
            for (QHash<QString, QGraphicsSimpleTextItem*>::const_iterator label = labels.cbegin();
                 label != labels.cend();
                 ++label)
            {
                QGraphicsSimpleTextItem& name = *label.value();
                name.setVisible(hasPortLabels);
                const float64 availableWidth = NODE_WIDTH / 2 - PORT_RADIUS - 2 * PORT_LABEL_GAP;
                SetScaledLabel(name, label.key(), viewScale, CanvasLabelRole::Annotation, availableWidth);
                const float64 x = isOutput == true ? NODE_WIDTH - PORT_RADIUS - PORT_LABEL_GAP -
                                                         name.boundingRect().width() * name.scale()
                                                   : PORT_RADIUS + PORT_LABEL_GAP;
                const float64 y = connectors.value(label.key())->rect().center().y();
                name.setPos(x, y - name.boundingRect().height() * name.scale() / 2);
            }
        }
        if (view->ActivityLabelOrNull != nullptr)
        {
            QGraphicsSimpleTextItem& activity = *view->ActivityLabelOrNull;
            activity.setVisible(
                hasDetails == true ||
                (hasReadableCards == true && view->Frame->rect().height() * viewScale >= MINIMUM_STATUS_CARD_HEIGHT));
            SetScaledLabel(activity,
                           hasDetails == true ? view->ActivityText : view->CompactActivityText,
                           viewScale,
                           CanvasLabelRole::Annotation,
                           NODE_WIDTH - 2 * NODE_PADDING);
            activity.setPos(NODE_PADDING,
                            hasDetails == true ? view->Frame->rect().height() - FEEDBACK_HEIGHT
                                               : view->Frame->rect().height() - NODE_PADDING -
                                                     activity.boundingRect().height() * activity.scale());
        }
        if (view->SizeLabelOrNull != nullptr)
        {
            view->SizeLabelOrNull->setVisible(hasDetails);
        }
    }
    if (mDiagnosticLabelOrNull != nullptr)
    {
        SetScaledLabel(*mDiagnosticLabelOrNull,
                       hasReadableCards == true ? mDiagnosticText : QStringLiteral("!"),
                       viewScale,
                       CanvasLabelRole::Title,
                       hasReadableCards == true ? NODE_WIDTH - 2 * NODE_PADDING : NODE_WIDTH / viewScale);
    }
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
    const RenderGraphViewState viewState = CaptureViewState();
    const QStringList selected = GetSelectedNodes();
    ClearDiagnostic();
    cancelLink();
    mIsDragging = false;
    mIsBoxSelecting = false;
    mSelectionBox->hide();
    mDragNodes.clear();
    mHoveredPort.clear();
    qDeleteAll(mNodes);
    mNodes.clear();
    mHiddenSelection.clear();
    qDeleteAll(mEdges);
    mEdges.clear();
    mCommentsOrNull->Clear();
    mReroutesOrNull->Clear();
    mScene->clear();
    const QJsonObject record = configuration();
    if (record.isEmpty() == true)
    {
        rebuildNavigation();
        updateFramingControls();
        updateSemanticZoom(true);
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
    QHash<int32, float64> rows;
    int32 lastColumn = 0;
    // Identity and display title are distinct named strings.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    const auto addNode = [this](const QString& identity,
                                const QString& title,
                                RenderGraphDomain domain,
                                const RenderGraphNodePorts& ports,
                                QPointF position,
                                const QJsonArray& writableResources) {
        // NOLINTEND(bugprone-easily-swappable-parameters)
        QHash<QString, qsizetype> outputRows;
        qsizetype rowCount = qMax(ports.Inputs.size(), ports.Outputs.size());
        for (const QJsonValueConstRef value : writableResources)
        {
            const QJsonObject resource = value.toObject();
            for (qsizetype row = 0; row < ports.Inputs.size(); ++row)
            {
                if (ports.Inputs.at(row).Id == resource.value(QStringLiteral("target")).toString())
                {
                    outputRows.insert(resource.value(QStringLiteral("id")).toString(), row);
                    rowCount = qMax(rowCount, row + 1);
                }
            }
        }
        const float64 height =
            HEADER_HEIGHT + static_cast<float64>(rowCount) * PORT_SPACING + NODE_PADDING + FEEDBACK_HEIGHT;
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
        view->TitleLabelOrNull = label;
        view->Title = title;
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        label->setText(QFontMetricsF(font).elidedText(title, Qt::ElideRight, NODE_WIDTH - 2 * NODE_PADDING));
        label->setToolTip(title);
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
            connector->setToolTip(
                QStringLiteral(
                    "%1.%2 : %3\nRight-click to connect or disconnect. Dropping a new wire replaces the input source.")
                    .arg(view->Id, port.Id, port.Type));
            QGraphicsSimpleTextItem* name = new QGraphicsSimpleTextItem(port.Id, view->Frame);
            QFont small = name->font();
            small.setPointSize(LABEL_POINT_SIZE);
            name->setFont(small);
            name->setBrush(QColor(214, 218, 224));
            name->setToolTip(connector->toolTip());
            const float64 labelX =
                isOutput == true ? NODE_WIDTH - PORT_RADIUS - 4 - name->boundingRect().width() : PORT_RADIUS + 4;
            name->setPos(labelX, y - name->boundingRect().height() / 2);
            (isOutput == true ? view->OutputItems : view->InputItems).insert(port.Id, connector);
            (isOutput == true ? view->OutputLabels : view->InputLabels).insert(port.Id, name);
        };
        for (qsizetype i = 0; i < ports.Inputs.size(); ++i)
        {
            addPort(ports.Inputs.at(i), i, false);
        }
        for (qsizetype i = 0; i < ports.Outputs.size(); ++i)
        {
            addPort(ports.Outputs.at(i), outputRows.value(ports.Outputs.at(i).Id, i), true);
        }
        view->ActivityLabelOrNull = new QGraphicsSimpleTextItem(view->Frame);
        view->SizeLabelOrNull = new QGraphicsSimpleTextItem(view->Frame);
        for (QGraphicsSimpleTextItem* annotation : {view->ActivityLabelOrNull, view->SizeLabelOrNull})
        {
            QFont small = annotation->font();
            small.setPointSize(LABEL_POINT_SIZE);
            annotation->setFont(small);
            annotation->setBrush(QColor::fromRgba(LABEL_COLOR));
        }
        view->ActivityLabelOrNull->setPos(NODE_PADDING, height - FEEDBACK_HEIGHT);
        view->SizeLabelOrNull->setPos(NODE_PADDING, height - FEEDBACK_HEIGHT / 2);
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
        if (IsSceneBundleExpanded(identity) == false)
        {
            HideUnusedSceneMembers(record, node, ports);
        }
        const int32 column = columns.value(identity);
        lastColumn = qMax(lastColumn, column);
        const float64 row = rows.value(column);
        const QPointF position =
            authored.contains(identity) == true ? authored.value(identity) : QPointF(column * COLUMN_SPACING, row);
        NodeView* view =
            addNode(identity,
                    identity + QStringLiteral(" : ") + kind +
                        (hasPorts == true ? QString() : QStringLiteral(" (missing source)")),
                    GetRenderGraphDomain(kind),
                    ports,
                    position,
                    IsPipelineNode(kind) == true ? node.value(QStringLiteral("outputs")).toArray() : QJsonArray{});
        view->Kind = kind;
        rows.insert(column, row + qMax(ROW_SPACING, view->Frame->rect().height() + NODE_ROW_GAP));
    }
    const QVector<RenderGraphPort> inputs =
        ReadRenderGraphInterfacePorts(record.value(QStringLiteral("inputs")).toArray());
    if (inputs.isEmpty() == false)
    {
        NodeView* view = addNode(INPUT_NODE,
                                 QStringLiteral("input (configuration)"),
                                 RenderGraphDomain::Interface,
                                 {{}, inputs},
                                 QPointF(-COLUMN_SPACING, 0),
                                 {});
        view->IsMovable = false;
    }
    QVector<CanvasLink> links;
    const QJsonArray outputs = record.value(QStringLiteral("outputs")).toArray();
    if (outputs.isEmpty() == false)
    {
        NodeView* view = addNode(OUTPUTS_NODE,
                                 QStringLiteral("outputs (configuration)"),
                                 RenderGraphDomain::Interface,
                                 {ReadRenderGraphInterfacePorts(outputs), {}},
                                 QPointF((lastColumn + 1) * COLUMN_SPACING, 0),
                                 {});
        view->IsMovable = false;
        for (const QJsonValueConstRef output : outputs)
        {
            const QString source = output.toObject().value(QStringLiteral("source")).toString();
            if (source.isEmpty() == false)
            {
                links.append(
                    {source,
                     OUTPUTS_NODE + QLatin1Char('.') + output.toObject().value(QStringLiteral("id")).toString(),
                     true});
            }
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
    mReroutesOrNull->Rebuild(record);
    for (EdgeView* edge : std::as_const(mEdges))
    {
        mReroutesOrNull->BindWire(edge->From, edge->To, *edge->Path);
        edge->Path->setZValue(-1);
        if (edge->IsEditable == false)
        {
            edge->Path->setPen(QPen(QColor(150, 150, 150), 1.5, Qt::DashLine));
            edge->Path->setToolTip(QStringLiteral("Configuration output %1").arg(GetEndpointPort(edge->To)));
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
    rebuildNavigation();
    refreshFeedback();
    updateSelection();
    mCommentsOrNull->Rebuild(record);
    QRectF nodeBounds = mCommentsOrNull->GetBounds().united(mReroutesOrNull->GetBounds());
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Frame->isVisible() == true)
        {
            nodeBounds = nodeBounds.united(view->Frame->sceneBoundingRect());
        }
    }
    mScene->setSceneRect(
        nodeBounds.adjusted(-SCENE_SCROLL_PADDING, -SCENE_SCROLL_PADDING, SCENE_SCROLL_PADDING, SCENE_SCROLL_PADDING));
    applyViewTransform(viewState);
    updateSemanticZoom(true);
}

void RenderGraphCanvas::SetReferenceSize(const QSize& size)
{
    if (mReferenceSize != size)
    {
        mReferenceSize = size;
        refreshFeedback();
        Q_EMIT ReferenceSizeChanged(size);
    }
}

void RenderGraphCanvas::refreshFeedback()
{
    QHash<QString, RenderGraphResourceFeedback> resourceCache;
    const QJsonObject record = configuration();
    const QHash<QString, RenderGraphActivityFeedback> activity =
        InspectRenderGraphActivity(mDocument, mFile, mConfiguration);
    const auto findSource = [&record](const QString& endpoint) {
        if (endpoint.startsWith(OUTPUTS_NODE + QLatin1Char('.')) == true)
        {
            for (const QJsonValueConstRef value : record.value(QStringLiteral("outputs")).toArray())
            {
                const QJsonObject output = value.toObject();
                if (output.value(QStringLiteral("id")).toString() == GetEndpointPort(endpoint))
                {
                    return output.value(QStringLiteral("source")).toString();
                }
            }
        }
        for (const QJsonValueConstRef value : record.value(QStringLiteral("edges")).toArray())
        {
            if (value.toObject().value(QStringLiteral("to")).toString() == endpoint)
            {
                return value.toObject().value(QStringLiteral("from")).toString();
            }
        }
        return QString();
    };
    for (NodeView* view : std::as_const(mNodes))
    {
        const RenderGraphActivityFeedback state = activity.value(view->Id);
        const bool isInterface = view->Domain == RenderGraphDomain::Interface;
        view->ActivityOpacity =
            (state.Activity == RenderGraphActivity::Inactive || state.Activity == RenderGraphActivity::Disabled) &&
                    isInterface == false
                ? INACTIVE_OPACITY
                : 1;
        view->Frame->setOpacity(view->ActivityOpacity);
        QString explanation = isInterface == true
                                  ? QStringLiteral("Configuration interface. Library inspection assumes all exposed "
                                                   "outputs are requested; caller instances can use fewer.")
                                  : state.Explanation;
        if (explanation.isEmpty() == true)
        {
            explanation = QStringLiteral("Activity unknown: the draft cannot be inspected within the authoring bounds. "
                                         "Validate for diagnostics.");
        }
        const QString label = isInterface == true             ? QStringLiteral("Configuration interface")
                              : state.Label.isEmpty() == true ? QStringLiteral("Activity unknown")
                                                              : state.Label;
        view->ActivityText = label;
        view->CompactActivityText = isInterface == true                               ? QStringLiteral("Interface")
                                    : state.Activity == RenderGraphActivity::Retained ? QStringLiteral("Draft route")
                                    : state.Activity == RenderGraphActivity::Inactive ? QStringLiteral("Inactive")
                                    : state.Activity == RenderGraphActivity::Disabled ? QStringLiteral("Disabled")
                                                                                      : QStringLiteral("Unknown");
        if (view->ActivityLabelOrNull != nullptr)
        {
            view->ActivityLabelOrNull->setText(QFontMetricsF(view->ActivityLabelOrNull->font())
                                                   .elidedText(label, Qt::ElideRight, NODE_WIDTH - 2 * NODE_PADDING));
            view->ActivityLabelOrNull->setToolTip(explanation);
        }
        QString sizeLabel;
        QSet<QString> sizes;
        QStringList resources;
        for (bool isOutput : {true, false})
        {
            const QVector<RenderGraphPort>& ports = isOutput == true ? view->Ports.Outputs : view->Ports.Inputs;
            for (const RenderGraphPort& port : ports)
            {
                const QString endpoint = view->Id + QLatin1Char('.') + port.Id;
                const QString source = isOutput == true ? endpoint : findSource(endpoint);
                QString tooltip = QStringLiteral("%1 : %2").arg(endpoint, port.Type);
                if (isOutput == false)
                {
                    tooltip += source.isEmpty() == true ? QStringLiteral("\nUnconnected input")
                                                        : QStringLiteral("\nSource: ") + source;
                }
                if (port.Type == QStringLiteral("ColorTexture") || port.Type == QStringLiteral("DepthTexture") ||
                    port.Type == QStringLiteral("Texture") || port.Type == QStringLiteral("RenderSize") ||
                    port.Type == QStringLiteral("Buffer"))
                {
                    if (resourceCache.contains(source) == false)
                    {
                        resourceCache.insert(
                            source,
                            DescribeRenderGraphResource(mDocument, mFile, mConfiguration, source, mReferenceSize));
                    }
                    const RenderGraphResourceFeedback resource = resourceCache.value(source);
                    tooltip += QLatin1Char('\n') + resource.Description;
                    resources.append(endpoint + QStringLiteral(": ") + resource.Description);
                    sizes.insert(resource.SizeLabel);
                    if (sizeLabel.isEmpty() == true)
                    {
                        sizeLabel = resource.SizeLabel;
                    }
                }
                tooltip += QStringLiteral("\nRight-click to connect/disconnect; drag an output onto an input to check "
                                          "compatibility or replace its source.");
                QGraphicsSimpleTextItem* portLabelOrNull =
                    (isOutput == true ? view->OutputLabels : view->InputLabels).value(port.Id, nullptr);
                if (portLabelOrNull != nullptr)
                {
                    portLabelOrNull->setToolTip(tooltip);
                }
                QGraphicsEllipseItem* connectorOrNull =
                    (isOutput == true ? view->OutputItems : view->InputItems).value(port.Id, nullptr);
                if (connectorOrNull != nullptr)
                {
                    if (connectorOrNull == mLinkPortOrNull)
                    {
                        mLinkPortTooltip = tooltip;
                    }
                    else
                    {
                        connectorOrNull->setToolTip(tooltip);
                    }
                }
            }
        }
        if (sizes.size() > 1)
        {
            sizeLabel = QStringLiteral("Mixed sizes (hover ports)");
        }
        if (view->Kind == QStringLiteral("Accumulate"))
        {
            const QJsonObject node = FindNode(configuration(), view->Id);
            sizeLabel = QStringLiteral("Mean · %1 samples").arg(node.value(QStringLiteral("samples")).toInt());
            resources.append(QStringLiteral("Linear RGB accumulation before display conversion. "
                                            "Resets on camera, scene, size or shader replacement. "
                                            "Disable this node for individual samples."));
        }
        if (view->Kind == QStringLiteral("Denoise"))
        {
            const QJsonObject node = FindNode(configuration(), view->Id);
            sizeLabel = QStringLiteral("OIDN · %1").arg(node.value(QStringLiteral("quality")).toString());
            resources.append(QStringLiteral("Linear HDR denoising before ToneMap. Requires OIDN and Metal. "
                                            "Fast favors previews; Balanced favors interaction; High favors quality."));
        }
        if (view->Kind == QStringLiteral("ToneMap"))
        {
            const QJsonObject node = FindNode(configuration(), view->Id);
            sizeLabel =
                QStringLiteral("Reinhard · %1× exposure").arg(node.value(QStringLiteral("exposure")).toDouble());
            resources.append(QStringLiteral("Exposure, Reinhard compression and sRGB encoding after optional "
                                            "linear accumulation. Disable for clamped linear RGB."));
        }
        view->Frame->setToolTip(view->Id + QStringLiteral(" : ") + view->Kind + QStringLiteral("\n") + explanation +
                                QLatin1Char('\n') + resources.join(QLatin1Char('\n')));
        if (view->SizeLabelOrNull != nullptr)
        {
            view->SizeLabelOrNull->setText(QFontMetricsF(view->SizeLabelOrNull->font())
                                               .elidedText(sizeLabel, Qt::ElideRight, NODE_WIDTH - 2 * NODE_PADDING));
            view->SizeLabelOrNull->setToolTip(resources.join(QLatin1Char('\n')));
        }
    }
    updateConnectionHighlight();
    updateSemanticZoom(true);
}

void RenderGraphCanvas::updateLinkFeedback(QPointF position)
{
    const auto positionHint = [this, position]() {
        if (mLinkHintOrNull == nullptr)
        {
            return;
        }
        const QRectF visible = mapToScene(viewport()->rect())
                                   .boundingRect()
                                   .adjusted(NODE_PADDING, NODE_PADDING, -NODE_PADDING, -NODE_PADDING);
        const QRectF hint = mLinkHintOrNull->boundingRect();
        mLinkHintOrNull->setPos(
            math::Max(visible.left(), math::Min(position.x() + LINK_HINT_OFFSET, visible.right() - hint.width())),
            math::Max(visible.top(), math::Min(position.y() + LINK_HINT_OFFSET, visible.bottom() - hint.height())));
    };
    bool isOutput = false;
    const QString endpoint = portAt(position, isOutput);
    if (endpoint == mLinkEndpoint && mLinkHintOrNull != nullptr)
    {
        positionHint();
        return;
    }
    mLinkEndpoint = endpoint;
    if (mLinkPortOrNull != nullptr)
    {
        mLinkPortOrNull->setPen(QPen(QColor::fromRgba(NODE_BORDER_COLOR), 1));
        mLinkPortOrNull->setToolTip(mLinkPortTooltip);
        mLinkPortOrNull = nullptr;
    }
    QString message = QStringLiteral(
        "Drop on an input port to connect or replace its source, or on empty canvas to add a connected node.");
    QColor color = QColor::fromRgba(EDITABLE_EDGE_COLOR);
    if (endpoint.isEmpty() == false && endpoint != mLinkFrom)
    {
        const RenderGraphLinkCheck check =
            isOutput == true ? RenderGraphLinkCheck{QStringLiteral("Port"),
                                                    QStringLiteral("Links end at an input; this is an output.")}
                             : CheckRenderGraphLink(mDocument, mFile, mConfiguration, mLinkFrom, endpoint, true);
        color = QColor::fromRgba(check.IsLegal() == true ? LINK_ACCEPT_COLOR : LINK_REJECT_COLOR);
        message = check.IsLegal() == true
                      ? QStringLiteral("Compatible: %1 → %2. Drop to connect or replace.").arg(mLinkFrom, endpoint)
                      : check.Code + QStringLiteral(": ") + check.Reason;
        NodeView* viewOrNull = mNodes.value(GetEndpointNode(endpoint), nullptr);
        if (viewOrNull != nullptr)
        {
            mLinkPortOrNull = (isOutput == true ? viewOrNull->OutputItems : viewOrNull->InputItems)
                                  .value(GetEndpointPort(endpoint), nullptr);
            if (mLinkPortOrNull != nullptr)
            {
                mLinkPortTooltip = mLinkPortOrNull->toolTip();
                mLinkPortOrNull->setToolTip(message);
                mLinkPortOrNull->setPen(QPen(color, 2));
            }
        }
    }
    if (mLinkHintOrNull != nullptr)
    {
        mLinkHintOrNull->setHtml(
            QStringLiteral("<p style=\"background-color:%1; color:%2;\">%3</p>")
                .arg(QColor::fromRgba(NODE_BACKGROUND_COLOR).name(), color.name(), message.toHtmlEscaped()));
        positionHint();
    }
    if (mLinkPreviewOrNull != nullptr)
    {
        mLinkPreviewOrNull->setPen(QPen(color, 2));
    }
}

bool RenderGraphCanvas::IsSceneBundleExpanded(const QString& node) const
{
    return mExpandedSceneBundles.contains(mFile + QLatin1Char('#') + mConfiguration + QLatin1Char('/') + node);
}
void RenderGraphCanvas::SetSceneBundleExpanded(const QString& node, bool isExpanded)
{
    const QString key = mFile + QLatin1Char('#') + mConfiguration + QLatin1Char('/') + node;
    mExpandedSceneBundles.removeAll(key);
    if (isExpanded == true)
    {
        mExpandedSceneBundles.append(key);
    }
    ShowConfiguration(mFile, mConfiguration);
}

bool RenderGraphCanvas::IsWireCollapsed(const QString& from, const QString& to) const
{
    return mCommentsOrNull->GetCollapsedFrame(GetEndpointNode(from)).isEmpty() == false ||
           mCommentsOrNull->GetCollapsedFrame(GetEndpointNode(to)).isEmpty() == false;
}

void RenderGraphCanvas::RefreshCommentPresentation()
{
    const RenderGraphViewState viewState = CaptureViewState();
    const QStringList selection = GetSelectedNodes();
    // Qt clears selection when hiding a card. Retain the logical IDs before changing visibility.
    mHiddenSelection = selection;
    for (NodeView* view : std::as_const(mNodes))
    {
        view->Frame->setVisible(mCommentsOrNull->GetCollapsedFrame(view->Id).isEmpty());
    }
    SelectNodes(selection);
    updateEdges();
    updateConnectionHighlight();
    QRectF bounds = mCommentsOrNull->GetBounds().united(mReroutesOrNull->GetBounds());
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Frame->isVisible() == true)
        {
            bounds = bounds.united(view->Frame->sceneBoundingRect());
        }
    }
    // Revealed nodes may be outside the compact stage's former scroll range. Growing it must
    // preserve the current view; explicit search/diagnostic reveal performs the subsequent pan.
    mScene->setSceneRect(mScene->sceneRect().united(
        bounds.adjusted(-SCENE_SCROLL_PADDING, -SCENE_SCROLL_PADDING, SCENE_SCROLL_PADDING, SCENE_SCROLL_PADDING)));
    applyViewTransform(viewState);
}

void RenderGraphCanvas::updateEdges()
{
    for (EdgeView* edge : std::as_const(mEdges))
    {
        const QString sourceStage = mCommentsOrNull->GetCollapsedFrame(GetEndpointNode(edge->From));
        const QString destinationStage = mCommentsOrNull->GetCollapsedFrame(GetEndpointNode(edge->To));
        edge->Path->setVisible(sourceStage.isEmpty() == true || sourceStage != destinationStage);
        const QPointF from = GetPortPosition(edge->From, true);
        const QPointF to = GetPortPosition(edge->To, false);
        if (from.isNull() == true || to.isNull() == true)
        {
            // A dangling endpoint stays visible as a short stub; validation names it.
            edge->Path->setPath(QPainterPath());
            continue;
        }
        QPainterPath path(from);
        QPointF previous = from;
        const QVector<QPointF> points = IsWireCollapsed(edge->From, edge->To) == true
                                            ? QVector<QPointF>{}
                                            : mReroutesOrNull->GetPoints(edge->From, edge->To);
        for (QPointF point : points)
        {
            path.connectPath(MakeCurve(previous, point));
            previous = point;
        }
        path.connectPath(MakeCurve(previous, to));
        edge->Path->setPath(path);
    }
    mReroutesOrNull->RefreshStyles();
}

int32 RenderGraphCanvas::findRerouteSegment(const QString& from, const QString& to, QPointF position) const
{
    QVector<QPointF> points = mReroutesOrNull->GetPoints(from, to);
    points.prepend(GetPortPosition(from, true));
    points.append(GetPortPosition(to, false));
    QPainterPathStroker stroker;
    stroker.setWidth(EDGE_HIT_WIDTH);
    for (int32 index = 0; index + 1 < points.size(); ++index)
    {
        if (stroker.createStroke(MakeCurve(points.at(index), points.at(index + 1))).contains(position) == true)
        {
            return index;
        }
    }
    return static_cast<int32>(points.size()) - 2;
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
    const QGraphicsEllipseItem* boundaryOrNull = mCommentsOrNull->GetBoundaryPortOrNull(endpoint, isOutput);
    if (boundaryOrNull != nullptr)
    {
        return boundaryOrNull->mapToScene(boundaryOrNull->rect().center());
    }
    const NodeView* viewOrNull = mNodes.value(GetEndpointNode(endpoint), nullptr);
    if (viewOrNull == nullptr)
    {
        return {};
    }
    const QGraphicsEllipseItem* connectorOrNull =
        (isOutput == true ? viewOrNull->OutputItems : viewOrNull->InputItems).value(GetEndpointPort(endpoint), nullptr);
    return connectorOrNull == nullptr ? QPointF() : connectorOrNull->mapToScene(connectorOrNull->rect().center());
}

QString RenderGraphCanvas::GetPortType(const QString& endpoint, bool isOutput) const
{
    const NodeView* viewOrNull = mNodes.value(GetEndpointNode(endpoint), nullptr);
    return viewOrNull == nullptr ? QString{}
                                 : FindType(isOutput == true ? viewOrNull->Ports.Outputs : viewOrNull->Ports.Inputs,
                                            GetEndpointPort(endpoint));
}

RenderGraphDomain RenderGraphCanvas::GetNodeDomain(const QString& node) const
{
    const NodeView* viewOrNull = mNodes.value(node, nullptr);
    return viewOrNull == nullptr ? RenderGraphDomain::Interface : viewOrNull->Domain;
}

void RenderGraphCanvas::SelectNode(const QString& node)
{
    SelectNodes(node.isEmpty() == true ? QStringList{} : QStringList{node});
}

void RenderGraphCanvas::SelectNodes(const QStringList& nodes)
{
    QStringList ordered = nodes;
    ordered.sort();
    if (ordered != GetSelectedNodes())
    {
        static_cast<void>(mReroutesOrNull->ClearSelection());
    }
    mHiddenSelection.clear();
    for (NodeView* view : std::as_const(mNodes))
    {
        const bool isSelected = view->IsMovable == true && nodes.contains(view->Id) == true;
        if (isSelected == true && view->Frame->isVisible() == false)
        {
            mHiddenSelection.append(view->Id);
        }
        view->Frame->setSelected(isSelected == true && view->Frame->isVisible() == true);
    }
    updateSelection();
}

void RenderGraphCanvas::notifySelection()
{
    for (EdgeView* edge : std::as_const(mEdges))
    {
        edge->Path->setSelected(false);
        edge->Path->setData(0, false);
        if (edge->IsEditable == true)
        {
            edge->Path->setPen(QPen(QColor::fromRgba(EDITABLE_EDGE_COLOR), 2));
        }
    }
    updateConnectionHighlight();
    Q_EMIT NodesSelected(GetSelectedNodes());
}

void RenderGraphCanvas::RevealNode(const QString& node)
{
    mCommentsOrNull->RevealNode(node);
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Id == node)
        {
            ensureVisible(view->Frame);
            return;
        }
    }
}

void RenderGraphCanvas::ClearDiagnostic()
{
    delete mDiagnosticMarkerOrNull;
    mDiagnosticMarkerOrNull = nullptr;
    mDiagnosticLabelOrNull = nullptr;
    mDiagnosticText.clear();
    mDiagnosticLocation = {};
    setAccessibleDescription({});
}

bool RenderGraphCanvas::ShowDiagnostic(const QJsonObject& location, const QString& description, bool isWarning)
{
    ClearDiagnostic();
    const bool isOutput = location.value(QStringLiteral("direction")).toString() == QStringLiteral("output");
    const QString port = location.value(QStringLiteral("port")).toString();
    QString owner = location.value(QStringLiteral("node")).toString();
    if (owner.isEmpty() == true && port.isEmpty() == false)
    {
        owner = isOutput == true ? OUTPUTS_NODE : INPUT_NODE;
    }
    NodeView* viewOrNull = mNodes.value(owner, nullptr);
    if (viewOrNull == nullptr)
    {
        return false;
    }
    mCommentsOrNull->RevealNode(owner);
    mDiagnosticLocation = location;
    setAccessibleDescription(description);
    mDiagnosticMarkerOrNull = new QGraphicsRectItem(viewOrNull->Frame->rect(), viewOrNull->Frame);
    mDiagnosticMarkerOrNull->setAcceptedMouseButtons(Qt::NoButton);
    const QColor diagnosticColour = QColor::fromRgba(isWarning == true ? WARNING_COLOR : LINK_REJECT_COLOR);
    QPen diagnosticPen(diagnosticColour, NODE_BORDER_WIDTH * 2);
    diagnosticPen.setCosmetic(true);
    mDiagnosticMarkerOrNull->setPen(diagnosticPen);
    mDiagnosticMarkerOrNull->setFlag(QGraphicsItem::ItemIgnoresParentOpacity);
    mDiagnosticMarkerOrNull->setToolTip(description.toHtmlEscaped());
    // Exposed outputs are consumers on the left side of the configuration output box.
    const bool isOutputConnector = owner == OUTPUTS_NODE ? false : isOutput;
    const QGraphicsEllipseItem* connectorOrNull =
        (isOutputConnector == true ? viewOrNull->OutputItems : viewOrNull->InputItems).value(port, nullptr);
    if (connectorOrNull != nullptr)
    {
        QGraphicsEllipseItem* ring = new QGraphicsEllipseItem(
            connectorOrNull->rect().adjusted(-NODE_PADDING, -NODE_PADDING, NODE_PADDING, NODE_PADDING),
            mDiagnosticMarkerOrNull);
        ring->setPos(connectorOrNull->mapToItem(mDiagnosticMarkerOrNull, QPointF()));
        ring->setAcceptedMouseButtons(Qt::NoButton);
        ring->setPen(diagnosticPen);
    }
    QGraphicsSimpleTextItem* label = new QGraphicsSimpleTextItem(mDiagnosticMarkerOrNull);
    mDiagnosticLabelOrNull = label;
    const QString target = port.isEmpty() == false ? port : location.value(QStringLiteral("field")).toString();
    mDiagnosticText = (isWarning == true ? QStringLiteral("Warning: %1") : QStringLiteral("Error: %1"))
                          .arg(target.isEmpty() == true ? owner : target);
    label->setText(
        QFontMetricsF(label->font()).elidedText(mDiagnosticText, Qt::ElideRight, NODE_WIDTH - NODE_PADDING * 2));
    label->setBrush(diagnosticColour);
    label->setPos(NODE_PADDING, viewOrNull->Frame->rect().bottom() + NODE_PADDING);
    label->setToolTip(description.toHtmlEscaped());
    updateSemanticZoom(true);
    ensureVisible(mDiagnosticMarkerOrNull->sceneBoundingRect().united(label->sceneBoundingRect()));
    return true;
}

QJsonObject RenderGraphCanvas::GetDiagnosticLocation() const
{
    return mDiagnosticLocation;
}

void RenderGraphCanvas::updateSelection()
{
    updateConnectionHighlight();
    updateFramingControls();
}

void RenderGraphCanvas::rebuildNavigation()
{
    QVector<RenderGraphNavigationNode> nodes;
    for (const NodeView* view : std::as_const(mNodes))
    {
        nodes.append({view->Id, view->Kind, view->IsMovable});
    }
    QVector<RenderGraphNavigationEdge> edges;
    for (const EdgeView* edge : std::as_const(mEdges))
    {
        edges.append({GetEndpointNode(edge->From), GetEndpointNode(edge->To)});
    }
    mNavigation->SetGraph(nodes, edges);
    mNavigation->SetSelection(GetSelectedNodes());
}

void RenderGraphCanvas::updateConnectionHighlight()
{
    const QStringList selected = GetSelectedNodes();
    mNavigation->SetSelection(selected);
    const bool hasTrace = mNavigation->GetTraceDirection() != RenderGraphTraceDirection::None;
    const RenderGraphDependencyTrace& trace = mNavigation->GetTrace();
    const bool hasHoveredPort = hasTrace == false && mHoveredPort.isEmpty() == false;
    const bool hasFocus = hasTrace == true || hasHoveredPort == true || selected.isEmpty() == false;
    QSet<QString> connected;
    QSet<QString> highlightedInputs;
    QSet<QString> highlightedOutputs;
    for (qsizetype index = 0; index < mEdges.size(); ++index)
    {
        EdgeView* edge = mEdges.at(index);
        const QString source = GetEndpointNode(edge->From);
        const QString destination = GetEndpointNode(edge->To);
        const bool isHighlighted = hasTrace == true ? trace.Edges.contains(index)
                                   : hasHoveredPort == true
                                       ? (mIsHoveredOutput == true ? edge->From : edge->To) == mHoveredPort
                                       : selected.contains(source) == true || selected.contains(destination) == true;
        const bool isSelected = edge->Path->data(0).toBool();
        if (isHighlighted == true || isSelected == true)
        {
            connected.insert(source);
            connected.insert(destination);
            highlightedOutputs.insert(edge->From);
            highlightedInputs.insert(edge->To);
        }
        const NodeView* destinationOrNull = mNodes.value(destination, nullptr);
        const float64 activityOpacity = destinationOrNull == nullptr ? 1 : destinationOrNull->ActivityOpacity;
        edge->Path->setOpacity(isHighlighted == true || isSelected == true ? 1
                               : hasFocus == true                          ? UNRELATED_EDGE_OPACITY
                                                                           : activityOpacity);
        QPen pen(isHighlighted == true || isSelected == true ? QColor::fromRgba(SELECTION_COLOR)
                                                             : QColor::fromRgba(EDITABLE_EDGE_COLOR),
                 isHighlighted == true || isSelected == true ? FOCUSED_EDGE_WIDTH : 2);
        pen.setCosmetic(true);
        if (edge->IsEditable == false)
        {
            pen.setStyle(Qt::DashLine);
        }
        edge->Path->setPen(pen);
    }
    for (NodeView* view : std::as_const(mNodes))
    {
        const bool isHighlighted = view->Frame->isSelected() == true || connected.contains(view->Id) == true;
        view->Frame->setOpacity(view->ActivityOpacity * (hasTrace == true && trace.Nodes.contains(view->Id) == false
                                                             ? UNRELATED_NODE_OPACITY
                                                             : 1));
        view->Frame->setPen(
            QPen(isHighlighted == true ? QColor::fromRgba(SELECTION_COLOR) : QColor::fromRgba(NODE_BORDER_COLOR),
                 isHighlighted == true ? FOCUSED_EDGE_WIDTH : NODE_BORDER_WIDTH));
        for (bool isOutput : {true, false})
        {
            const QHash<QString, QGraphicsEllipseItem*>& ports =
                isOutput == true ? view->OutputItems : view->InputItems;
            const QSet<QString>& highlighted = isOutput == true ? highlightedOutputs : highlightedInputs;
            for (const QString& port : ports.keys())
            {
                QGraphicsEllipseItem* connector = ports.value(port);
                if (connector == mLinkPortOrNull)
                {
                    continue;
                }
                const QString endpoint = view->Id + QLatin1Char('.') + port;
                const bool isPortHighlighted = highlighted.contains(endpoint) == true ||
                                               (mHoveredPort == endpoint && mIsHoveredOutput == isOutput);
                QPen pen(isPortHighlighted == true ? QColor::fromRgba(SELECTION_COLOR)
                                                   : QColor::fromRgba(NODE_BORDER_COLOR),
                         isPortHighlighted == true ? FOCUSED_EDGE_WIDTH : 1);
                pen.setCosmetic(true);
                connector->setPen(pen);
            }
        }
    }
    mCommentsOrNull->RefreshHighlights(
    {
        .Selection = selected,
        .TracedNodes = hasTrace == true ? trace.Nodes : QSet<QString>{},
        .Inputs = highlightedInputs,
        .Outputs = highlightedOutputs,
    });
    mReroutesOrNull->RefreshStyles();
}

QStringList RenderGraphCanvas::GetSelectedNodes() const
{
    QStringList selected;
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->IsMovable == true &&
            (view->Frame->isSelected() == true || mHiddenSelection.contains(view->Id) == true))
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

void RenderGraphCanvas::AddLayoutMenu(QMenu& inoutMenu)
{
    QMenu* layout = inoutMenu.addMenu(QStringLiteral("Layout"));
    layout->setObjectName(QStringLiteral("renderGraphLayoutMenu"));
    const qsizetype selectedCount = GetSelectedNodes().size();
    qsizetype authoredCount = 0;
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->IsMovable == true)
        {
            ++authoredCount;
        }
    }
    const bool canEdit =
        mFile.isEmpty() == false && mIsDragging == false && mIsBoxSelecting == false && mLinkFrom.isEmpty() == true;
    const auto addAction = [this, layout, canEdit](const QString& label,
                                                   const char* actionName,
                                                   RenderGraphLayoutOperation operation,
                                                   bool hasEnoughNodes,
                                                   bool shouldArrangeAll = false) {
        QAction* action = layout->addAction(label);
        action->setObjectName(QString::fromLatin1(actionName));
        action->setEnabled(canEdit == true && hasEnoughNodes == true);
        connect(action, &QAction::triggered, this, [this, operation, shouldArrangeAll]() {
            static_cast<void>(ApplyLayout(operation, shouldArrangeAll));
        });
    };
    addAction(QStringLiteral("Auto-arrange Selection"),
              "renderGraphArrangeSelection",
              RenderGraphLayoutOperation::Arrange,
              selectedCount > 0);
    addAction(QStringLiteral("Auto-arrange All"),
              "renderGraphArrangeAll",
              RenderGraphLayoutOperation::Arrange,
              authoredCount > 0,
              true);
    layout->addSeparator();
    addAction(QStringLiteral("Align Left"),
              "renderGraphAlignLeft",
              RenderGraphLayoutOperation::AlignLeft,
              selectedCount >= 2);
    addAction(QStringLiteral("Align Horizontal Centres"),
              "renderGraphAlignHorizontalCentre",
              RenderGraphLayoutOperation::AlignHorizontalCentre,
              selectedCount >= 2);
    addAction(QStringLiteral("Align Right"),
              "renderGraphAlignRight",
              RenderGraphLayoutOperation::AlignRight,
              selectedCount >= 2);
    addAction(QStringLiteral("Align Top"),
              "renderGraphAlignTop",
              RenderGraphLayoutOperation::AlignTop,
              selectedCount >= 2);
    addAction(QStringLiteral("Align Vertical Centres"),
              "renderGraphAlignVerticalCentre",
              RenderGraphLayoutOperation::AlignVerticalCentre,
              selectedCount >= 2);
    addAction(QStringLiteral("Align Bottom"),
              "renderGraphAlignBottom",
              RenderGraphLayoutOperation::AlignBottom,
              selectedCount >= 2);
    layout->addSeparator();
    addAction(QStringLiteral("Distribute Horizontally"),
              "renderGraphDistributeHorizontal",
              RenderGraphLayoutOperation::DistributeHorizontal,
              selectedCount >= MINIMUM_RENDER_GRAPH_DISTRIBUTION_COUNT);
    addAction(QStringLiteral("Distribute Vertically"),
              "renderGraphDistributeVertical",
              RenderGraphLayoutOperation::DistributeVertical,
              selectedCount >= MINIMUM_RENDER_GRAPH_DISTRIBUTION_COUNT);
}

bool RenderGraphCanvas::ApplyLayout(RenderGraphLayoutOperation operation, bool shouldArrangeAll)
{
    if (mIsDragging == true || mLinkFrom.isEmpty() == false || mIsBoxSelecting == true ||
        (shouldArrangeAll == true && operation != RenderGraphLayoutOperation::Arrange))
    {
        return false;
    }
    const QStringList selected = GetSelectedNodes();
    QVector<RenderGraphLayoutNode> nodes;
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->IsMovable == true && (shouldArrangeAll == true || selected.contains(view->Id) == true))
        {
            nodes.append({view->Id, QRectF(view->Frame->pos(), view->Frame->rect().size())});
        }
    }
    QVector<RenderGraphLayoutEdge> edges;
    for (const EdgeView* edge : std::as_const(mEdges))
    {
        edges.append({GetEndpointNode(edge->From), GetEndpointNode(edge->To)});
    }
    QVector<RenderGraphNodePosition> positions;
    if (ComputeRenderGraphLayout(nodes, edges, operation, positions) == false)
    {
        Q_EMIT LayoutRejected(
            QStringLiteral("Select enough authored nodes with valid card bounds; the resulting layout "
                           "must fit within the saved coordinate limits."));
        return false;
    }
    const QJsonArray saved =
        configuration().value(QStringLiteral("layout")).toObject().value(QStringLiteral("positions")).toArray();
    QVector<RenderGraphNodePosition> changed;
    for (const RenderGraphNodePosition& position : std::as_const(positions))
    {
        bool isSaved = false;
        for (const QJsonValueConstRef value : saved)
        {
            const QJsonObject record = value.toObject();
            if (record.value(QStringLiteral("node")).toString() == position.Node &&
                record.value(QStringLiteral("x")).toInt() == position.X &&
                record.value(QStringLiteral("y")).toInt() == position.Y)
            {
                isSaved = true;
                break;
            }
        }
        if (isSaved == false)
        {
            changed.append(position);
        }
    }
    if (changed.isEmpty() == false)
    {
        Q_EMIT MoveRequested(changed);
    }
    if (operation == RenderGraphLayoutOperation::Arrange)
    {
        QRectF bounds;
        for (const RenderGraphLayoutNode& node : std::as_const(nodes))
        {
            const NodeView* viewOrNull = mNodes.value(node.Identity, nullptr);
            if (viewOrNull != nullptr)
            {
                bounds = bounds.united(QRectF(viewOrNull->Frame->pos(), viewOrNull->Frame->rect().size()));
            }
        }
        fitBounds(bounds);
    }
    return true;
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

bool RenderGraphCanvas::RequestLink(const QString& from, const QString& to, bool shouldReplaceExisting)
{
    const RenderGraphLinkCheck check =
        CheckRenderGraphLink(mDocument, mFile, mConfiguration, from, to, shouldReplaceExisting);
    if (check.IsLegal() == false)
    {
        Q_EMIT LinkRejected(QStringLiteral("%1: %2").arg(check.Code, check.Reason));
        return false;
    }
    if (shouldReplaceExisting == true)
    {
        Q_EMIT ReplaceConnectionRequested(from, to);
    }
    else
    {
        Q_EMIT ConnectRequested(from, to);
    }
    return true;
}

bool RenderGraphCanvas::ActivateNode(const QString& node)
{
    if (node == INPUT_NODE || node == OUTPUTS_NODE)
    {
        Q_EMIT InterfaceEditRequested();
        return true;
    }
    const QJsonObject record = FindNode(configuration(), node);
    if (record.isEmpty() == true)
    {
        return false;
    }
    if (record.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph"))
    {
        Q_EMIT NodeDetailsRequested(node);
        return true;
    }
    Q_EMIT SubgraphActivated(record.value(QStringLiteral("asset")).toString(),
                             record.value(QStringLiteral("configuration")).toString(),
                             node);
    return true;
}

QPointF RenderGraphCanvas::GetNewNodePosition(const QJsonObject& node,
                                              const QString& from,
                                              const QString& input,
                                              QPointF inputPosition) const
{
    const QString identity = node.value(QStringLiteral("id")).toString();
    QJsonObject record = configuration();
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    nodes.append(node);
    record.insert(QStringLiteral("nodes"), nodes);
    QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    edges.append(
        QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), identity + QLatin1Char('.') + input}});
    record.insert(QStringLiteral("edges"), edges);
    RenderGraphNodePorts ports;
    if (GetRenderGraphNodePorts(mDocument, node, ports) == false)
    {
        return inputPosition;
    }
    if (IsSceneBundleExpanded(identity) == false)
    {
        HideUnusedSceneMembers(record, node, ports);
    }
    for (qsizetype row = 0; row < ports.Inputs.size(); ++row)
    {
        if (ports.Inputs.at(row).Id == input)
        {
            // Matches addPort in Rebuild: inputs sit on the left edge, centred in their row.
            return inputPosition - QPointF(0, HEADER_HEIGHT + (static_cast<float64>(row) + 0.5) * PORT_SPACING);
        }
    }
    return inputPosition;
}

QString RenderGraphCanvas::nodeAt(QPointF position) const
{
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Frame->isVisible() == true && view->Frame->sceneBoundingRect().contains(position) == true)
        {
            return view->Id;
        }
    }
    return {};
}

QString RenderGraphCanvas::portAt(QPointF position, bool& outIsOutput) const
{
    QString boundary = mCommentsOrNull->GetPortAt(position, outIsOutput);
    if (boundary.isEmpty() == false)
    {
        return boundary;
    }
    for (const NodeView* view : std::as_const(mNodes))
    {
        if (view->Frame->isVisible() == false)
        {
            continue;
        }
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
        if (edge->Path->isVisible() == true && edge->IsEditable == true &&
            stroker.createStroke(edge->Path->path()).contains(position) == true)
        {
            outTo = edge->To;
            return edge->From;
        }
    }
    return {};
}

void RenderGraphCanvas::cancelLink()
{
    if (mLinkPortOrNull != nullptr)
    {
        mLinkPortOrNull->setPen(QPen(QColor::fromRgba(NODE_BORDER_COLOR), 1));
        mLinkPortOrNull->setToolTip(mLinkPortTooltip);
        mLinkPortOrNull = nullptr;
    }
    if (mLinkHintOrNull != nullptr)
    {
        delete mLinkHintOrNull;
        mLinkHintOrNull = nullptr;
    }
    mLinkFrom.clear();
    mLinkEndpoint.clear();
    if (mLinkPreviewOrNull != nullptr)
    {
        mScene->removeItem(mLinkPreviewOrNull);
        delete mLinkPreviewOrNull;
        mLinkPreviewOrNull = nullptr;
    }
}

void RenderGraphCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    const QPointF position = mapToScene(event->pos());
    bool isOutput = false;
    const QString port = portAt(position, isOutput);
    const QString node = nodeAt(position);
    if (node.isEmpty() == true && port.isEmpty() == true &&
        mReroutesOrNull->ShowContextMenu(position, event->globalPos()) == true)
    {
        event->accept();
        return;
    }
    if (node.isEmpty() == true && port.isEmpty() == true &&
        mCommentsOrNull->ShowContextMenu(position, event->globalPos()) == true)
    {
        event->accept();
        return;
    }
    QString to;
    const QString from = node.isEmpty() == true ? edgeAt(position, to) : QString();
    if (port.isEmpty() == false || from.isEmpty() == false)
    {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("renderGraphConnectionMenu"));
        const QString inspectedEndpoint = port.isEmpty() == false ? port : from;
        const bool isInspectedOutput = port.isEmpty() == false ? isOutput : true;
        const QString inspectedFile = mFile;
        const QString inspectedConfiguration = mConfiguration;
        const uint64 inspectedRevision = GetDocumentRevision();
        const QVector<RenderGraphHistoryLocation> inspectedPath = mHistoryOrNull->GetPath();
        QAction* inspect = menu.addAction(QStringLiteral("Inspect Resource"));
        inspect->setObjectName(QStringLiteral("renderGraphInspectResource"));
        connect(inspect,
                &QAction::triggered,
                this,
                [this,
                 inspectedEndpoint,
                 isInspectedOutput,
                 inspectedFile,
                 inspectedConfiguration,
                 inspectedRevision,
                 inspectedPath]() {
                    if (mFile == inspectedFile && mConfiguration == inspectedConfiguration &&
                        GetDocumentRevision() == inspectedRevision &&
                        AreRenderGraphResourcePathsEqual(inspectedPath, mHistoryOrNull->GetPath()) == true)
                    {
                        Q_EMIT ResourceInspectionRequested(inspectedEndpoint, isInspectedOutput);
                    }
                });
        menu.addSeparator();
        if (port.isEmpty() == true && from.isEmpty() == false)
        {
            const int32 segment = findRerouteSegment(from, to, position);
            const uint64 revision = GetDocumentRevision();
            const QString file = mFile;
            const QString configuration = mConfiguration;
            QAction* add = menu.addAction(QStringLiteral("Add Reroute Point"));
            add->setObjectName(QStringLiteral("renderGraphAddReroute"));
            add->setEnabled(IsWireCollapsed(from, to) == false);
            connect(add,
                    &QAction::triggered,
                    this,
                    [this, from, to, segment, position, revision, file, configuration]() {
                        if (revision == GetDocumentRevision() && file == mFile && configuration == mConfiguration)
                        {
                            mReroutesOrNull->AddPoint(from, to, segment, position);
                        }
                    });
            menu.addSeparator();
        }
        // Copy endpoints before executing: an edit can rebuild and delete every EdgeView.
        for (const EdgeView* edge : std::as_const(mEdges))
        {
            if (edge->IsEditable == true &&
                ((port.isEmpty() == false && (isOutput == true ? edge->From == port : edge->To == port)) ||
                 (from.isEmpty() == false && edge->From == from && edge->To == to)))
            {
                const QString source = edge->From;
                const QString destination = edge->To;
                QAction* disconnect = menu.addAction(QStringLiteral("Disconnect %1 → %2").arg(source, destination));
                disconnect->setObjectName(QStringLiteral("renderGraphDisconnectAction"));
                connect(disconnect, &QAction::triggered, this, [this, source, destination]() {
                    Q_EMIT DisconnectRequested(source, destination);
                });
            }
        }
        if (port.isEmpty() == false)
        {
            QMenu* connections =
                menu.addMenu(isOutput == true ? QStringLiteral("Connect to") : QStringLiteral("Connect from"));
            connections->setObjectName(QStringLiteral("renderGraphConnectPortMenu"));
            for (const NodeView* view : std::as_const(mNodes))
            {
                const QVector<RenderGraphPort>& candidates =
                    isOutput == true ? view->Ports.Inputs : view->Ports.Outputs;
                for (const RenderGraphPort& candidate : candidates)
                {
                    const QString endpoint = view->Id + QLatin1Char('.') + candidate.Id;
                    const QString source = isOutput == true ? port : endpoint;
                    const QString destination = isOutput == true ? endpoint : port;
                    if (CheckRenderGraphLink(mDocument, mFile, mConfiguration, source, destination, true).IsLegal() ==
                        true)
                    {
                        QAction* action = connections->addAction(endpoint);
                        action->setData(endpoint);
                        connect(action, &QAction::triggered, this, [this, source, destination]() {
                            static_cast<void>(RequestLink(source, destination, true));
                        });
                    }
                }
            }
            connections->setEnabled(connections->isEmpty() == false);
        }
        if (menu.isEmpty() == true)
        {
            QAction* empty = menu.addAction(QStringLiteral("No connections"));
            empty->setEnabled(false);
        }
        menu.exec(event->globalPos());
        event->accept();
        return;
    }
    const bool isAuthoredNode = FindNode(configuration(), node).isEmpty() == false;
    if (isAuthoredNode == true && GetSelectedNodes().contains(node) == false)
    {
        SelectNode(node);
        notifySelection();
    }
    Q_EMIT ContextMenuRequested(isAuthoredNode == true ? node : QString(), event->globalPos());
    event->accept();
}

void RenderGraphCanvas::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && mIsSpacePressed == true))
    {
        mIsPanning = true;
        mPanPosition = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton)
    {
        event->accept();
        return;
    }
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
        static_cast<void>(mReroutesOrNull->ClearSelection());
        mLinkFrom = port;
        mLinkPreviewOrNull =
            mScene->addPath(MakeCurve(position, position), QPen(QColor::fromRgba(SELECTION_COLOR), 2, Qt::DashLine));
        mLinkHintOrNull = new QGraphicsTextItem;
        mScene->addItem(mLinkHintOrNull);
        mLinkHintOrNull->setData(0, QStringLiteral("#link-feedback"));
        mLinkHintOrNull->setTextWidth(LINK_HINT_WIDTH);
        mLinkHintOrNull->setZValue(2);
        mLinkHintOrNull->setAcceptedMouseButtons(Qt::NoButton);
        updateLinkFeedback(position);
        event->accept();
        return;
    }
    const QString node = nodeAt(position);
    if (node.isEmpty() == false)
    {
        static_cast<void>(mReroutesOrNull->ClearSelection());
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
        notifySelection();
        if (GetSelectedNodes().contains(node) == true)
        {
            Q_EMIT NodeSelected(node);
        }
        BeginDrag(GetSelectedNodes());
        event->accept();
        return;
    }
    if (port.isEmpty() == true && mReroutesOrNull->BeginGesture(position) == true)
    {
        event->accept();
        return;
    }
    static_cast<void>(mReroutesOrNull->ClearSelection());
    if (mCommentsOrNull->BeginGesture(position) == true)
    {
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
    updateConnectionHighlight();
    if (from.isEmpty() == true)
    {
        mSelectionBeforeBox = event->modifiers().testFlag(Qt::ShiftModifier) == true ||
                                      event->modifiers().testFlag(Qt::ControlModifier) == true
                                  ? GetSelectedNodes()
                                  : QStringList{};
        mSelectionStart = event->position().toPoint();
        mIsBoxSelecting = true;
        SelectNodes(mSelectionBeforeBox);
        mSelectionBox->setGeometry(QRect(mSelectionStart, QSize()));
        mSelectionBox->show();
        notifySelection();
    }
    event->accept();
}

void RenderGraphCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (mIsPanning == true)
    {
        const QPoint delta = event->position().toPoint() - mPanPosition;
        mPanPosition = event->position().toPoint();
        panBy(delta);
        event->accept();
        return;
    }
    if (mReroutesOrNull->UpdateGesture(mapToScene(event->position().toPoint())) == true)
    {
        event->accept();
        return;
    }
    if (mCommentsOrNull->UpdateGesture(mapToScene(event->position().toPoint())) == true)
    {
        event->accept();
        return;
    }
    if (mIsBoxSelecting == true)
    {
        const QRect rectangle = QRect(mSelectionStart, event->position().toPoint()).normalized();
        mSelectionBox->setGeometry(rectangle);
        const QRectF sceneRectangle = mapToScene(rectangle).boundingRect();
        QStringList selected = mSelectionBeforeBox;
        for (const NodeView* view : std::as_const(mNodes))
        {
            if (view->IsMovable == true && view->Frame->isVisible() == true &&
                sceneRectangle.intersects(view->Frame->sceneBoundingRect()) == true &&
                selected.contains(view->Id) == false)
            {
                selected.append(view->Id);
            }
        }
        SelectNodes(selected);
        event->accept();
        return;
    }
    const QPointF position = mapToScene(event->position().toPoint());
    if (mLinkPreviewOrNull != nullptr)
    {
        mLinkPreviewOrNull->setPath(MakeCurve(GetPortPosition(mLinkFrom, true), position));
        updateLinkFeedback(position);
        event->accept();
        return;
    }
    if (mIsDragging == true)
    {
        UpdateDrag(position - mPressPosition);
        event->accept();
        return;
    }
    bool isOutput = false;
    const QString port = portAt(position, isOutput);
    if (port != mHoveredPort || isOutput != mIsHoveredOutput)
    {
        mHoveredPort = port;
        mIsHoveredOutput = isOutput;
        updateConnectionHighlight();
    }
    bool isResourceOutput = isOutput;
    QString resourceEndpoint = port;
    if (resourceEndpoint.isEmpty() == true && nodeAt(position).isEmpty() == true)
    {
        QString destination;
        resourceEndpoint = edgeAt(position, destination);
        isResourceOutput = true;
    }
    if (resourceEndpoint.isEmpty() == false)
    {
        Q_EMIT ResourceHovered(resourceEndpoint, isResourceOutput);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void RenderGraphCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (mIsPanning == true && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton))
    {
        mIsPanning = false;
        setCursor(mIsSpacePressed == true ? Qt::OpenHandCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && mReroutesOrNull->EndGesture() == true)
    {
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && mCommentsOrNull->EndGesture() == true)
    {
        event->accept();
        return;
    }
    if (mIsBoxSelecting == true && event->button() == Qt::LeftButton)
    {
        mouseMoveEvent(event);
        mIsBoxSelecting = false;
        mSelectionBox->hide();
        notifySelection();
        event->accept();
        return;
    }
    if (mLinkPreviewOrNull != nullptr)
    {
        const QString from = mLinkFrom;
        cancelLink();
        const QPoint point = event->position().toPoint();
        const QPointF position = mapToScene(point);
        bool isOutput = false;
        const QString to = portAt(position, isOutput);
        const bool isClick =
            (point - mapFromScene(mPressPosition)).manhattanLength() < QApplication::startDragDistance();
        if (isClick == true)
        {
            // A plain click on an output offers nodes that accept it, placed a column gap to the right.
            Q_EMIT ConnectQuickAddRequested(from, GetPortPosition(from, true) + QPointF(QUICK_ADD_PORT_GAP, 0));
        }
        else if (to.isEmpty() == false)
        {
            static_cast<void>(RequestLink(from, to, true));
        }
        else if (nodeAt(position).isEmpty() == true && itemAt(point) == nullptr)
        {
            // Released on empty canvas: offer nodes that accept the wire, with their input at the drop point.
            Q_EMIT ConnectQuickAddRequested(from, position);
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
    const QPointF position = mapToScene(event->position().toPoint());
    bool isOutput = false;
    if (node.isEmpty() == true && portAt(position, isOutput).isEmpty() == true)
    {
        if (mReroutesOrNull->HasPointAt(position) == true)
        {
            event->accept();
            return;
        }
        QString to;
        const QString from = edgeAt(position, to);
        if (from.isEmpty() == false)
        {
            mReroutesOrNull->AddPoint(from, to, findRerouteSegment(from, to, position), position);
            event->accept();
            return;
        }
    }
    if (node.isEmpty() == true && mCommentsOrNull->ActivateAt(mapToScene(event->position().toPoint())) == true)
    {
        event->accept();
        return;
    }
    // Empty canvas: no node, port, wire or interface box under the pointer.
    if (node.isEmpty() == true && itemAt(event->position().toPoint()) == nullptr)
    {
        Q_EMIT QuickAddRequested(mapToScene(event->position().toPoint()));
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void RenderGraphCanvas::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space)
    {
        mIsSpacePressed = true;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_A && event->modifiers() == Qt::ShiftModifier)
    {
        const QPoint pointer = viewport()->mapFromGlobal(QCursor::pos());
        Q_EMIT QuickAddRequested(
            mapToScene(viewport()->rect().contains(pointer) == true ? pointer : viewport()->rect().center()));
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Copy) == true || event->matches(QKeySequence::Paste) == true)
    {
        if (event->matches(QKeySequence::Copy) == true)
        {
            Q_EMIT CopyRequested();
        }
        else
        {
            Q_EMIT PasteRequested();
        }
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll) == true)
    {
        static_cast<void>(mReroutesOrNull->ClearSelection());
        SelectNodes(mNodes.keys());
        notifySelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape)
    {
        if (mReroutesOrNull->CancelGesture() == true || mReroutesOrNull->ClearSelection() == true)
        {
            event->accept();
            return;
        }
        if (mCommentsOrNull->CancelGesture() == true)
        {
            event->accept();
            return;
        }
        if (mNavigation->GetTraceDirection() != RenderGraphTraceDirection::None && mLinkPreviewOrNull == nullptr &&
            mIsDragging == false && mIsBoxSelecting == false && mIsPanning == false)
        {
            mNavigation->SetTraceDirection(RenderGraphTraceDirection::None);
            event->accept();
            return;
        }
        cancelLink();
        mIsBoxSelecting = false;
        mSelectionBox->hide();
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
        notifySelection();
        event->accept();
        return;
    }
    if (event->key() != Qt::Key_Delete && event->key() != Qt::Key_Backspace)
    {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    if (mReroutesOrNull->DeleteSelectedPoint() == true)
    {
        event->accept();
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
    const QStringList selected = GetSelectedNodes();
    if (selected.size() == 1)
    {
        Q_EMIT RemoveRequested(selected.first());
    }
    else if (selected.isEmpty() == false)
    {
        Q_EMIT RemoveNodesRequested(selected);
    }
    event->accept();
}

void RenderGraphCanvas::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && event->isAutoRepeat() == false)
    {
        mIsSpacePressed = false;
        if (mIsPanning == false)
        {
            unsetCursor();
        }
        event->accept();
        return;
    }
    QGraphicsView::keyReleaseEvent(event);
}

void RenderGraphCanvas::focusOutEvent(QFocusEvent* event)
{
    static_cast<void>(mReroutesOrNull->CancelGesture());
    static_cast<void>(mCommentsOrNull->CancelGesture());
    mIsSpacePressed = false;
    mIsPanning = false;
    mIsBoxSelecting = false;
    mSelectionBox->hide();
    unsetCursor();
    QGraphicsView::focusOutEvent(event);
}

void RenderGraphCanvas::wheelEvent(QWheelEvent* event)
{
    // A touchpad has no middle button, so its two-finger scroll pans like middle-drag.
    // Scroll phases identify macOS/Wayland scroll surfaces; a plain mouse wheel has none.
    // Ctrl (Command on macOS) zooms from any device, which is also how Windows precision
    // touchpads commonly deliver pinch.
    const QPointingDevice* deviceOrNull = event->pointingDevice();
    const bool isScrollSurface =
        event->phase() != Qt::NoScrollPhase ||
        (deviceOrNull != nullptr && deviceOrNull->type() == QInputDevice::DeviceType::TouchPad);
    if (isScrollSurface == true && event->modifiers().testFlag(Qt::ControlModifier) == false)
    {
        const QPoint delta =
            event->pixelDelta().isNull() == false
                ? event->pixelDelta()
                : (QPointF(event->angleDelta()) * (PAN_PIXELS_PER_WHEEL_NOTCH / WHEEL_NOTCH)).toPoint();
        panBy(delta);
        event->accept();
        return;
    }
    const float64 steps = event->pixelDelta().isNull() == false ? event->pixelDelta().y() / SCROLL_PIXELS_PER_ZOOM_STEP
                                                                : event->angleDelta().y() / WHEEL_NOTCH;
    zoomAt(math::Pow(ZOOM_STEP, steps), event->position());
    event->accept();
}

bool RenderGraphCanvas::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::Leave)
    {
        mHoveredPort.clear();
        updateConnectionHighlight();
    }
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
        if (gesture->gestureType() == Qt::SmartZoomNativeGesture)
        {
            // macOS two-finger double-tap: recover from a lost view by framing the graph.
            FitGraph();
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

void RenderGraphCanvas::panBy(QPoint delta)
{
    // Content follows the pointer or fingers: a positive delta reveals the left/top.
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
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
    updateSemanticZoom();
}
} // namespace ludus::editor
