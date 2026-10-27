#pragma once

#include <ludus/foundation/base/types.h>

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QAction;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QToolBar;
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Direction through displayed connections in one configuration; never enters Subgraph assets.
enum class RenderGraphTraceDirection : foundation::uint8
{
    None,       ///< Normal selection/port-hover highlighting.
    Upstream,   ///< Producers that reach the selected nodes.
    Downstream, ///< Consumers reachable from the selected nodes.
};

/// Snapshot of a displayed node. Only authored nodes can be searched or used as trace roots.
struct RenderGraphNavigationNode final
{
    QString Identity;       ///< Canvas identity, used to select and trace the node.
    QString Kind;           ///< Authored node kind, also searchable when ports are unresolved.
    bool IsAuthored = true; ///< False for configuration interface boxes.
};

/// Directed displayed connection, including configuration-interface links.
struct RenderGraphNavigationEdge final
{
    QString Source;      ///< Producer node identity, rather than a port endpoint.
    QString Destination; ///< Consumer node identity, rather than a port endpoint.
};

/// Transitive reachability, including roots. Edge indices refer to the current SetGraph snapshot.
/// Cycles terminate through a visited set; malformed endpoints are ignored.
struct RenderGraphDependencyTrace final
{
    QSet<QString> Nodes;
    QSet<qsizetype> Edges;
};

/// Canvas navigation UI and its view-only search/trace model. Owns no document or scene items.
/// GUI thread only. Snapshot copies remain valid independently of the canvas's item lifetimes.
class RenderGraphNavigation final : public QObject
{
    Q_OBJECT
public:
    /// Add controls to toolbar; shortcuts apply only within canvas, which owns this component.
    RenderGraphNavigation(QToolBar& inoutToolbar, QWidget& inoutCanvas);
    /// Replace the displayed graph, close stale search results and retain valid trace roots.
    /// The canvas must refresh its styling after this call; no selection/trace signal is emitted.
    void SetGraph(const QVector<RenderGraphNavigationNode>& nodes, const QVector<RenderGraphNavigationEdge>& edges);
    /// Follow the owner's current authored selection. Empty selection clears the trace.
    /// The owner refreshes styling itself; this call emits no navigation request.
    void SetSelection(const QStringList& nodes);
    /// Choose a trace direction, recompute reachability and emit TraceChanged when it changes.
    /// A nonempty authored selection is required for either tracing direction.
    void SetTraceDirection(RenderGraphTraceDirection direction);
    /// Current mode; None after clearing a trace or losing all authored selection roots.
    [[nodiscard]] RenderGraphTraceDirection GetTraceDirection() const noexcept;
    /// Current cached trace; reference remains valid until the next graph/selection/direction change.
    [[nodiscard]] const RenderGraphDependencyTrace& GetTrace() const noexcept;
    /// Open a search over the current configuration, without previewing selection or changing the view.
    void OpenFindNode();

Q_SIGNALS:
    /// Confirmed existing authored node. The popup has closed; the canvas selects and frames it.
    void NodeRequested(const QString& identity);
    /// Direction changed through a control or Escape; the canvas should refresh its styling.
    void TraceChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] QStringList getAuthoredSelection(const QStringList& nodes) const;
    void refreshResults();
    void acceptResult();
    void refreshTrace();
    void refreshActions();

    QToolBar& mToolbar;
    QWidget& mCanvas;
    QAction* mFindAction;
    QAction* mUpstreamAction;
    QAction* mDownstreamAction;
    QAction* mClearAction;
    QFrame* mPopup;
    QLineEdit* mSearch;
    QListWidget* mResults;
    QLabel* mResultCount;
    QVector<RenderGraphNavigationNode> mNodes;
    QVector<RenderGraphNavigationEdge> mEdges;
    QStringList mSelection;
    RenderGraphTraceDirection mDirection = RenderGraphTraceDirection::None;
    RenderGraphDependencyTrace mTrace;
};
} // namespace ludus::editor
