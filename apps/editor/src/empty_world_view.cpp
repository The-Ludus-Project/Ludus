#include "internal/empty_world_view.h"
#include "internal/render_preview.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/world_grid.hpp>

#include <QPainter>
#include <QPalette>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr int32 TEXT_MARGIN = 12;
constexpr usize CLIP_PLANE_COUNT = 6;
constexpr QRgb BACKGROUND = qRgb(9, 12, 15);
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
        namespace renderer = graphics::renderer;
        StaticArray<renderer::WorldGridLine, renderer::WORLD_GRID_LINE_CAPACITY> lines;
        usize count = 0;
        float32 step = 0;
        if (renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, step) == false)
        {
            painter.end();
            QLabel::paintEvent(event);
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing);
        for (usize i = 0; i < count; ++i)
        {
            const renderer::WorldGridLine& line = lines[i];
            const QColor colour = QColor::fromRgbF(static_cast<float64>(math::EncodeSrgb(line.Color.X)),
                                                   static_cast<float64>(math::EncodeSrgb(line.Color.Y)),
                                                   static_cast<float64>(math::EncodeSrgb(line.Color.Z)));
            DrawLine(painter, view, size(), line.StartWorld, line.EndWorld, colour.rgb());
        }
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
