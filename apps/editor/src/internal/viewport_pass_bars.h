#pragma once

#include "internal/viewport_statistics.h"

#include <QPainter>
#include <QRectF>

namespace ludus::editor
{
/// Paint the bars behind the pass rows of the statistics overlay, before its text. textArea is the
/// text's rectangle (panel padding excluded), lineHeight the font's line spacing and firstLine the
/// text line of the first barred row. A bar is as wide as textArea in proportion to its pass's share of
/// the slowest pass; bars use the accent token, and the slowest uses the warning token when
/// bars.IsSlowestWarning says it dominates.
void PaintViewportPassBars(QPainter& inoutPainter,
                           const QRectF& textArea,
                           qreal lineHeight,
                           qsizetype firstLine,
                           const ViewportPassBars& bars);
} // namespace ludus::editor
