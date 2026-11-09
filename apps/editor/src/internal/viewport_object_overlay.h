#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include "design_tokens.h"

#include <QColor>
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace ludus::editor
{
/// A projected handle stroke. Coordinates and width are logical viewport pixels.
struct ViewportHandleStroke final
{
    QLineF LinePixel;
    QRgb Color = 0;
    foundation::float32 WidthPixels = 1;
};
/// A filled, convex handle polygon in logical viewport pixels: a triangle (arrowhead) or a quad (cube
/// face, pivot marker). Corners are unused beyond CornerCount.
struct ViewportHandleFill final
{
    static constexpr foundation::usize TRIANGLE_CORNERS = 3;
    static constexpr foundation::usize MAX_CORNERS = 4;
    using Corners = foundation::StaticArray<QPointF, MAX_CORNERS>;
    Corners CornersPixel{};
    foundation::usize CornerCount = TRIANGLE_CORNERS;
    QRgb Color = 0;
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
    static constexpr foundation::usize MAX_GLYPH_STROKES = 3;
    static constexpr foundation::usize MAX_HANDLE_STROKES = AXIS_COUNT * (RING_SEGMENTS + MAX_GLYPH_STROKES);
    /// A plane handle or the centre handle is a dark casing and the solid shape over it.
    static constexpr foundation::usize FILLS_PER_CASED_HANDLE = 2;
    /// Arrowheads and cubes of the three axes, the three plane handles with their casings, and the pivot marker.
    static constexpr foundation::usize MAX_TRANSFORM_HANDLE_FILLS =
        AXIS_COUNT + AXIS_COUNT * FILLS_PER_CASED_HANDLE + 1;
    static constexpr foundation::usize TERRAIN_EXTENT_HANDLES = 2;
    static constexpr foundation::int32 PROBE_FACE_HANDLES = 2 * AXIS_COUNT;
    static constexpr foundation::usize MAX_HANDLE_FILLS =
        MAX_TRANSFORM_HANDLE_FILLS + TERRAIN_EXTENT_HANDLES + PROBE_FACE_HANDLES * FILLS_PER_CASED_HANDLE;
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
    /// Solid handle shapes drawn over the strokes.
    foundation::Array<ViewportHandleFill> HandleFills;
    QRectF FramePixel;
    /// Number shown beside the pointer while a handle is dragged; empty otherwise.
    QString ReadoutText;
    /// Pointer position the readout is placed beside, in the same logical pixels as the strokes.
    QPointF ReadoutAnchorPixel;
};
} // namespace ludus::editor
