#pragma once

// Statistics over the viewport drawn as a two-column grid: each "label: value" line of the overlay text
// (VIEWPORT_STATISTICS_VALUE_SEPARATOR) becomes a quiet label and a right-aligned value in body type with
// tabular figures, so numbers line up without a monospace font. A line without the separator spans both
// columns. Every row is one line spacing tall, so pass bars can be drawn behind rows by index. GUI or
// scene-graph render thread; nothing here allocates per frame beyond the text it lays out.

#include <QFont>
#include <QLabel>
#include <QPainter>
#include <QRectF>
#include <QRgb>
#include <QSizeF>
#include <QString>

namespace ludus::editor
{
/// Body type with tabular figures, the font of the viewport's statistics and messages.
[[nodiscard]] QFont GetViewportHudFont();

/// Space between the label and value columns, in logical pixels.
[[nodiscard]] qreal GetViewportHudColumnGap();

/// Size of text laid out as the grid in font, without padding; empty text is empty.
[[nodiscard]] QSizeF MeasureViewportHudGrid(const QString& text, const QFont& font);

/// Draw text as the grid at the top-left of area: labels in labelColour, values and spanning lines in valueColour.
void PaintViewportHudGrid(QPainter& inoutPainter,
                          const QRectF& area,
                          const QString& text,
                          const QFont& font,
                          QRgb labelColour,
                          QRgb valueColour);

/// A HUD label (see HudLabel in overlay_widgets.h) whose plain text is drawn as the grid. GUI thread only.
class HudGridLabel final : public QLabel
{
public:
    explicit HudGridLabel(QWidget* parent);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};
} // namespace ludus::editor
