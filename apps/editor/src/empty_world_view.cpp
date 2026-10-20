#include "internal/empty_world_view.h"
#include "internal/render_preview.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QPainter>
#include <QPalette>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr int32 GRID_HALF_COUNT = 40;
constexpr int32 MAJOR_LINE_PERIOD = 10;
constexpr float32 MINIMUM_GRID_STEP = 0.000001F;
constexpr float32 MAXIMUM_GRID_STEP = 1000000;
constexpr float32 GRID_SCALE = 10;
constexpr int32 TEXT_MARGIN = 12;
constexpr usize CLIP_PLANE_COUNT = 6;
constexpr QRgb BACKGROUND = qRgb(9, 12, 15);
constexpr QRgb MINOR_GRID = qRgb(39, 45, 52);
constexpr QRgb MAJOR_GRID = qRgb(65, 73, 82);
constexpr QRgb X_AXIS = qRgb(205, 76, 76);
constexpr QRgb Y_AXIS = qRgb(94, 180, 100);
constexpr QRgb Z_AXIS = qRgb(81, 133, 220);
constexpr QRgb CAPTION = qRgb(175, 184, 194);

StaticArray<float32, CLIP_PLANE_COUNT> GetClipDistances(math::Vector4 positionClip) noexcept
{
    return {positionClip.W + positionClip.X,
            positionClip.W - positionClip.X,
            positionClip.W + positionClip.Y,
            positionClip.W - positionClip.Y,
            positionClip.Z,
            positionClip.W - positionClip.Z};
}
/// Clip before dividing by W, including segments crossing the eye/near plane.
void DrawLine(QPainter& inoutPainter,
              const graphics::renderer::ViewDescription& view,
              QSize size,
              math::Vector3 startWorld,
              math::Vector3 endWorld,
              QRgb colour)
{
    const math::Vector4 startClip = view.WorldToClip * math::Vector4{startWorld.X, startWorld.Y, startWorld.Z, 1};
    const math::Vector4 endClip = view.WorldToClip * math::Vector4{endWorld.X, endWorld.Y, endWorld.Z, 1};
    const StaticArray<float32, CLIP_PLANE_COUNT> startDistances = GetClipDistances(startClip);
    const StaticArray<float32, CLIP_PLANE_COUNT> endDistances = GetClipDistances(endClip);
    float32 first = 0;
    float32 last = 1;
    for (usize i = 0; i < CLIP_PLANE_COUNT; ++i)
    {
        const float32 start = startDistances[i];
        const float32 end = endDistances[i];
        if (start < 0 && end < 0)
        {
            return;
        }
        if (start < 0)
        {
            first = math::Max(first, start / (start - end));
        }
        else if (end < 0)
        {
            last = math::Min(last, start / (start - end));
        }
    }
    if (first > last)
    {
        return;
    }
    const math::Vector4 a = startClip + (endClip - startClip) * first;
    const math::Vector4 b = startClip + (endClip - startClip) * last;
    if (a.W <= 0 || b.W <= 0 || math::IsFinite(a) == false || math::IsFinite(b) == false)
    {
        return;
    }
    const auto project = [size](math::Vector4 positionClip) {
        return QPointF(
            static_cast<qreal>((positionClip.X / positionClip.W + 1) * 0.5F * static_cast<float32>(size.width())),
            static_cast<qreal>((1 - positionClip.Y / positionClip.W) * 0.5F * static_cast<float32>(size.height())));
    };
    inoutPainter.setPen(QColor::fromRgb(colour));
    inoutPainter.drawLine(project(a), project(b));
}
} // namespace
EmptyWorldView::EmptyWorldView(RenderPreview& inoutPreview, QWidget* parentOrNull)
    : QLabel(parentOrNull), mPreview(inoutPreview)
{
    setObjectName(QStringLiteral("emptyWorldView"));
    setAccessibleName(QStringLiteral("Empty world coordinate grid"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAlignment(Qt::AlignBottom | Qt::AlignLeft);
    setMargin(TEXT_MARGIN);
    setWordWrap(true);
    QPalette colours = palette();
    colours.setColor(QPalette::WindowText, QColor::fromRgb(CAPTION));
    setPalette(colours);
    connect(&mPreview, &RenderPreview::NavigationChanged, this, [this]() { update(); });
    connect(&mPreview, &RenderPreview::Changed, this, [this]() { update(); });
}
void EmptyWorldView::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor::fromRgb(BACKGROUND));
    graphics::renderer::ViewDescription view;
    QString error;
    if (mPreview.Scene().Identity.isEmpty() == true && width() > 0 && height() > 0 &&
        mPreview.DescribeView(static_cast<uint32>(width()), static_cast<uint32>(height()), view, error) == true)
    {
        float32 step = 1;
        // Decade spacing keeps a bounded line count as the camera dollies or flies.
        const math::Vector3 verticalClipRow{view.WorldToClip.At(1, 0),
                                            view.WorldToClip.At(1, 1),
                                            view.WorldToClip.At(1, 2)};
        const float32 visibleSpan =
            view.OrthographicCamera == true ? 2 / math::Length(verticalClipRow) : mPreview.GetFocusDistance();
        const float32 aspect = static_cast<float32>(width()) / static_cast<float32>(height());
        const float32 scale = math::Max(visibleSpan, MINIMUM_GRID_STEP) * math::Max(1.0F, aspect) / GRID_SCALE;
        while (step > scale && step > MINIMUM_GRID_STEP)
        {
            step /= GRID_SCALE;
        }
        while (step * GRID_SCALE <= scale && step < MAXIMUM_GRID_STEP)
        {
            step *= GRID_SCALE;
        }
        math::Vector3 horizontalWorld{1, 0, 0};
        math::Vector3 verticalWorld{0, 0, 1};
        if (view.OrthographicCamera == true)
        {
            const math::Vector3 forwardWorld = view.CameraForward;
            if (math::Abs(forwardWorld.Z) >= math::Abs(forwardWorld.X) &&
                math::Abs(forwardWorld.Z) >= math::Abs(forwardWorld.Y))
            {
                verticalWorld = {0, 1, 0};
            }
            else if (math::Abs(forwardWorld.X) > math::Abs(forwardWorld.Y))
            {
                horizontalWorld = {0, 0, 1};
                verticalWorld = {0, 1, 0};
            }
        }
        const math::Vector3 focusWorld = view.CameraPosition + view.CameraForward * mPreview.GetFocusDistance();
        const float32 centerHorizontal =
            static_cast<float32>(math::Floor(static_cast<float64>(math::Dot(focusWorld, horizontalWorld) / step)));
        const float32 centerVertical =
            static_cast<float32>(math::Floor(static_cast<float64>(math::Dot(focusWorld, verticalWorld) / step)));
        const float32 extent = static_cast<float32>(GRID_HALF_COUNT) * step;
        const math::Vector3 centerWorld =
            horizontalWorld * (centerHorizontal * step) + verticalWorld * (centerVertical * step);
        painter.setRenderHint(QPainter::Antialiasing);
        for (int32 i = -GRID_HALF_COUNT; i <= GRID_HALF_COUNT; ++i)
        {
            const float32 offset = static_cast<float32>(i) * step;
            const auto colour = [i](float32 center) {
                const float32 coordinate = center + static_cast<float32>(i);
                const float32 period = static_cast<float32>(MAJOR_LINE_PERIOD);
                return coordinate ==
                               static_cast<float32>(math::Floor(static_cast<float64>(coordinate / period))) * period
                           ? MAJOR_GRID
                           : MINOR_GRID;
            };
            const math::Vector3 horizontalStart = centerWorld + verticalWorld * offset;
            const math::Vector3 verticalStart = centerWorld + horizontalWorld * offset;
            DrawLine(painter,
                     view,
                     size(),
                     horizontalStart - horizontalWorld * extent,
                     horizontalStart + horizontalWorld * extent,
                     colour(centerVertical));
            DrawLine(painter,
                     view,
                     size(),
                     verticalStart - verticalWorld * extent,
                     verticalStart + verticalWorld * extent,
                     colour(centerHorizontal));
        }
        DrawLine(painter, view, size(), {-extent, 0, 0}, {extent, 0, 0}, X_AXIS);
        DrawLine(painter, view, size(), {0, -extent, 0}, {0, extent, 0}, Y_AXIS);
        DrawLine(painter, view, size(), {0, 0, -extent}, {0, 0, extent}, Z_AXIS);
        painter.setPen(QColor::fromRgb(CAPTION));
        painter.drawText(
            rect().adjusted(TEXT_MARGIN, TEXT_MARGIN, -TEXT_MARGIN, -TEXT_MARGIN),
            Qt::AlignTop | Qt::AlignLeft,
            QStringLiteral("%1 · grid %2 world units · X red / Y green / Z blue")
                .arg(view.OrthographicCamera == true ? QStringLiteral("Orthographic") : QStringLiteral("Perspective"))
                .arg(static_cast<float64>(step)));
    }
    painter.end();
    QLabel::paintEvent(event);
}
} // namespace ludus::editor
