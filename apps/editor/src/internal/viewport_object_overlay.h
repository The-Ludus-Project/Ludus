#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>

#include "design_tokens.h"

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
/// GUI-owned projected selection data (the object's silhouette and its transform handles), independent of a renderer
/// device or painter. Buffers retain their capacity across camera/gesture updates; consumers borrow until the next
/// build.
struct ViewportObjectOverlay final
{
    /// Bound CPU projection and GPU upload work even for a dense imported mesh. The outline of a selection is
    /// its silhouette, far shorter than its tessellation, so the bound is reached only by pathological meshes.
    static constexpr foundation::usize MAX_OUTLINE_LINES = 12000;
    static constexpr foundation::usize TRIANGLE_VERTICES = 3;
    static constexpr foundation::int32 AXIS_COUNT = 3;
    static constexpr foundation::int32 RING_SEGMENTS = 64;
    static constexpr foundation::int32 TIP_SEGMENTS = 16;
    static constexpr foundation::usize MAX_GLYPH_STROKES = 3;
    static constexpr foundation::usize MAX_HANDLE_STROKES =
        AXIS_COUNT * (RING_SEGMENTS + TIP_SEGMENTS + MAX_GLYPH_STROKES);
    /// The selection amber of the design tokens, shared with the graph canvas.
    static constexpr QRgb SELECTION_COLOR = design::GetThemeArgb(design::ThemeColour::SelectionAmber, true);
    /// Opacity, of 255, of the casing under the outline.
    static constexpr foundation::int32 CASING_ALPHA = 150;
    /// The translucent viewport surface colour drawn under the outline so it reads on bright and dark pixels alike.
    static constexpr QRgb CASING_COLOR = qRgba(qRed(design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true)),
                                               qGreen(design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true)),
                                               qBlue(design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true)),
                                               CASING_ALPHA);
    static constexpr foundation::float32 OUTLINE_WIDTH_PIXELS = 2.0F;
    /// Casing width: the outline plus one pixel of contrast on each side.
    static constexpr foundation::float32 CASING_WIDTH_PIXELS = OUTLINE_WIDTH_PIXELS + 2.0F;
    foundation::Array<QLineF> OutlineLinesPixel;
    foundation::Array<ViewportHandleStroke> HandleStrokes;
    QRectF FramePixel;
};
} // namespace ludus::editor
