#include "internal/viewport_gizmo_readout.h"
#include "design_tokens.h"
#include "internal/theme_colour.h"
#include "internal/theme_fonts.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QChar>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MOVE_DECIMALS = 3;
constexpr int32 ROTATE_DECIMALS = 1;
constexpr int32 SCALE_DECIMALS = 2;
/// Opacity, of 255, of the label background; the same as the statistics panel over the viewport.
constexpr int32 BACKGROUND_OPACITY = 190;
constexpr QRgb BACKGROUND = WithAlpha(GetViewportArgb(design::ThemeColour::SurfaceViewport), BACKGROUND_OPACITY);
constexpr QRgb TEXT = GetViewportArgb(design::ThemeColour::TextHud);
constexpr float64 HORIZONTAL_PADDING_PIXELS = design::SPACING_SMALL;
constexpr float64 VERTICAL_PADDING_PIXELS = design::SPACING_EXTRA_SMALL;
constexpr float64 CORNER_RADIUS_PIXELS = design::RADIUS_CONTROL;
/// Distance of the label from the anchor on each axis, so the pointer does not cover it.
constexpr float64 ANCHOR_OFFSET_PIXELS = design::SPACING_LARGE;
constexpr char16_t DEGREE_SIGN = u'\u00B0';
constexpr char16_t MULTIPLICATION_SIGN = u'\u00D7';
constexpr char16_t FIRST_AXIS_LETTER = u'X';

QChar GetAxisLetter(int32 axis)
{
    return QChar(static_cast<char16_t>(FIRST_AXIS_LETTER + axis));
}

/// True for "-0", "-0.0" and the like, which a value that rounds to zero would otherwise print.
bool IsNegativeZero(const QString& text)
{
    if (text.startsWith(QLatin1Char('-')) == false)
    {
        return false;
    }
    for (qsizetype index = 1; index < text.size(); ++index)
    {
        if (text[index] != QLatin1Char('0') && text[index] != QLatin1Char('.'))
        {
            return false;
        }
    }
    return true;
}

/// The value rounded to decimals places, with a plus sign when positive and no sign when it rounds to zero.
QString FormatSigned(float64 value, int32 decimals)
{
    QString text = QString::number(value, 'f', decimals);
    if (IsNegativeZero(text) == true)
    {
        text.remove(0, 1);
    }
    if (text.toDouble() > 0)
    {
        text.prepend(QLatin1Char('+'));
    }
    return text;
}

QFont GetReadoutFont()
{
    return MakeTabularFont(MakeTypeFont(QFontDatabase::systemFont(QFontDatabase::FixedFont), TypeRole::Caption));
}
} // namespace

QString FormatMoveReadout(const GizmoReadoutComponent& first, const GizmoReadoutComponent& second)
{
    QString text = QString(GetAxisLetter(first.Axis)) + QLatin1Char(' ') + FormatSigned(first.Value, MOVE_DECIMALS);
    if (second.Axis != GIZMO_READOUT_NO_AXIS)
    {
        text += QStringLiteral("  ") + GetAxisLetter(second.Axis) + QLatin1Char(' ') +
                FormatSigned(second.Value, MOVE_DECIMALS);
    }
    return text + QStringLiteral(" m");
}

QString FormatRotateReadout(int32 axis, float64 degrees)
{
    return QString(GetAxisLetter(axis)) + QLatin1Char(' ') + FormatSigned(degrees, ROTATE_DECIMALS) +
           QChar(DEGREE_SIGN);
}

QString FormatScaleReadout(int32 axis, float64 factor)
{
    const QString number = QChar(MULTIPLICATION_SIGN) + QString::number(factor, 'f', SCALE_DECIMALS);
    return axis == GIZMO_READOUT_NO_AXIS ? number : QString(GetAxisLetter(axis)) + QLatin1Char(' ') + number;
}

QSizeF MeasureGizmoReadout(const QString& text)
{
    if (text.isEmpty() == true)
    {
        return {};
    }
    const QFontMetricsF metrics(GetReadoutFont());
    return {metrics.horizontalAdvance(text) + 2 * HORIZONTAL_PADDING_PIXELS,
            metrics.height() + 2 * VERTICAL_PADDING_PIXELS};
}

QPointF PlaceGizmoReadout(QPointF anchorPixel, QSizeF sizePixel, const QRectF& framePixel)
{
    QPointF originPixel = anchorPixel + QPointF(ANCHOR_OFFSET_PIXELS, ANCHOR_OFFSET_PIXELS);
    if (originPixel.x() + sizePixel.width() > framePixel.right())
    {
        originPixel.setX(anchorPixel.x() - ANCHOR_OFFSET_PIXELS - sizePixel.width());
    }
    if (originPixel.y() + sizePixel.height() > framePixel.bottom())
    {
        originPixel.setY(anchorPixel.y() - ANCHOR_OFFSET_PIXELS - sizePixel.height());
    }
    // A frame narrower than the label keeps its left and top edges visible.
    originPixel.setX(math::Max(originPixel.x(), framePixel.left()));
    originPixel.setY(math::Max(originPixel.y(), framePixel.top()));
    return originPixel;
}

void PaintGizmoReadout(QPainter& inoutPainter, const QString& text, QPointF originPixel)
{
    if (text.isEmpty() == true)
    {
        return;
    }
    const QRectF labelPixel(originPixel, MeasureGizmoReadout(text));
    inoutPainter.save();
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    inoutPainter.setPen(Qt::NoPen);
    inoutPainter.setBrush(QColor::fromRgba(BACKGROUND));
    inoutPainter.drawRoundedRect(labelPixel, CORNER_RADIUS_PIXELS, CORNER_RADIUS_PIXELS);
    inoutPainter.setFont(GetReadoutFont());
    inoutPainter.setPen(QColor::fromRgb(TEXT));
    inoutPainter.drawText(labelPixel, Qt::AlignCenter, text);
    inoutPainter.restore();
}
} // namespace ludus::editor
