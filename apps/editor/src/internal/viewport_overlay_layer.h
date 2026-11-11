#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>

class QPainter;

namespace ludus::editor
{
struct ViewportObjectOverlay;

/// Paint the selection outline, transform handles and drag readout in the overlay's logical pixels, clipped to its
/// frame: the casing under the amber outline, then handle strokes, then handle fills, then the readout label. Every
/// painter-based presentation (the offscreen readback canvas and the browser overlay layer) shares this order.
void PaintViewportObjectOverlay(QPainter& inoutPainter, const ViewportObjectOverlay& overlay);
/// Logical-pixel rectangle that PaintViewportObjectOverlay can change: every outline, stroke, fill and the readout
/// label, padded by their stroke widths and antialiasing, and clipped to the frame. Empty when nothing is drawn.
[[nodiscard]] QRectF ComputeViewportObjectOverlayBounds(const ViewportObjectOverlay& overlay);

/// A transparent layer of physical pixels that shows the object overlay over a viewport image presented outside Qt,
/// such as the browser editor's GPU canvas, which covers Qt's own canvas so Qt cannot draw over it directly.
///
/// Each Update rasterizes only the region the overlay covers, and reports the region painted last time so the owner
/// clears it first. Pixels are straight-alpha RGBA8, rows packed without padding, ready for a 2D canvas's
/// putImageData. Buffers keep their capacity between updates. GUI thread only.
class ViewportOverlayLayer final
{
public:
    /// Rasterize overlay for a layer of layerSizePhysical pixels, where one logical pixel of overlay is pixelRatio
    /// physical pixels. Returns false when the layer's pixels stay as they are: nothing was drawn before and nothing
    /// is drawn now. When true, clear GetClearedRegion, then copy GetPixels into GetPaintedRegion.
    [[nodiscard]] bool
    Update(const ViewportObjectOverlay& overlay, QSize layerSizePhysical, foundation::float64 pixelRatio);
    /// Forget what the layer shows, for example after its owner resized or recreated the surface, which clears it.
    void Invalidate() noexcept;
    /// Physical-pixel region painted by the previous Update, which the owner clears before copying the new one.
    [[nodiscard]] QRect GetClearedRegion() const noexcept;
    /// Physical-pixel region the pixels of the last Update cover; empty when nothing is drawn.
    [[nodiscard]] QRect GetPaintedRegion() const noexcept;
    /// GetPaintedRegion().width() * GetPaintedRegion().height() straight-alpha RGBA8 pixels, row by row.
    [[nodiscard]] const foundation::uint8* GetPixels() const noexcept;
    /// Bytes in GetPixels.
    [[nodiscard]] foundation::usize GetPixelBytes() const noexcept;

private:
    /// Premultiplied painting surface, grown to the largest region painted so far and reused.
    QImage mCanvas;
    foundation::Array<foundation::uint8> mPixels;
    QRect mClearedRegion;
    QRect mPaintedRegion;
};
} // namespace ludus::editor
