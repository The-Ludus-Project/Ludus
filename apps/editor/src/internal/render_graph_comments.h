#pragma once

#include "internal/render_graph_comment_frame.h"
#include "internal/render_graph_document.h"

#include <QHash>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QVector>

QT_BEGIN_NAMESPACE
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsEllipseItem;
class QGraphicsSimpleTextItem;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphCanvas;

/// Logical emphasis shared by full node cards and their collapsed stage proxies; contains no scene pointers.
struct RenderGraphCommentHighlights final
{
    QStringList Selection;     ///< Authored selection IDs, including hidden nodes.
    QSet<QString> TracedNodes; ///< All logical nodes visited by the active dependency trace.
    QSet<QString> Inputs;      ///< Emphasized input endpoint identities.
    QSet<QString> Outputs;     ///< Emphasized output endpoint identities.
};

/// Canvas-owned comment presentation and gestures. Scene items are borrowed until Clear;
/// copied frame metadata never owns graph nodes. GUI thread only. Edits are requested through
/// the workspace's document transaction, keeping node selection/trace independent of frames.
class RenderGraphComments final : public QObject
{
    Q_OBJECT
public:
    /// The canvas and scene must outlive this component; canvas owns it as a QObject child.
    RenderGraphComments(RenderGraphCanvas& inoutCanvas, QGraphicsScene& inoutScene);
    ~RenderGraphComments() override;
    /// Drop presentation/gesture state before the canvas clears its scene. No document edit.
    void Clear();
    /// Show valid saved metadata from a configuration. Malformed arrays are skipped with feedback.
    void Rebuild(const QJsonObject& configuration);
    /// Union of displayed frame rectangles; empty when none are shown.
    [[nodiscard]] QRectF GetBounds() const;
    /// Displayed rectangle for one frame, empty if missing. Includes live gesture preview.
    [[nodiscard]] QRectF GetFrameBounds(const QString& identity) const;
    /// True only for nonempty stages whose saved bounds intersect no other comment frame.
    [[nodiscard]] bool CanCollapse(const QString& identity) const;
    /// Effective hidden owner for an authored node; empty for visible nodes.
    [[nodiscard]] QString GetCollapsedFrame(const QString& node) const;
    /// Save one collapse/expand edit without changing node coordinates, selection or trace.
    void SetCollapsed(const QString& identity, bool isCollapsed);
    /// Temporarily expand a node's stage for search/diagnostics, without a document edit.
    void RevealNode(const QString& node);
    /// Temporary expansion IDs captured/restored by browser history; unknown IDs are ignored.
    [[nodiscard]] QStringList GetRevealedFrames() const;
    /// Restore temporary expansion IDs in this configuration; unknown and duplicate IDs are ignored.
    void SetRevealedFrames(const QStringList& frames);
    /// Boundary connectors keep the original endpoint identity and direction. Borrowed scene item.
    [[nodiscard]] QGraphicsEllipseItem* GetBoundaryPortOrNull(const QString& endpoint, bool isOutput) const;
    /// Hit a visible boundary connector, returning its original endpoint; empty on a miss.
    [[nodiscard]] QString GetPortAt(QPointF position, bool& outIsOutput) const;
    /// Update stage selection/trace emphasis and original endpoint port emphasis, without rebuilding items.
    void RefreshHighlights(const RenderGraphCommentHighlights& highlights);
    /// Refresh node/report error counts and freshness labels on existing stage cards.
    void RefreshCounts();
    /// Create a frame around selected nodes, or at position when selection is empty; opens shared edit controls.
    void CreateFrame(QPointF position);
    /// Edit title, plain-text notes and RGB colour, preserving geometry. Missing IDs are a no-op.
    void EditFrame(const QString& identity);
    /// Remove only this frame, retaining every node. Missing IDs are a no-op.
    void RemoveFrame(const QString& identity);
    /// Open frame actions when a header/resize handle is hit; returns whether handled.
    [[nodiscard]] bool ShowContextMenu(QPointF position, const QPoint& globalPosition);
    /// Edit an expanded header or expand a collapsed card on double-click; no node/trace selection change.
    [[nodiscard]] bool ActivateAt(QPointF position);
    /// Start a header move or bottom-right resize. Group membership is full node containment at press.
    /// Does not change node selection/trace; returns false on empty space or stale metadata.
    [[nodiscard]] bool BeginGesture(QPointF position);
    /// Preview a bounded rigid translation or frame-only resize; false when no gesture is active.
    [[nodiscard]] bool UpdateGesture(QPointF position);
    /// Publish one combined frame/node edit after resetting gesture state for synchronous rebuilds.
    [[nodiscard]] bool EndGesture();
    /// Restore preview origins without editing; true only when a gesture was active.
    [[nodiscard]] bool CancelGesture();

Q_SIGNALS:
    /// Complete replacement metadata plus grouped saved node positions, one document transaction.
    void EditRequested(const QJsonArray& frames, const QVector<ludus::editor::RenderGraphNodePosition>& positions);
    /// Rejected or stale metadata/edit; suitable for the workspace status line.
    void Rejected(const QString& reason);

private:
    [[nodiscard]] foundation::int32 findFrame(const QString& identity) const;
    [[nodiscard]] foundation::int32 hitFrame(QPointF position) const;
    void renderFrames();
    void refreshPresentation();
    void editFrame(QJsonObject frame, bool isNew);
    [[nodiscard]] bool isCurrent() const;
    void publish(const QJsonArray& frames, const QVector<RenderGraphNodePosition>& positions = {});

    RenderGraphCanvas& mCanvas;
    QGraphicsScene& mScene;
    QJsonArray mFrames;
    QJsonObject mConfigurationRecord;
    QHash<QString, QStringList> mMembers;
    QHash<QString, QString> mCollapsedOwners;
    QStringList mRevealedFrames;
    struct BoundaryPort final
    {
        QString Endpoint;
        bool IsOutput = false;
        QGraphicsEllipseItem* Item;
    };
    QVector<BoundaryPort> mBoundaryPorts;
    QHash<QString, QGraphicsSimpleTextItem*> mCounts;
    QHash<QString, QGraphicsSimpleTextItem*> mPlanSummaries;
    QVector<QGraphicsRectItem*> mItems;
    QString mFile;
    QString mConfiguration;
    foundation::uint64 mRevision = 0;
    foundation::int32 mGestureIndex = -1;
    bool mIsResizing = false;
    bool mHasValidMetadata = true;
    QPointF mPressPosition;
    QRectF mBeforeBounds;
    QRectF mPreviewBounds;
    QVector<RenderGraphNodePosition> mBeforePositions;
    QVector<RenderGraphNodePosition> mPreviewPositions;
};
} // namespace ludus::editor
