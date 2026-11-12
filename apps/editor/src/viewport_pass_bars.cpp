#include "internal/viewport_pass_bars.h"
#include "design_tokens.h"
#include "internal/theme_colour.h"

namespace ludus::editor
{
namespace
{
/// Corner radius of a pass bar, in logical pixels.
constexpr qreal BAR_RADIUS = 2;
/// Opacity, of 255, of a pass bar behind its row text; low enough to keep the text readable.
constexpr int BAR_OPACITY = 120;
/// Pass bar of every scope but the slowest.
constexpr QRgb BAR_COLOUR = WithAlpha(GetViewportArgb(design::ThemeColour::AccentPrimary), BAR_OPACITY);
/// Pass bar of the slowest scope when it dominates the others, so the frame's bottleneck stands out at a glance.
constexpr QRgb SLOWEST_BAR_COLOUR = WithAlpha(GetViewportArgb(design::ThemeColour::StatusWarning), BAR_OPACITY);
} // namespace
void PaintViewportPassBars(QPainter& inoutPainter,
                           const QRectF& textArea,
                           qreal lineHeight,
                           qsizetype firstLine,
                           const ViewportPassBars& bars)
{
    if (firstLine < 0)
    {
        return;
    }
    inoutPainter.save();
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    inoutPainter.setPen(Qt::NoPen);
    for (foundation::usize row = 0; row < bars.RowCount; ++row)
    {
        const QRectF bar(textArea.left(),
                         textArea.top() + static_cast<qreal>(firstLine + static_cast<qsizetype>(row)) * lineHeight,
                         textArea.width() * static_cast<qreal>(bars.Fractions[row]),
                         lineHeight);
        inoutPainter.setBrush(QColor::fromRgba(
            bars.IsSlowestWarning == true && row == bars.SlowestRow ? SLOWEST_BAR_COLOUR : BAR_COLOUR));
        inoutPainter.drawRoundedRect(bar, BAR_RADIUS, BAR_RADIUS);
    }
    inoutPainter.restore();
}
} // namespace ludus::editor
