#include "internal/render_graph_minimap.h"

#include "design_tokens.h"
#include "internal/render_graph_canvas.h"
#include "internal/theme_colour.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Map size in design spacing steps: 6 x 4 pages, a 3:2 box that suits wide pipeline graphs.
constexpr int32 MAP_WIDTH_STEPS = 6;
constexpr int32 MAP_HEIGHT_STEPS = 4;
constexpr int32 MAP_WIDTH = MAP_WIDTH_STEPS * design::SPACING_PAGE;
constexpr int32 MAP_HEIGHT = MAP_HEIGHT_STEPS * design::SPACING_PAGE;
/// Opacity (of 255) of the map's backing, so the graph can still be read behind a corner overlay.
constexpr int32 BACKGROUND_ALPHA = 230;
constexpr int32 VIEW_FILL_ALPHA = 36;
constexpr int32 CARD_ALPHA = 210;
constexpr float64 VIEW_OUTLINE_WIDTH = 1.5;
/// A card is never drawn smaller than this, so a 128-card graph stays visible on a small map.
constexpr float64 MINIMUM_CARD_EXTENT = 2;
} // namespace

QPointF RenderGraphMinimapProjection::ToMap(QPointF scenePoint) const
{
    return Origin + (scenePoint - Content.topLeft()) * Scale;
}

QPointF RenderGraphMinimapProjection::ToScene(QPointF mapPoint) const
{
    return Content.topLeft() + (mapPoint - Origin) / Scale;
}

QRectF RenderGraphMinimapProjection::ToMap(const QRectF& sceneRectangle) const
{
    return QRectF(ToMap(sceneRectangle.topLeft()), ToMap(sceneRectangle.bottomRight()));
}

RenderGraphMinimapProjection MakeRenderGraphMinimapProjection(const QRectF& content, const QRectF& target)
{
    RenderGraphMinimapProjection projection;
    projection.Content = content;
    projection.Target = target;
    if (content.isValid() == false || content.width() <= 0 || content.height() <= 0 ||
        math::IsFinite(content.width()) == false || math::IsFinite(content.height()) == false || target.width() <= 0 ||
        target.height() <= 0)
    {
        projection.Content = QRectF(QPointF(0, 0), QSizeF(target.width(), target.height()));
        projection.Origin = target.topLeft();
        return projection;
    }
    projection.Scale = math::Min(target.width() / content.width(), target.height() / content.height());
    const QSizeF shown = content.size() * projection.Scale;
    projection.Origin =
        target.topLeft() + QPointF((target.width() - shown.width()) * 0.5, (target.height() - shown.height()) * 0.5);
    return projection;
}

RenderGraphMinimap::RenderGraphMinimap(RenderGraphCanvas& inoutCanvas, QWidget* parentOrNull)
    : QWidget(parentOrNull), mCanvas(inoutCanvas)
{
    setObjectName(QStringLiteral("renderGraphMinimap"));
    setAccessibleName(QStringLiteral("Graph minimap"));
    setAccessibleDescription(QStringLiteral("Overview of the whole graph. Click or drag to move the view."));
    setToolTip(QStringLiteral("Overview of the whole graph · click or drag to move the view · M hides it"));
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
}

QSize RenderGraphMinimap::sizeHint() const
{
    return QSize(MAP_WIDTH, MAP_HEIGHT);
}

RenderGraphMinimapProjection RenderGraphMinimap::GetProjection() const
{
    if (mIsDragging == true)
    {
        // The view moving must not rescale the map under the pointer while it is being dragged.
        return mDragProjection;
    }
    QRectF content = mCanvas.GetVisibleSceneRectangle();
    for (const RenderGraphMinimapCard& card : mCanvas.GetMinimapCards())
    {
        content = content.united(card.Bounds);
    }
    const QRectF target = QRectF(rect()).adjusted(design::SPACING_SMALL,
                                                  design::SPACING_SMALL,
                                                  -design::SPACING_SMALL,
                                                  -design::SPACING_SMALL);
    return MakeRenderGraphMinimapProjection(content, target);
}

QPointF RenderGraphMinimap::GetScenePosition(QPoint widgetPosition) const
{
    const RenderGraphMinimapProjection projection = GetProjection();
    const QPointF scenePoint = projection.ToScene(QPointF(widgetPosition));
    return QPointF(math::Clamp(scenePoint.x(), projection.Content.left(), projection.Content.right()),
                   math::Clamp(scenePoint.y(), projection.Content.top(), projection.Content.bottom()));
}

void RenderGraphMinimap::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(GetCanvasColour(design::ThemeColour::BorderControl), 1));
    painter.setBrush(
        QColor::fromRgba(WithAlpha(GetCanvasColour(design::ThemeColour::SurfaceRaised).rgb(), BACKGROUND_ALPHA)));
    painter.drawRoundedRect(frame, design::RADIUS_CONTROL, design::RADIUS_CONTROL);
    const RenderGraphMinimapProjection projection = GetProjection();
    painter.setPen(Qt::NoPen);
    const QColor selection = GetCanvasColour(design::ThemeColour::SelectionAmber);
    for (const RenderGraphMinimapCard& card : mCanvas.GetMinimapCards())
    {
        QRectF block = projection.ToMap(card.Bounds);
        block.setWidth(math::Max(block.width(), MINIMUM_CARD_EXTENT));
        block.setHeight(math::Max(block.height(), MINIMUM_CARD_EXTENT));
        QColor colour = card.IsSelected == true ? selection : card.Colour;
        colour.setAlpha(CARD_ALPHA);
        painter.setBrush(colour);
        painter.drawRect(block);
    }
    const QColor accent = GetCanvasColour(design::ThemeColour::AccentPrimary);
    QColor fill = accent;
    fill.setAlpha(VIEW_FILL_ALPHA);
    painter.setBrush(fill);
    painter.setPen(QPen(accent, VIEW_OUTLINE_WIDTH));
    painter.drawRect(projection.ToMap(mCanvas.GetVisibleSceneRectangle()));
}

void RenderGraphMinimap::centreViewAt(QPoint widgetPosition)
{
    mCanvas.CentreViewOn(GetScenePosition(widgetPosition));
    update();
}

void RenderGraphMinimap::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        event->ignore();
        return;
    }
    mDragProjection = GetProjection();
    mIsDragging = true;
    centreViewAt(event->position().toPoint());
    event->accept();
}

void RenderGraphMinimap::mouseMoveEvent(QMouseEvent* event)
{
    if (mIsDragging == false)
    {
        event->ignore();
        return;
    }
    centreViewAt(event->position().toPoint());
    event->accept();
}

void RenderGraphMinimap::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        mIsDragging = false;
        event->accept();
    }
}
} // namespace ludus::editor
