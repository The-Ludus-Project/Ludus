#include "internal/viewport_hud_text.h"
#include "design_tokens.h"
#include "internal/overlay_widgets.h"
#include "internal/theme_colour.h"
#include "internal/theme_fonts.h"
#include "internal/viewport_statistics.h"

#include <QApplication>
#include <QColor>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace ludus::editor
{
namespace
{
/// One grid row: a label and a value, or a line that spans both columns when label is empty.
struct HudRow final
{
    QString Label;
    QString Value;
};

[[nodiscard]] HudRow SplitRow(const QString& line)
{
    const qsizetype split = line.indexOf(VIEWPORT_STATISTICS_VALUE_SEPARATOR);
    if (split <= 0)
    {
        return { .Label = QString(), .Value = line };
    }
    return { .Label = line.left(split), .Value = line.mid(split + VIEWPORT_STATISTICS_VALUE_SEPARATOR.size()) };
}

/// Widest label and widest value of the rows that have both.
struct HudColumns final
{
    qreal LabelWidth = 0;
    qreal ValueWidth = 0;
    qreal SpanningWidth = 0;
};

[[nodiscard]] HudColumns MeasureColumns(const QStringList& lines, const QFontMetricsF& metrics)
{
    HudColumns columns;
    for (const QString& line : lines)
    {
        const HudRow row = SplitRow(line);
        if (row.Label.isEmpty() == true)
        {
            columns.SpanningWidth = std::max(columns.SpanningWidth, metrics.horizontalAdvance(row.Value));
            continue;
        }
        columns.LabelWidth = std::max(columns.LabelWidth, metrics.horizontalAdvance(row.Label));
        columns.ValueWidth = std::max(columns.ValueWidth, metrics.horizontalAdvance(row.Value));
    }
    return columns;
}

[[nodiscard]] qreal GetGridWidth(const HudColumns& columns)
{
    const qreal gridWidth =
        columns.LabelWidth > 0 ? columns.LabelWidth + GetViewportHudColumnGap() + columns.ValueWidth : 0;
    return std::max(gridWidth, columns.SpanningWidth);
}
} // namespace

QFont GetViewportHudFont()
{
    return MakeTabularFont(QApplication::font());
}

qreal GetViewportHudColumnGap()
{
    return design::SPACING_MEDIUM;
}

QSizeF MeasureViewportHudGrid(const QString& text, const QFont& font)
{
    if (text.isEmpty() == true)
    {
        return {};
    }
    const QFontMetricsF metrics(font);
    const QStringList lines = text.split(QLatin1Char('\n'));
    return {GetGridWidth(MeasureColumns(lines, metrics)), metrics.lineSpacing() * static_cast<qreal>(lines.size())};
}

void PaintViewportHudGrid(QPainter& inoutPainter,
                          const QRectF& area,
                          const QString& text,
                          const QFont& font,
                          QRgb labelColour,
                          QRgb valueColour)
{
    if (text.isEmpty() == true)
    {
        return;
    }
    const QFontMetricsF metrics(font);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const HudColumns columns = MeasureColumns(lines, metrics);
    const qreal width = std::max(GetGridWidth(columns), area.width());
    inoutPainter.save();
    inoutPainter.setFont(font);
    qreal top = area.top();
    for (const QString& line : lines)
    {
        const HudRow row = SplitRow(line);
        const QRectF rowRect(area.left(), top, width, metrics.lineSpacing());
        if (row.Label.isEmpty() == true)
        {
            inoutPainter.setPen(QColor::fromRgb(valueColour));
            inoutPainter.drawText(rowRect, Qt::AlignLeft | Qt::AlignVCenter, row.Value);
        }
        else
        {
            inoutPainter.setPen(QColor::fromRgb(labelColour));
            inoutPainter.drawText(rowRect, Qt::AlignLeft | Qt::AlignVCenter, row.Label);
            inoutPainter.setPen(QColor::fromRgb(valueColour));
            inoutPainter.drawText(rowRect, Qt::AlignRight | Qt::AlignVCenter, row.Value);
        }
        top += metrics.lineSpacing();
    }
    inoutPainter.restore();
}

HudGridLabel::HudGridLabel(QWidget* parent) : QLabel(parent)
{
    setContentsMargins(HudLabel::PADDING, HudLabel::PADDING, HudLabel::PADDING, HudLabel::PADDING);
    setFont(GetViewportHudFont());
    setTextFormat(Qt::PlainText);
}

QSize HudGridLabel::sizeHint() const
{
    const QSizeF grid = MeasureViewportHudGrid(text(), font());
    const QMargins margins = contentsMargins();
    return {static_cast<int>(std::ceil(grid.width())) + margins.left() + margins.right(),
            static_cast<int>(std::ceil(grid.height())) + margins.top() + margins.bottom()};
}

QSize HudGridLabel::minimumSizeHint() const
{
    return sizeHint();
}

void HudGridLabel::paintEvent(QPaintEvent* event)
{
    static_cast<void>(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromRgba(
        WithAlpha(GetViewportArgb(design::ThemeColour::SurfaceViewport), HudLabel::BACKGROUND_OPACITY)));
    painter.drawRoundedRect(rect(), design::RADIUS_GROUP, design::RADIUS_GROUP);
    PaintViewportHudGrid(painter,
                         contentsRect(),
                         text(),
                         font(),
                         GetViewportArgb(design::ThemeColour::TextHudQuiet),
                         GetViewportArgb(design::ThemeColour::TextHud));
}
} // namespace ludus::editor
