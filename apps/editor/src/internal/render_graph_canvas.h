#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_document.h"

#include <QGraphicsView>
#include <QHash>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QGraphicsPathItem;
class QGraphicsScene;
class QShowEvent;
QT_END_NAMESPACE

namespace ludus::editor
{
/// One typed node port: Scene, View, ColorTexture or DepthTexture.
struct RenderGraphPort final
{
    QString Id;
    QString Type;
};

/// Inputs (consumed) and outputs (produced) of one authored node or configuration interface.
struct RenderGraphNodePorts final
{
    QVector<RenderGraphPort> Inputs;
    QVector<RenderGraphPort> Outputs;
};

/// Connection domain shown on the canvas (docs/architecture/render-graph-authoring.md,
/// "Node and connection contracts"). Shader functions live in build manifests, not here.
enum class RenderGraphDomain : foundation::uint8
{
    SceneView,
    Execution,
    Resource,
    Subgraph,
    Interface,
};

/// Result of the canvas pre-check of one proposed edge. Code uses the shared validator's
/// RenderGraph error names (Port, MultipleWriters, TypeMismatch, Cycle) without the prefix.
struct RenderGraphLinkCheck final
{
    QString Code;
    QString Reason;
    [[nodiscard]] bool IsLegal() const noexcept
    {
        return Code.isEmpty();
    }
};

/// Domain of a node kind; unknown kinds are Execution so they remain visible.
[[nodiscard]] RenderGraphDomain GetRenderGraphDomain(const QString& kind);

/// Ports of one node, mirroring ludus_tools.render_graph `node_ports`. A Subgraph's ports
/// are its referenced configuration's interface, resolved from the package draft.
/// @return False when a Subgraph's asset or configuration is not in the draft.
[[nodiscard]] bool
GetRenderGraphNodePorts(const RenderGraphDocument& document, const QJsonObject& node, RenderGraphNodePorts& outPorts);

/// Pre-check a `node.port` -> `node.port` edge against the draft configuration before it is
/// committed: direction, known ports, type, a single writer per input and within-frame cycles.
/// The shared CLI validator still owns legality; this only gives the canvas immediate reasons.
[[nodiscard]] RenderGraphLinkCheck CheckRenderGraphLink(const RenderGraphDocument& document,
                                                        const QString& file,
                                                        const QString& configuration,
                                                        const QString& from,
                                                        const QString& to);

// RG3b visual node canvas over one configuration of a RenderGraphDocument draft. The canvas
// owns no graph state: it renders the draft and emits typed edit requests that the Renderer
// workspace applies through the same document operations as the inspector, so undo, save and
// validation are shared. A drag gesture reports all moved nodes once, on release, so it is one
// undo step. Nodes without an authored layout position are placed by dependency order only for
// display. Illegal links are rejected before any request with the validator's reason codes.
// GUI thread only. See docs/development/render-graph-authoring.md#node-canvas-and-preview-rg3b.
class RenderGraphCanvas final : public QGraphicsView
{
    Q_OBJECT
public:
    static constexpr foundation::int32 MAX_COORDINATE = 32768;

    explicit RenderGraphCanvas(const RenderGraphDocument& document, QWidget* parentOrNull = nullptr);
    ~RenderGraphCanvas() override;
    RenderGraphCanvas(const RenderGraphCanvas&) = delete;
    RenderGraphCanvas& operator=(const RenderGraphCanvas&) = delete;

    /// Show a configuration of a draft graph file; an empty file clears the canvas.
    void ShowConfiguration(const QString& file, const QString& configuration);
    /// Re-read the draft after any document change, keeping the view and selection.
    void Rebuild();
    [[nodiscard]] const QString& GetFile() const noexcept
    {
        return mFile;
    }
    [[nodiscard]] const QString& GetConfiguration() const noexcept
    {
        return mConfiguration;
    }
    [[nodiscard]] foundation::int32 GetNodeCount() const;
    [[nodiscard]] foundation::int32 GetEdgeCount() const;
    [[nodiscard]] bool HasNode(const QString& node) const;
    /// Displayed scene position of a node's top-left corner.
    [[nodiscard]] QPointF GetNodePosition(const QString& node) const;
    /// Scene position of a port connector; outputs are on the right edge.
    [[nodiscard]] QPointF GetPortPosition(const QString& endpoint, bool isOutput) const;
    /// Domain colour category of a displayed node (for tests and accessibility text).
    [[nodiscard]] RenderGraphDomain GetNodeDomain(const QString& node) const;
    void SelectNode(const QString& node);
    /// Scroll the node into view without changing the current zoom or selection.
    void RevealNode(const QString& node);
    [[nodiscard]] QStringList GetSelectedNodes() const;

    /// Gesture entry points shared by mouse handling and tests. A gesture moves every
    /// selected node live in the view and reports the final positions once in EndDrag.
    void BeginDrag(const QStringList& nodes);
    void UpdateDrag(QPointF delta);
    void EndDrag();
    /// Request an edge from an output endpoint to an input endpoint; rejected links emit LinkRejected.
    [[nodiscard]] bool RequestLink(const QString& from, const QString& to);
    /// Activate a node as a double-click does: open a Subgraph or request a Raster's shader sources.
    [[nodiscard]] bool ActivateNode(const QString& node);

Q_SIGNALS:
    /// Open the displayed configuration interface editor (double-click an interface box).
    void InterfaceEditRequested();
    void NodeSelected(const QString& node);
    /// Context target is empty on the background or configuration interface.
    void ContextMenuRequested(const QString& node, const QPoint& globalPosition);
    void MoveRequested(const QVector<ludus::editor::RenderGraphNodePosition>& positions);
    void ConnectRequested(const QString& from, const QString& to);
    void DisconnectRequested(const QString& from, const QString& to);
    void RemoveRequested(const QString& node);
    void ShaderSourcesRequested(const QString& node);
    void SubgraphActivated(const QString& asset, const QString& configuration);
    void LinkRejected(const QString& reason);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct NodeView;
    struct EdgeView;
    [[nodiscard]] QJsonObject configuration() const;
    void updateEdges();
    void updateSelection();
    [[nodiscard]] QString nodeAt(QPointF position) const;
    [[nodiscard]] QString portAt(QPointF position, bool& outIsOutput) const;
    [[nodiscard]] QString edgeAt(QPointF position, QString& outTo) const;
    void cancelLink();
    void fitGraph();
    void zoomAt(foundation::float64 factor, QPointF position);

    const RenderGraphDocument& mDocument;
    QGraphicsScene* mScene;
    QString mFile;
    QString mConfiguration;
    QHash<QString, NodeView*> mNodes;
    QVector<EdgeView*> mEdges;
    QStringList mDragNodes;
    QHash<QString, QPointF> mDragOrigins;
    QPointF mDragOffset;
    QPointF mPressPosition;
    QString mLinkFrom;
    QGraphicsPathItem* mLinkPreviewOrNull = nullptr;
    bool mIsDragging = false;
    bool mShouldFitOnShow = false;
};
} // namespace ludus::editor
