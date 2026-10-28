#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>

#include <QColor>
#include <QLineF>
#include <QRectF>

namespace ludus::editor
{
/// A projected handle stroke. Coordinates and width are logical viewport pixels.
struct ViewportHandleStroke final
{
    QLineF LinePixel;
    QRgb Color = 0;
    foundation::float32 WidthPixels = 1;
};
/// GUI-owned projected selection data, independent of a renderer device or painter.
/// Buffers retain their capacity across camera/gesture updates; consumers borrow until the next build.
struct ViewportObjectOverlay final
{
    /// Bound CPU projection and GPU upload work even for a dense imported mesh.
    static constexpr foundation::usize MAX_WIRE_TRIANGLES = 12000;
    static constexpr foundation::usize TRIANGLE_VERTICES = 3;
    static constexpr foundation::usize MAX_WIRE_LINES = MAX_WIRE_TRIANGLES * TRIANGLE_VERTICES;
    static constexpr foundation::int32 AXIS_COUNT = 3;
    static constexpr foundation::int32 RING_SEGMENTS = 64;
    static constexpr foundation::int32 TIP_SEGMENTS = 16;
    static constexpr foundation::usize MAX_GLYPH_STROKES = 3;
    static constexpr foundation::usize MAX_HANDLE_STROKES =
        AXIS_COUNT * (RING_SEGMENTS + TIP_SEGMENTS + MAX_GLYPH_STROKES);
    static constexpr QRgb SELECTION_COLOR = 0xffffbd59;
    static constexpr foundation::float32 WIRE_WIDTH_PIXELS = 1.5F;
    foundation::Array<QLineF> WireLinesPixel;
    foundation::Array<ViewportHandleStroke> HandleStrokes;
    QRectF FramePixel;
};
} // namespace ludus::editor
