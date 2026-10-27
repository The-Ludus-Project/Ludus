#include "internal/render_graph_reroutes.h"
#include "internal/render_graph_canvas.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QJsonValue>
#include <QMenu>
#include <QPen>
#include <QSizeF>
#include <QtAlgorithms>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr float64 HANDLE_RADIUS = 5;
constexpr float64 HIT_RADIUS = 10;
constexpr float64 SELECTED_WIDTH = 3;
constexpr QRgb HANDLE_FILL = 0xff1d252c;
constexpr QRgb SELECTED_FILL = 0xffeeeeee;
QJsonObject MakePoint(QPointF position)
{
    return {{QStringLiteral("x"), qRound(position.x())}, {QStringLiteral("y"), qRound(position.y())}};
}
QPointF BoundPoint(QPointF position)
{
    return QPointF(math::Clamp(position.x(),
                               -static_cast<float64>(layout_policy::MAX_COORDINATE),
                               static_cast<float64>(layout_policy::MAX_COORDINATE)),
                   math::Clamp(position.y(),
                               -static_cast<float64>(layout_policy::MAX_COORDINATE),
                               static_cast<float64>(layout_policy::MAX_COORDINATE)));
}
} // namespace
RenderGraphReroutes::RenderGraphReroutes(RenderGraphCanvas& inoutCanvas) : QObject(&inoutCanvas), mCanvas(inoutCanvas)
{
}
RenderGraphReroutes::~RenderGraphReroutes()
{
    Clear();
}
void RenderGraphReroutes::Clear()
{
    mIsDragging = false;
    mBeforeRoutes = {};
    qDeleteAll(mHandles);
    mHandles.clear();
    mWires.clear();
    mRoutes = {};
}
void RenderGraphReroutes::Rebuild(const QJsonObject& configuration)
{
    Clear();
    if (mFile != mCanvas.GetFile() || mConfiguration != mCanvas.GetConfiguration())
    {
        static_cast<void>(ClearSelection());
    }
    mFile = mCanvas.GetFile();
    mConfiguration = mCanvas.GetConfiguration();
    mRevision = mCanvas.GetDocumentRevision();
    mConfigurationRecord = configuration;
    const QJsonValue value = configuration.value(QStringLiteral("layout")).toObject().value(QStringLiteral("routes"));
    mHasValidMetadata =
        value.isUndefined() == true ||
        (value.isArray() == true && AreRenderGraphWireRoutesValid(value.toArray(), configuration) == true);
    if (mHasValidMetadata == false)
    {
        static_cast<void>(ClearSelection());
        Q_EMIT Rejected(QStringLiteral("Invalid wire routes; repair layout.routes in Source."));
        return;
    }
    mRoutes = value.toArray();
    if (mSelectedPoint >= GetPoints(mSelectedFrom, mSelectedTo).size())
    {
        static_cast<void>(ClearSelection());
    }
}
void RenderGraphReroutes::BindWire(const QString& from, const QString& to, QGraphicsPathItem& inoutPath)
{
    mWires.append({from, to, &inoutPath});
    // Each binding owns only its own handles; other paths and logical edge indices are untouched.
    const QVector<QPointF> points = GetPoints(from, to);
    for (int32 index = 0; index < points.size(); ++index)
    {
        QGraphicsEllipseItem* handle =
            new QGraphicsEllipseItem(-HANDLE_RADIUS, -HANDLE_RADIUS, 2 * HANDLE_RADIUS, 2 * HANDLE_RADIUS, &inoutPath);
        handle->setFlag(QGraphicsItem::ItemIgnoresTransformations);
        handle->setAcceptedMouseButtons(Qt::NoButton);
        handle->setPos(points.at(index));
        handle->setData(0, from);
        handle->setData(1, to);
        handle->setData(2, index);
        handle->setToolTip(QStringLiteral("Reroute point · drag to move · Delete or right-click to remove"));
        mHandles.append(handle);
    }
    RefreshStyles();
}
int32 RenderGraphReroutes::findRoute(const QString& from, const QString& to) const
{
    for (int32 index = 0; index < mRoutes.size(); ++index)
    {
        const QJsonObject route = mRoutes.at(index).toObject();
        if (route.value(QStringLiteral("from")).toString() == from &&
            route.value(QStringLiteral("to")).toString() == to)
        {
            return index;
        }
    }
    return -1;
}
QVector<QPointF> RenderGraphReroutes::GetPoints(const QString& from, const QString& to) const
{
    QVector<QPointF> points;
    const int32 route = findRoute(from, to);
    if (route >= 0)
    {
        for (const QJsonValueConstRef value : mRoutes.at(route).toObject().value(QStringLiteral("points")).toArray())
        {
            const QJsonObject point = value.toObject();
            points.append(QPointF(point.value(QStringLiteral("x")).toInt(), point.value(QStringLiteral("y")).toInt()));
        }
    }
    return points;
}
QRectF RenderGraphReroutes::GetBounds() const
{
    QRectF bounds;
    for (const QGraphicsEllipseItem* handle : mHandles)
    {
        bounds = bounds.united(
            QRectF(handle->pos() - QPointF(HIT_RADIUS, HIT_RADIUS), QSizeF(2 * HIT_RADIUS, 2 * HIT_RADIUS)));
    }
    return bounds;
}
void RenderGraphReroutes::RefreshStyles()
{
    for (QGraphicsEllipseItem* handle : mHandles)
    {
        const bool isSelected = handle->data(0).toString() == mSelectedFrom &&
                                handle->data(1).toString() == mSelectedTo && handle->data(2).toInt() == mSelectedPoint;
        const QGraphicsPathItem* path = static_cast<QGraphicsPathItem*>(handle->parentItem());
        QPen pen = path->pen();
        pen.setWidthF(isSelected == true ? SELECTED_WIDTH : 2);
        pen.setCosmetic(true);
        handle->setPen(pen);
        handle->setBrush(QColor::fromRgba(isSelected == true ? SELECTED_FILL : HANDLE_FILL));
        // A broken endpoint produces an empty path. Keep its metadata but do not expose an orphan handle.
        handle->setVisible(path->path().isEmpty() == false);
    }
}
bool RenderGraphReroutes::isCurrent() const
{
    return mHasValidMetadata == true && mFile.isEmpty() == false && mFile == mCanvas.GetFile() &&
           mConfiguration == mCanvas.GetConfiguration() && mRevision == mCanvas.GetDocumentRevision();
}
void RenderGraphReroutes::publish(const QJsonArray& routes)
{
    if (isCurrent() == false || AreRenderGraphWireRoutesValid(routes, mConfigurationRecord) == false)
    {
        Q_EMIT Rejected(QStringLiteral("Wire route edit is stale, invalid or exceeds layout capacity."));
        return;
    }
    Q_EMIT EditRequested(routes);
}
void RenderGraphReroutes::AddPoint(const QString& from, const QString& to, int32 segment, QPointF position)
{
    if (isCurrent() == false || math::IsFinite(position.x()) == false || math::IsFinite(position.y()) == false)
    {
        Q_EMIT Rejected(QStringLiteral("Wire route metadata changed or is invalid; reopen the connection menu."));
        return;
    }
    QJsonArray routes = mRoutes;
    const int32 index = findRoute(from, to);
    QJsonObject route = index >= 0 ? routes.at(index).toObject()
                                   : QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}};
    QJsonArray points = route.value(QStringLiteral("points")).toArray();
    if (segment < 0 || segment > points.size() || points.size() >= layout_policy::MAX_WIRE_REROUTE_POINTS)
    {
        Q_EMIT Rejected(QStringLiteral("Wire point limit reached or insertion segment is invalid."));
        return;
    }
    points.insert(segment, MakePoint(BoundPoint(position)));
    route.insert(QStringLiteral("points"), points);
    if (index >= 0)
    {
        routes.replace(index, route);
    }
    else
    {
        routes.append(route);
    }
    publish(routes);
    // Only select after an accepted synchronous transaction/rebuild.
    if (mRoutes == routes)
    {
        mSelectedFrom = from;
        mSelectedTo = to;
        mSelectedPoint = segment;
        RefreshStyles();
    }
}
QGraphicsEllipseItem* RenderGraphReroutes::hitPointOrNull(QPointF position) const
{
    const float64 radius = HIT_RADIUS / mCanvas.transform().m11();
    for (QGraphicsEllipseItem* handle : mHandles)
    {
        if (handle->isVisible() == true &&
            QRectF(handle->pos() - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius)).contains(position) == true)
        {
            return handle;
        }
    }
    return nullptr;
}
bool RenderGraphReroutes::HasPointAt(QPointF position) const
{
    return hitPointOrNull(position) != nullptr;
}
bool RenderGraphReroutes::BeginGesture(QPointF position)
{
    QGraphicsEllipseItem* handleOrNull = hitPointOrNull(position);
    if (handleOrNull == nullptr)
    {
        return false;
    }
    if (isCurrent() == false)
    {
        return true;
    }
    mSelectedFrom = handleOrNull->data(0).toString();
    mSelectedTo = handleOrNull->data(1).toString();
    mSelectedPoint = handleOrNull->data(2).toInt();
    mOrigin = handleOrNull->pos();
    mPressPosition = position;
    mBeforeRoutes = mRoutes;
    mIsDragging = true;
    RefreshStyles();
    return true;
}
void RenderGraphReroutes::renderHandles()
{
    qDeleteAll(mHandles);
    mHandles.clear();
    const QVector<Wire> wires = mWires;
    mWires.clear();
    for (const Wire& wire : wires)
    {
        BindWire(wire.From, wire.To, *wire.Path);
    }
    Q_EMIT PreviewChanged();
    RefreshStyles();
}
bool RenderGraphReroutes::UpdateGesture(QPointF position)
{
    if (mIsDragging == false)
    {
        return false;
    }
    if (isCurrent() == false)
    {
        static_cast<void>(CancelGesture());
        Q_EMIT Rejected(QStringLiteral("Wire route drag cancelled because the graph changed."));
        return true;
    }
    const QPointF next = mOrigin + position - mPressPosition;
    if (math::IsFinite(next.x()) == false || math::IsFinite(next.y()) == false)
    {
        return true;
    }
    const int32 index = findRoute(mSelectedFrom, mSelectedTo);
    QJsonObject route = mRoutes.at(index).toObject();
    QJsonArray points = route.value(QStringLiteral("points")).toArray();
    points.replace(mSelectedPoint, MakePoint(BoundPoint(next)));
    route.insert(QStringLiteral("points"), points);
    mRoutes.replace(index, route);
    renderHandles();
    return true;
}
bool RenderGraphReroutes::EndGesture()
{
    if (mIsDragging == false)
    {
        return false;
    }
    const QJsonArray routes = mRoutes;
    const bool hasChanged = routes != mBeforeRoutes;
    mRoutes = mBeforeRoutes;
    mBeforeRoutes = {};
    mIsDragging = false;
    renderHandles();
    if (hasChanged == true)
    {
        publish(routes);
    }
    return true;
}
bool RenderGraphReroutes::CancelGesture()
{
    if (mIsDragging == false)
    {
        return false;
    }
    mRoutes = mBeforeRoutes;
    mBeforeRoutes = {};
    mIsDragging = false;
    renderHandles();
    return true;
}
bool RenderGraphReroutes::ClearSelection()
{
    const bool hasSelection = mSelectedPoint >= 0;
    mSelectedPoint = -1;
    mSelectedFrom.clear();
    mSelectedTo.clear();
    RefreshStyles();
    return hasSelection;
}
bool RenderGraphReroutes::DeleteSelectedPoint()
{
    if (mSelectedPoint < 0)
    {
        return false;
    }
    if (isCurrent() == false)
    {
        return true;
    }
    const int32 index = findRoute(mSelectedFrom, mSelectedTo);
    if (index < 0)
    {
        static_cast<void>(ClearSelection());
        return true;
    }
    QJsonArray routes = mRoutes;
    QJsonObject route = routes.at(index).toObject();
    QJsonArray points = route.value(QStringLiteral("points")).toArray();
    points.removeAt(mSelectedPoint);
    if (points.isEmpty() == true)
    {
        routes.removeAt(index);
    }
    else
    {
        route.insert(QStringLiteral("points"), points);
        routes.replace(index, route);
    }
    publish(routes);
    if (mRoutes == routes)
    {
        static_cast<void>(ClearSelection());
    }
    return true;
}
bool RenderGraphReroutes::ShowContextMenu(QPointF position, const QPoint& globalPosition)
{
    QGraphicsEllipseItem* handleOrNull = hitPointOrNull(position);
    if (handleOrNull == nullptr)
    {
        return false;
    }
    const QString from = handleOrNull->data(0).toString();
    const QString to = handleOrNull->data(1).toString();
    const int32 point = handleOrNull->data(2).toInt();
    const uint64 revision = mRevision;
    const QString file = mFile;
    const QString configuration = mConfiguration;
    QMenu menu(&mCanvas);
    menu.setObjectName(QStringLiteral("renderGraphRerouteMenu"));
    QAction* remove = menu.addAction(QStringLiteral("Remove Reroute Point"));
    remove->setObjectName(QStringLiteral("renderGraphRemoveReroute"));
    if (menu.exec(globalPosition) == remove && isCurrent() == true && revision == mRevision && file == mFile &&
        configuration == mConfiguration)
    {
        mSelectedFrom = from;
        mSelectedTo = to;
        mSelectedPoint = point;
        static_cast<void>(DeleteSelectedPoint());
    }
    return true;
}
} // namespace ludus::editor
