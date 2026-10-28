#include "internal/empty_world_view.h"
#include "internal/render_preview.h"
#include "internal/state_view.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/world_grid.hpp>

#include <QPainter>
#include <QPointF>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

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
constexpr QRgb SECONDARY_CAPTION = qRgb(150, 160, 170);
/// Share of white mixed into an axis colour for its letter, so the letter reads at text contrast.
constexpr float32 AXIS_LETTER_LIGHTEN = 0.35F;
/// Smallest spread between the brightest and darkest channel of a coloured axis line; grey grid lines have none.
constexpr float32 AXIS_MINIMUM_SATURATION = 0.2F;
/// Distance a letter sits inward from the end of its axis line, in pixels.
constexpr qreal AXIS_LETTER_INSET = 14;
/// Side of the square a letter is centred in, in pixels.
constexpr qreal AXIS_LETTER_BOX = 16;
/// Axis letters, in X, Y, Z order.
constexpr const char* AXIS_LETTERS[] = {"X", "Y", "Z"};

StaticArray<float32, CLIP_PLANE_COUNT> GetClipDistances(math::Vector4 positionClip) noexcept
{
    return {positionClip.W + positionClip.X,
            positionClip.W - positionClip.X,
            positionClip.W + positionClip.Y,
            positionClip.W - positionClip.Y,
            positionClip.Z,
            positionClip.W - positionClip.Z};
}
/// Where a clipped line ended up on screen, for placing a label at its far end.
struct DrawnLine final
{
    bool IsVisible = false;
    QPointF StartScreen;
    QPointF EndScreen;
};
/// Clip before dividing by W, including segments crossing the eye/near plane. Returns the visible part's ends in
/// widget pixels; the end is the visible part's far end, on the viewport border when the line runs off screen.
DrawnLine DrawLine(QPainter& inoutPainter,
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
            return {};
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
        return {};
    }
    const math::Vector4 a = startClip + (endClip - startClip) * first;
    const math::Vector4 b = startClip + (endClip - startClip) * last;
    if (a.W <= 0 || b.W <= 0 || math::IsFinite(a) == false || math::IsFinite(b) == false)
    {
        return {};
    }
    const auto project = [size](math::Vector4 positionClip) {
        return QPointF(
            static_cast<qreal>((positionClip.X / positionClip.W + 1) * 0.5F * static_cast<float32>(size.width())),
            static_cast<qreal>((1 - positionClip.Y / positionClip.W) * 0.5F * static_cast<float32>(size.height())));
    };
    inoutPainter.setPen(QColor::fromRgb(colour));
    const DrawnLine drawn{true, project(a), project(b)};
    inoutPainter.drawLine(drawn.StartScreen, drawn.EndScreen);
    return drawn;
}
} // namespace
/// Axis a world grid line lies along (0 X, 1 Y, 2 Z), or a negative value for a grid line off the axes.
int32 GetAxisIndexOrNegative(const graphics::renderer::WorldGridLine& line) noexcept
{
    constexpr int32 NOT_AN_AXIS = -1;
    // Grey grid lines through the origin are symmetric too; only the coloured axes carry a letter.
    const float32 brightest = math::Max(line.Color.X, math::Max(line.Color.Y, line.Color.Z));
    const float32 darkest = math::Min(line.Color.X, math::Min(line.Color.Y, line.Color.Z));
    if (brightest - darkest < AXIS_MINIMUM_SATURATION)
    {
        return NOT_AN_AXIS;
    }
    if ((line.StartWorld == -line.EndWorld) == false)
    {
        return NOT_AN_AXIS;
    }
    int32 axis = NOT_AN_AXIS;
    for (int32 candidate = 0; candidate < static_cast<int32>(graphics::renderer::WORLD_GRID_AXIS_COUNT); ++candidate)
    {
        const float32 component =
            candidate == 0 ? line.EndWorld.X : (candidate == 1 ? line.EndWorld.Y : line.EndWorld.Z);
        if (component != 0)
        {
            if (axis != NOT_AN_AXIS)
            {
                return NOT_AN_AXIS;
            }
            axis = candidate;
        }
    }
    return axis;
}
/// Mix of the axis colour toward white so the letter keeps WCAG text contrast on the dark viewport.
QColor GetAxisLetterColour(QColor axisColour)
{
    return QColor::fromRgbF(axisColour.redF() + (1 - axisColour.redF()) * AXIS_LETTER_LIGHTEN,
                            axisColour.greenF() + (1 - axisColour.greenF()) * AXIS_LETTER_LIGHTEN,
                            axisColour.blueF() + (1 - axisColour.blueF()) * AXIS_LETTER_LIGHTEN);
}
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
    QVBoxLayout* centre = new QVBoxLayout(this);
    centre->setContentsMargins(TEXT_MARGIN, TEXT_MARGIN, TEXT_MARGIN, TEXT_MARGIN);
    mProblemOrNull = new StateView(this);
    mProblemOrNull->setObjectName(QStringLiteral("viewportProblem"));
    mProblemOrNull->SetFixedColours({QColor::fromRgb(CAPTION), QColor::fromRgb(SECONDARY_CAPTION)});
    mProblemOrNull->hide();
    connect(mProblemOrNull, &StateView::ActionTriggered, this, [this]() { update(); });
    centre->addWidget(mProblemOrNull, 0, Qt::AlignCenter);
    connect(&mPreview, &RenderPreview::NavigationChanged, this, [this]() { update(); });
    connect(&mPreview, &RenderPreview::Changed, this, [this]() { update(); });
}
void EmptyWorldView::SetPrompt(const QString& text)
{
    mProblemOrNull->hide();
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setText(text);
    update();
}
void EmptyWorldView::ShowProblem(const QString& status)
{
    const StateViewContent content = MakeViewportProblem(status);
    mProblemOrNull->SetContent(content);
    mProblemOrNull->show();
    // Error actions must receive mouse input; plain prompts pass it through for viewport navigation.
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    // The sentence stays the label text so assistive technology and tests read the same words.
    setText(content.Message);
    update();
}
bool EmptyWorldView::HasProblem() const noexcept
{
    return mProblemOrNull->isVisibleTo(this);
}
void EmptyWorldView::paintEvent(QPaintEvent* /*event*/)
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
            drawCaption(painter);
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing);
        for (usize i = 0; i < count; ++i)
        {
            const renderer::WorldGridLine& line = lines[i];
            const QColor colour = QColor::fromRgbF(static_cast<float64>(math::EncodeSrgb(line.Color.X)),
                                                   static_cast<float64>(math::EncodeSrgb(line.Color.Y)),
                                                   static_cast<float64>(math::EncodeSrgb(line.Color.Z)));
            const DrawnLine drawn = DrawLine(painter, view, size(), line.StartWorld, line.EndWorld, colour.rgb());
            const int32 axis = GetAxisIndexOrNegative(line);
            if (axis >= 0 && drawn.IsVisible == true)
            {
                // Colour alone must not name an axis: put its letter near the visible far end.
                QPointF towardStart = drawn.StartScreen - drawn.EndScreen;
                const qreal length = std::hypot(towardStart.x(), towardStart.y());
                towardStart = length > AXIS_LETTER_INSET ? towardStart * (AXIS_LETTER_INSET / length) : towardStart;
                QFont font = painter.font();
                font.setBold(true);
                painter.setFont(font);
                painter.setPen(GetAxisLetterColour(colour));
                const QPointF inset = drawn.EndScreen + towardStart;
                const QPointF centre(std::clamp(inset.x(), AXIS_LETTER_BOX / 2, width() - AXIS_LETTER_BOX / 2),
                                     std::clamp(inset.y(), AXIS_LETTER_BOX / 2, height() - AXIS_LETTER_BOX / 2));
                painter.drawText(QRectF(centre.x() - AXIS_LETTER_BOX / 2,
                                        centre.y() - AXIS_LETTER_BOX / 2,
                                        AXIS_LETTER_BOX,
                                        AXIS_LETTER_BOX),
                                 Qt::AlignCenter,
                                 QString::fromLatin1(AXIS_LETTERS[axis]));
            }
        }
        painter.setPen(QColor::fromRgb(CAPTION));
        painter.drawText(
            rect().adjusted(TEXT_MARGIN, TEXT_MARGIN, -TEXT_MARGIN, -TEXT_MARGIN),
            Qt::AlignTop | Qt::AlignLeft,
            QStringLiteral("%1 · grid %2 world units · X red / Y green / Z blue")
                .arg(view.OrthographicCamera == true ? QStringLiteral("Orthographic") : QStringLiteral("Perspective"))
                .arg(static_cast<float64>(step)));
    }
    drawCaption(painter);
}
void EmptyWorldView::drawCaption(QPainter& inoutPainter) const
{
    // Painted here rather than by QLabel: the workspace style sheet colours every QLabel from the window
    // palette, which left this caption charcoal on charcoal. The viewport background is fixed, so the
    // caption colour is too.
    if (HasProblem() == true)
    {
        return;
    }
    inoutPainter.setRenderHint(QPainter::Antialiasing, false);
    inoutPainter.setPen(QColor::fromRgb(CAPTION));
    inoutPainter.drawText(rect().adjusted(TEXT_MARGIN, TEXT_MARGIN, -TEXT_MARGIN, -TEXT_MARGIN),
                          Qt::AlignBottom | Qt::AlignLeft | Qt::TextWordWrap,
                          text());
}
} // namespace ludus::editor
