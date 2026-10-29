#pragma once

#include "internal/editor_camera_controller.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <QPointF>
#include <QRectF>
#include <QRgb>

class QPainter;

namespace ludus::editor
{
/// Number of handles: the positive and negative end of each world axis.
inline constexpr foundation::usize ORIENTATION_HANDLE_COUNT = 6;
/// Side of the widget's square, in logical pixels.
inline constexpr qreal ORIENTATION_WIDGET_SIZE_PIXEL = 88;
/// Inset of the widget from the viewport's top-right corner, in logical pixels.
inline constexpr qreal ORIENTATION_WIDGET_MARGIN_PIXEL = 8;

/// One end of a world axis as the widget draws it. Coordinates are logical viewport pixels.
struct OrientationHandle final
{
    /// The editor view a click selects: looking at the scene from this end of the axis.
    EditorAxisView View = EditorAxisView::Front;
    QPointF CentrePixel;
    qreal RadiusPixel = 0;
    /// Component of the axis along the view direction; larger is farther from the camera.
    foundation::float32 Depth = 0;
    QRgb Colour = 0;
    /// "X", "Y" or "Z" on positive ends, so colour never names an axis alone; zero on negative ends.
    char Letter = 0;
    bool IsPositive = false;
};

/// The six handles for a camera, ordered back to front for painting.
struct OrientationLayout final
{
    foundation::StaticArray<OrientationHandle, ORIENTATION_HANDLE_COUNT> Handles;
    QPointF OriginPixel;
};

/// Square in the top-right corner of a viewport that is viewportWidthPixel wide.
[[nodiscard]] QRectF GetOrientationWidgetRect(qreal viewportWidthPixel) noexcept;

/// Project the world axes through the camera's orientation into rect. Orthographic and perspective
/// cameras draw alike: only the rotation matters.
[[nodiscard]] OrientationLayout BuildOrientationLayout(const gameplay::camera::CameraSample& sample,
                                                       const QRectF& rect) noexcept;

/// Front-most handle under pointPixel. Failure leaves outView unchanged.
[[nodiscard]] bool
TryHitOrientationHandle(const OrientationLayout& layout, QPointF pointPixel, EditorAxisView& outView) noexcept;

/// Paint the axes, a translucent disc behind them, and a ring on the handle of the current axis view
/// when the camera looks exactly along an axis.
void PaintOrientationWidget(QPainter& inoutPainter,
                            const OrientationLayout& layout,
                            const gameplay::camera::CameraSample& sample);
} // namespace ludus::editor
