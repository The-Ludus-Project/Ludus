#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_document.h"
#include "internal/render_graph_node_ports.h"

#include <QGraphicsView>
#include <QHash>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QGraphicsPathItem;
class QGraphicsRectItem;
class QGraphicsEllipseItem;
class QGraphicsSimpleTextItem;
class QGraphicsTextItem;
class QGraphicsScene;
class QShowEvent;
class QRubberBand;
class QFocusEvent;
class QResizeEvent;
class QPaintEvent;
class QAction;
class QLabel;
class QToolBar;
QT_END_NAMESPACE

namespace ludus::editor
{
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
/// RenderGraph error names (Port, MultipleWriters, TypeMismatch, AttachmentMismatch, Cycle) without the prefix.
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

/// True for kinds whose authored record may set `"enabled": false` (Accumulate, Denoise, ToneMap, Export).
/// Mirrors ludus_tools.render_graph `DISABLE_KINDS`; the shared validator still owns legality.
[[nodiscard]] bool CanDisableRenderGraphNode(const QString& kind);

/// False only when a node that can be disabled is authored with `"enabled": false`. The compiler
/// then forwards a pass-through node's color input to its consumers, and treats a disabled Export as
/// no root, so its input needs no connection. The node, its wires and settings stay in the draft.
[[nodiscard]] bool IsRenderGraphNodeEnabled(const QJsonObject& node);

/// Pre-check a `node.port` -> `node.port` edge against the draft configuration before it is
/// committed: direction, known ports, type, a single writer per input, within-frame cycles
/// and newly provable texture-contract mismatches through subgraph bindings.
/// The shared CLI validator still owns legality; this only gives the canvas immediate reasons.
/// shouldReplaceExisting allows an occupied destination; all other checks still apply.
[[nodiscard]] RenderGraphLinkCheck CheckRenderGraphLink(const RenderGraphDocument& document,
                                                        const QString& file,
                                                        const QString& configuration,
                                                        const QString& from,
                                                        const QString& to,
                                                        bool shouldReplaceExisting = false);

/// Pre-check a `from` -> `to` edge to an input of node, a complete record that is not in the draft
/// yet, exactly as CheckRenderGraphLink would once node has been added. Rejects an identity that is
/// empty or already used, and a `to` that is not on node. Used to offer and add a node together
/// with its connection (RenderGraphDocument::AddNode). Does not modify the draft.
[[nodiscard]] RenderGraphLinkCheck CheckRenderGraphLinkToNewNode(const RenderGraphDocument& document,
                                                                 const QString& file,
                                                                 const QString& configuration,
                                                                 const QJsonObject& node,
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
    static constexpr foundation::int32 MAX_COORDINATE = RenderGraphDocument::MAX_COORDINATE;

    /// Expand available Scene members; connected members remain visible when collapsed.
    void SetSceneBundleExpanded(const QString& node, bool isExpanded);
    /// Current per-canvas expansion preference for this configuration and node.
    [[nodiscard]] bool IsSceneBundleExpanded(const QString& node) const;
    explicit RenderGraphCanvas(const RenderGraphDocument& document, QWidget* parentOrNull = nullptr);
    ~RenderGraphCanvas() override;
    RenderGraphCanvas(const RenderGraphCanvas&) = delete;
    RenderGraphCanvas& operator=(const RenderGraphCanvas&) = delete;

    /// Show a configuration of a draft graph file; an empty file clears the canvas.
    void ShowConfiguration(const QString& file, const QString& configuration);
    /// Re-read the draft after any document change, keeping the view and selection.
    void Rebuild();
    /// Current World drawable reference in physical pixels; empty means unavailable.
    /// Refreshes size annotations without changing drafts, selection, layout or undo history.
    void SetReferenceSize(const QSize& size);
    /// Frame all node boxes, including configuration interfaces. View only: preserves
    /// selection, authored positions and undo. Empty graphs are a no-op; magnification is bounded.
    void FitGraph();
    /// Frame the selected authored nodes; no selection is a no-op. Never edits the draft.
    void FitSelection();
    /// Restore 100% magnification around the viewport centre, preserving selection and draft.
    void ResetZoom();
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
    /// Type of a displayed port, such as ColorTexture; empty when the canvas does not show it.
    [[nodiscard]] QString GetPortType(const QString& endpoint, bool isOutput) const;
    /// Domain colour category of a displayed node (for tests and accessibility text).
    [[nodiscard]] RenderGraphDomain GetNodeDomain(const QString& node) const;
    void SelectNode(const QString& node);
    /// Select authored nodes without emitting a user-selection event; unknown IDs are ignored.
    void SelectNodes(const QStringList& nodes);
    /// Scroll the node into view without changing the current zoom or selection.
    void RevealNode(const QString& node);
    [[nodiscard]] QStringList GetSelectedNodes() const;
    /// Remove the display marker and accessibility description without changing graph state.
    void ClearDiagnostic();
    /// Reveal a diagnostic's authored node/interface and ring its exact port when present.
    /// Missing ports fall back to their owner. Display only; cleared by the next rebuild.
    /// Returns false when no displayed owner exists; never changes document/history.
    [[nodiscard]] bool ShowDiagnostic(const QJsonObject& location, const QString& description);
    /// Current display marker, empty after a rebuild or when its owner is absent.
    [[nodiscard]] QJsonObject GetDiagnosticLocation() const;

    /// Gesture entry points shared by mouse handling and tests. A gesture moves every
    /// selected node live in the view and reports the final positions once in EndDrag.
    void BeginDrag(const QStringList& nodes);
    void UpdateDrag(QPointF delta);
    void EndDrag();
    /// Request an edge from an output to an input; rejected links emit LinkRejected.
    /// With shouldReplaceExisting, a legal drop replaces the input source in one undo step.
    [[nodiscard]] bool RequestLink(const QString& from, const QString& to, bool shouldReplaceExisting = false);
    /// Activate a node as a double-click does: enter a Subgraph or request node details.
    [[nodiscard]] bool ActivateNode(const QString& node);
    /// Layout position for node, a record not in the draft yet, that puts its input connector on
    /// inputPosition once it is added with a `from` -> `node.input` connection. Uses the same port
    /// rows as the canvas, including hidden Scene bundle members. Returns inputPosition when the
    /// node has no such input.
    [[nodiscard]] QPointF
    GetNewNodePosition(const QJsonObject& node, const QString& from, const QString& input, QPointF inputPosition) const;

Q_SIGNALS:
    /// Open the displayed configuration interface editor (double-click an interface box).
    void InterfaceEditRequested();
    void NodeSelected(const QString& node);
    /// Complete authored-node selection after a mouse or keyboard gesture.
    void NodesSelected(const QStringList& nodes);
    void CopyRequested();
    void PasteRequested();
    void RemoveNodesRequested(const QStringList& nodes);
    /// Context target is empty on the background or configuration interface.
    void ContextMenuRequested(const QString& node, const QPoint& globalPosition);
    /// Open Quick Add at a scene position: Shift+A at the pointer (or the view centre when the
    /// pointer is outside), or a double-click on empty canvas. Never edits the draft.
    void QuickAddRequested(QPointF scenePosition);
    /// Open Quick Add filtered to nodes that accept the output `from`: a wire from it was released on
    /// empty canvas (inputPosition is the drop point), or the output was clicked without dragging
    /// (inputPosition is a column gap to its right). The new node's input belongs at inputPosition.
    /// Never edits the draft.
    void ConnectQuickAddRequested(const QString& from, QPointF inputPosition);
    void MoveRequested(const QVector<ludus::editor::RenderGraphNodePosition>& positions);
    void ConnectRequested(const QString& from, const QString& to);
    void ReplaceConnectionRequested(const QString& from, const QString& to);
    void DisconnectRequested(const QString& from, const QString& to);
    void RemoveRequested(const QString& node);
    /// Open shared authoring controls for an ordinary node; Subgraphs keep their enter gesture.
    void NodeDetailsRequested(const QString& node);
    void SubgraphActivated(const QString& asset, const QString& configuration);
    void LinkRejected(const QString& reason);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct NodeView;
    struct EdgeView;
    [[nodiscard]] QJsonObject configuration() const;
    void updateEdges();
    void updateSelection();
    void updateConnectionHighlight();
    void notifySelection();
    [[nodiscard]] QString nodeAt(QPointF position) const;
    [[nodiscard]] QString portAt(QPointF position, bool& outIsOutput) const;
    [[nodiscard]] QString edgeAt(QPointF position, QString& outTo) const;
    void cancelLink();
    void refreshFeedback();
    void updateLinkFeedback(QPointF position);
    void fitBounds(const QRectF& bounds);
    void updateFramingControls();
    void updateSemanticZoom(bool shouldForce = false);
    /// Scroll the view so its content moves by a viewport-pixel delta; never edits the draft.
    void panBy(QPoint delta);
    void zoomAt(foundation::float64 factor, QPointF position);

    const RenderGraphDocument& mDocument;
    QGraphicsScene* mScene;
    QToolBar* mFramingToolbar;
    QAction* mFitGraphAction;
    QAction* mFitSelectionAction;
    QLabel* mZoomLabel;
    foundation::float64 mPresentedScale = 0;
    QGraphicsRectItem* mDiagnosticMarkerOrNull = nullptr;
    QGraphicsSimpleTextItem* mDiagnosticLabelOrNull = nullptr;
    QString mDiagnosticText;
    QJsonObject mDiagnosticLocation;
    QString mFile;
    QString mConfiguration;
    QHash<QString, NodeView*> mNodes;
    QStringList mExpandedSceneBundles;
    QVector<EdgeView*> mEdges;
    QStringList mDragNodes;
    QHash<QString, QPointF> mDragOrigins;
    QPointF mDragOffset;
    QPointF mPressPosition;
    QString mLinkFrom;
    QGraphicsPathItem* mLinkPreviewOrNull = nullptr;
    QGraphicsTextItem* mLinkHintOrNull = nullptr;
    QGraphicsEllipseItem* mLinkPortOrNull = nullptr;
    QString mLinkPortTooltip;
    QString mLinkEndpoint;
    QString mHoveredPort;
    bool mIsHoveredOutput = false;
    QSize mReferenceSize;
    QRubberBand* mSelectionBox;
    QPoint mSelectionStart;
    QPoint mPanPosition;
    QStringList mSelectionBeforeBox;
    bool mIsBoxSelecting = false;
    bool mIsPanning = false;
    bool mIsSpacePressed = false;
    bool mIsDragging = false;
    bool mShouldFitOnShow = false;
};
} // namespace ludus::editor
