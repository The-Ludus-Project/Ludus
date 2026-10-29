#include "internal/orientation_widget.h"

#include "design_tokens.h"

#include <ludus/foundation/math/quaternion.hpp>

#include <QColor>
#include <QFont>
#include <QPainter>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr qreal ARM_LENGTH_PIXEL = 29;
constexpr qreal POSITIVE_RADIUS_PIXEL = 9;
constexpr qreal NEGATIVE_RADIUS_PIXEL = 5.5;
constexpr qreal LINE_WIDTH_PIXEL = 2;
constexpr qreal RING_WIDTH_PIXEL = 1.5;
constexpr qreal RING_GAP_PIXEL = 2;
/// Extra pixels around a handle that still count as a click on it.
constexpr qreal HIT_SLOP_PIXEL = 2;
constexpr int NEGATIVE_FILL_ALPHA = 110;
constexpr int DISC_BACKGROUND_ALPHA = 70;
constexpr int OUTLINE_ALPHA = 150;
constexpr qreal LETTER_POINT_SIZE = 8.5;
constexpr QRgb LETTER_COLOUR = design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true);
constexpr QRgb RING_COLOUR = design::GetThemeArgb(design::ThemeColour::TextHud, true);

struct AxisEnd final
{
    EditorAxisView View;
    math::Vector3 DirectionWorld;
    QRgb Colour;
    char Letter;
    bool IsPositive;
};
/// Looking at the scene from +X is the Right view (it looks along -X), from +Y the Top view, from +Z the Front view.
const StaticArray<AxisEnd, ORIENTATION_HANDLE_COUNT> AXIS_ENDS{{
    {EditorAxisView::Right, {1, 0, 0}, design::GetThemeArgb(design::ThemeColour::AxisX, true), 'X', true},
    {EditorAxisView::Left, {-1, 0, 0}, design::GetThemeArgb(design::ThemeColour::AxisX, true), 0, false},
    {EditorAxisView::Top, {0, 1, 0}, design::GetThemeArgb(design::ThemeColour::AxisY, true), 'Y', true},
    {EditorAxisView::Bottom, {0, -1, 0}, design::GetThemeArgb(design::ThemeColour::AxisY, true), 0, false},
    {EditorAxisView::Front, {0, 0, 1}, design::GetThemeArgb(design::ThemeColour::AxisZ, true), 'Z', true},
    {EditorAxisView::Back, {0, 0, -1}, design::GetThemeArgb(design::ThemeColour::AxisZ, true), 0, false},
}};
} // namespace

QRectF GetOrientationWidgetRect(qreal viewportWidthPixel) noexcept
{
    return {viewportWidthPixel - ORIENTATION_WIDGET_SIZE_PIXEL - ORIENTATION_WIDGET_MARGIN_PIXEL,
            ORIENTATION_WIDGET_MARGIN_PIXEL,
            ORIENTATION_WIDGET_SIZE_PIXEL,
            ORIENTATION_WIDGET_SIZE_PIXEL};
}

OrientationLayout BuildOrientationLayout(const gameplay::camera::CameraSample& sample, const QRectF& rect) noexcept
{
    OrientationLayout layout;
    layout.OriginPixel = rect.center();
    const math::Vector3 rightWorld = math::Rotate(sample.Pose.Orientation, {1, 0, 0});
    const math::Vector3 upWorld = math::Rotate(sample.Pose.Orientation, {0, 1, 0});
    const math::Vector3 forwardWorld = math::Rotate(sample.Pose.Orientation, {0, 0, -1});
    for (usize index = 0; index < ORIENTATION_HANDLE_COUNT; ++index)
    {
        const AxisEnd& end = AXIS_ENDS[index];
        OrientationHandle& handle = layout.Handles[index];
        handle.View = end.View;
        handle.CentrePixel = layout.OriginPixel + QPointF(static_cast<qreal>(math::Dot(end.DirectionWorld, rightWorld)),
                                                          -static_cast<qreal>(math::Dot(end.DirectionWorld, upWorld))) *
                                                      ARM_LENGTH_PIXEL;
        handle.RadiusPixel = end.IsPositive == true ? POSITIVE_RADIUS_PIXEL : NEGATIVE_RADIUS_PIXEL;
        handle.Depth = math::Dot(end.DirectionWorld, forwardWorld);
        handle.Colour = end.Colour;
        handle.Letter = end.Letter;
        handle.IsPositive = end.IsPositive;
    }
    // Insertion sort, farthest first, so nearer handles paint over farther ones.
    for (usize index = 1; index < ORIENTATION_HANDLE_COUNT; ++index)
    {
        const OrientationHandle moving = layout.Handles[index];
        usize slot = index;
        while (slot > 0 && layout.Handles[slot - 1].Depth < moving.Depth)
        {
            layout.Handles[slot] = layout.Handles[slot - 1];
            --slot;
        }
        layout.Handles[slot] = moving;
    }
    return layout;
}

