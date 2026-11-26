#pragma once

// Statistics over the viewport drawn as a two-column grid: each "label: value" line of the overlay text
// (VIEWPORT_STATISTICS_VALUE_SEPARATOR) becomes a quiet label and a right-aligned value in body type with
// tabular figures, so numbers line up without a monospace font. A line without the separator spans both
// columns. Every row is one line spacing tall, so pass bars can be drawn behind rows by index. GUI or
// scene-graph render thread; nothing here allocates per frame beyond the text it lays out.

#include "design_tokens.h"

#include <QFont>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QRectF>
#include <QRgb>
#include <QSizeF>
#include <QString>

namespace ludus::editor
{
/// Body type with tabular figures, the font of the viewport's statistics and messages.
[[nodiscard]] QFont GetViewportHudFont();

/// Caption-scaled HUD font used by the statistics overlay so the panel stays compact.
[[nodiscard]] QFont GetViewportStatisticsFont();

/// Opacity, of 255, of the statistics overlay panel. Quieter than a message chip so the scene stays visible.
inline constexpr int VIEWPORT_STATISTICS_BACKGROUND_OPACITY = 120;

/// Padding between the statistics panel edge and its text, in logical pixels.
inline constexpr int VIEWPORT_STATISTICS_PADDING = design::SPACING_EXTRA_SMALL;

/// Prefix compactText with a disclosure mark, or replace it with the collapsed one-line chip.
[[nodiscard]] QString MakeStatisticsOverlayText(const QString& compactText, const QString& summary);

/// The overlay's whole tooltip: what a click does now. Short on purpose, so hovering never covers the frame.
[[nodiscard]] QString GetStatisticsOverlayHint();

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

    /// compactText is the mean-times grid and summary is the collapsed chip. The label paints the grid or the
    /// chip, and a click toggles that state. The tooltip is only the click hint: the grid already is the
    /// detail, so a second copy of it would cover the picture.
    void SetStatisticsText(const QString& compactText, const QString& summary);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void applyStatisticsDisplay();
    QString mCompactText;
    QString mSummary;
};
} // namespace ludus::editor
