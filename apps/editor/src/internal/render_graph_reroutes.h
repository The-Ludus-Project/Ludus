#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_wire_routes.h"

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QVector>

QT_BEGIN_NAMESPACE
class QGraphicsEllipseItem;
class QGraphicsPathItem;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphCanvas;
/// Canvas-owned layout-only wire handles; GUI thread only. Borrows scene-owned paths until
/// Clear. Requests one document transaction per add, remove or completed drag; graph selection
/// and dependency trace remain independent. Preview cancellation never edits the document.
class RenderGraphReroutes final : public QObject
{
    Q_OBJECT
public:
    /// Canvas owns the component and must outlive it.
    explicit RenderGraphReroutes(RenderGraphCanvas& inoutCanvas);
    /// Release handles while the borrowed canvas scene is still alive.
    ~RenderGraphReroutes() override;
    /// Release handles and borrowed paths before the scene is cleared. Retains point selection identity.
    void Clear();
    /// Copy valid layout metadata; invalid metadata is preserved in Source and reported via Rejected.
    void Rebuild(const QJsonObject& configuration);
    /// Borrow a scene path for a logical wire and create its handles. Path outlives Clear.
    void BindWire(const QString& from, const QString& to, QGraphicsPathItem& inoutPath);
    /// Copy ordered scene points, including an active drag preview; empty for an unrouted wire.
    [[nodiscard]] QVector<QPointF> GetPoints(const QString& from, const QString& to) const;
    /// Bounds of all saved/preview points; empty when there are none.
    [[nodiscard]] QRectF GetBounds() const;
    /// Inherit wire colour/opacity; distinguish the independently selected point.
    void RefreshStyles();
    /// Insert at segment index (zero through current point count); reject stale metadata or capacity.
    void AddPoint(const QString& from, const QString& to, foundation::int32 segment, QPointF position);
    /// Return whether a visible point is under position, using a screen-sized hit target.
    [[nodiscard]] bool HasPointAt(QPointF position) const;
    /// Select and start a point drag without changing node selection or trace.
    [[nodiscard]] bool BeginGesture(QPointF position);
    /// Preview a bounded position; returns false when no drag is active.
    [[nodiscard]] bool UpdateGesture(QPointF position);
    /// Commit a changed drag as one undo step, resetting gesture state before synchronous rebuilds.
    [[nodiscard]] bool EndGesture();
    /// Restore the drag origin without editing; returns whether a drag was active.
    [[nodiscard]] bool CancelGesture();
    /// Clear point selection only; returns whether a point was selected.
    [[nodiscard]] bool ClearSelection();
    /// Remove the selected point (and empty route); returns whether a point selection was handled.
    [[nodiscard]] bool DeleteSelectedPoint();
    /// Show point removal on a handle; endpoint identities/revision are checked after the modal menu.
    [[nodiscard]] bool ShowContextMenu(QPointF position, const QPoint& globalPosition);
Q_SIGNALS:
    /// Complete replacement routes, for an atomic document edit through the workspace.
    void EditRequested(const QJsonArray& routes);
    /// Changed preview geometry needs wire paths refreshed.
    void PreviewChanged();
    /// Invalid/stale metadata or a bounded edit rejection.
    void Rejected(const QString& reason);

private:
    struct Wire
    {
        QString From;
        QString To;
        QGraphicsPathItem* Path;
    };
    [[nodiscard]] foundation::int32 findRoute(const QString& from, const QString& to) const;
    [[nodiscard]] QGraphicsEllipseItem* hitPointOrNull(QPointF position) const;
    [[nodiscard]] bool isCurrent() const;
    void renderHandles();
    void publish(const QJsonArray& routes);
    RenderGraphCanvas& mCanvas;
    QVector<Wire> mWires;
    QVector<QGraphicsEllipseItem*> mHandles;
    QJsonArray mRoutes;
    QJsonArray mBeforeRoutes;
    QJsonObject mConfigurationRecord;
    QString mFile;
    QString mConfiguration;
    QString mSelectedFrom;
    QString mSelectedTo;
    foundation::int32 mSelectedPoint = -1;
    foundation::uint64 mRevision = 0;
    bool mHasValidMetadata = true;
    bool mIsDragging = false;
    QPointF mPressPosition;
    QPointF mOrigin;
};
} // namespace ludus::editor