bool TryHitOrientationHandle(const OrientationLayout& layout, QPointF pointPixel, EditorAxisView& outView) noexcept
{
    // The last handle is the nearest one, so test from the back of the array.
    for (usize index = ORIENTATION_HANDLE_COUNT; index > 0; --index)
    {
        const OrientationHandle& handle = layout.Handles[index - 1];
        const QPointF offset = pointPixel - handle.CentrePixel;
        const qreal reach = handle.RadiusPixel + HIT_SLOP_PIXEL;
        if (QPointF::dotProduct(offset, offset) <= reach * reach)
        {
            outView = handle.View;
            return true;
        }
    }
    return false;
}

void PaintOrientationWidget(QPainter& inoutPainter,
                            const OrientationLayout& layout,
                            const gameplay::camera::CameraSample& sample)
{
    inoutPainter.save();
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    QColor discBackground = QColor::fromRgb(design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true));
    discBackground.setAlpha(DISC_BACKGROUND_ALPHA);
    inoutPainter.setPen(Qt::NoPen);
    inoutPainter.setBrush(discBackground);
    inoutPainter.drawEllipse(layout.OriginPixel,
                             ARM_LENGTH_PIXEL + POSITIVE_RADIUS_PIXEL + RING_GAP_PIXEL,
                             ARM_LENGTH_PIXEL + POSITIVE_RADIUS_PIXEL + RING_GAP_PIXEL);
    EditorAxisView current = EditorAxisView::Front;
    const bool isOnAxis = TryGetEditorAxisView(sample, current);
    QFont font = inoutPainter.font();
    font.setBold(true);
    font.setPointSizeF(LETTER_POINT_SIZE);
    for (const OrientationHandle& handle : layout.Handles)
    {
        QColor colour = QColor::fromRgb(handle.Colour);
        if (handle.IsPositive == true)
        {
            inoutPainter.setPen(QPen(colour, LINE_WIDTH_PIXEL, Qt::SolidLine, Qt::RoundCap));
            inoutPainter.drawLine(layout.OriginPixel, handle.CentrePixel);
            inoutPainter.setPen(Qt::NoPen);
            inoutPainter.setBrush(colour);
        }
        else
        {
            QColor fill = colour;
            fill.setAlpha(NEGATIVE_FILL_ALPHA);
            colour.setAlpha(OUTLINE_ALPHA);
            inoutPainter.setPen(QPen(colour, 1));
            inoutPainter.setBrush(fill);
        }
        inoutPainter.drawEllipse(handle.CentrePixel, handle.RadiusPixel, handle.RadiusPixel);
        if (handle.Letter != 0)
        {
            inoutPainter.setFont(font);
            inoutPainter.setPen(QColor::fromRgb(LETTER_COLOUR));
            inoutPainter.drawText(QRectF(handle.CentrePixel.x() - handle.RadiusPixel,
                                         handle.CentrePixel.y() - handle.RadiusPixel,
                                         handle.RadiusPixel * 2,
                                         handle.RadiusPixel * 2),
                                  Qt::AlignCenter,
                                  QString(QLatin1Char(handle.Letter)));
        }
        if (isOnAxis == true && handle.View == current)
        {
            inoutPainter.setBrush(Qt::NoBrush);
            inoutPainter.setPen(QPen(QColor::fromRgb(RING_COLOUR), RING_WIDTH_PIXEL));
            const qreal ringRadius = handle.RadiusPixel + RING_GAP_PIXEL;
            inoutPainter.drawEllipse(handle.CentrePixel, ringRadius, ringRadius);
        }
    }
    inoutPainter.restore();
}
} // namespace ludus::editor
