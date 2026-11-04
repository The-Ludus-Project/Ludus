#pragma once

#include <ludus/foundation/base/types.h>

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

class QPainter;

namespace ludus::editor
{
/// Marks a readout component that is not shown: the second component of a one-axis move, or the axis of a uniform
/// scale.
inline constexpr foundation::int32 GIZMO_READOUT_NO_AXIS = -1;

/// One axis of a move: the axis (0 for X) and the distance moved along it, in metres.
struct GizmoReadoutComponent final
{
    foundation::int32 Axis = GIZMO_READOUT_NO_AXIS;
    foundation::float64 Value = 0;
};

/// The distance moved, such as "X +0.250 m", or "X +0.250  Z -0.120 m" for a plane. A second component whose axis is
/// GIZMO_READOUT_NO_AXIS is left out. Distances are rounded to a millimetre.
[[nodiscard]] QString FormatMoveReadout(const GizmoReadoutComponent& first, const GizmoReadoutComponent& second);
/// The angle turned about an axis, such as "Z +15.0°", rounded to a tenth of a degree.
[[nodiscard]] QString FormatRotateReadout(foundation::int32 axis, foundation::float64 degrees);
/// The factor an axis was scaled by, such as "X ×1.25", or "×1.25" for every axis together when axis is
/// GIZMO_READOUT_NO_AXIS. Factors are rounded to a hundredth.
[[nodiscard]] QString FormatScaleReadout(foundation::int32 axis, foundation::float64 factor);

/// Size of the label that shows text, in logical pixels. Empty text has no label.
[[nodiscard]] QSizeF MeasureGizmoReadout(const QString& text);
/// Top-left of a label of the given size beside the anchor, on the side of the anchor that keeps it inside the frame.
[[nodiscard]] QPointF PlaceGizmoReadout(QPointF anchorPixel, QSizeF sizePixel, const QRectF& framePixel);
/// Paint the label with its top-left at originPixel, using the viewport colours and the caption type of the design
/// tokens.
void PaintGizmoReadout(QPainter& inoutPainter, const QString& text, QPointF originPixel);
} // namespace ludus::editor
