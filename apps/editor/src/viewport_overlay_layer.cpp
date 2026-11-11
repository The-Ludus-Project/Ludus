#include "internal/viewport_overlay_layer.h"
#include "internal/viewport_gizmo_readout.h"
#include "internal/viewport_object_overlay.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QColor>
#include <QLineF>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QRgb>

namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Pixels antialiasing may tint beyond a shape's geometric edge.
constexpr float64 ANTIALIAS_MARGIN_PIXELS = 1;
/// Bytes of one straight-alpha RGBA8 pixel.
constexpr usize RGBA_BYTES = 4;
constexpr usize GREEN_BYTE = 1;
constexpr usize BLUE_BYTE = 2;
constexpr usize ALPHA_BYTE = 3;

/// Bounds of a stroked segment. A square cap reaches half the width past each end, and diagonally at most the
/// half width times the square root of two, which the full width covers.
QRectF GetStrokeBounds(const QLineF& linePixel, float64 widthPixels)
{
    const float64 padding = widthPixels + ANTIALIAS_MARGIN_PIXELS;
    return QRectF(linePixel.p1(), linePixel.p2()).normalized().adjusted(-padding, -padding, padding, padding);
}
} // namespace

void PaintViewportObjectOverlay(QPainter& inoutPainter, const ViewportObjectOverlay& overlay)
{
    inoutPainter.save();
    inoutPainter.setClipRect(overlay.FramePixel);
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    // The same casing and amber strokes as the native overlay, with round joins and caps.
    QPen casingPen(QColor::fromRgba(ViewportObjectOverlay::CASING_COLOR),
                   static_cast<float64>(ViewportObjectOverlay::CASING_WIDTH_PIXELS));
    casingPen.setCapStyle(Qt::RoundCap);
    inoutPainter.setPen(casingPen);
    inoutPainter.drawLines(overlay.OutlineLinesPixel.GetData(),
                           static_cast<int32>(overlay.OutlineLinesPixel.GetSize()));
    QPen outlinePen(QColor::fromRgb(ViewportObjectOverlay::SELECTION_COLOR),
                    static_cast<float64>(ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS));
    outlinePen.setCapStyle(Qt::RoundCap);
    inoutPainter.setPen(outlinePen);
    inoutPainter.drawLines(overlay.OutlineLinesPixel.GetData(),
                           static_cast<int32>(overlay.OutlineLinesPixel.GetSize()));
    for (const ViewportHandleStroke& stroke : overlay.HandleStrokes)
    {
        inoutPainter.setPen(QPen(QColor::fromRgb(stroke.Color), static_cast<float64>(stroke.WidthPixels)));
        inoutPainter.drawLine(stroke.LinePixel);
    }
    inoutPainter.setPen(Qt::NoPen);
    for (const ViewportHandleFill& fill : overlay.HandleFills)
    {
        inoutPainter.setBrush(QColor::fromRgb(fill.Color));
        inoutPainter.drawPolygon(fill.CornersPixel.GetData(), static_cast<int32>(fill.CornerCount));
    }
    if (overlay.ReadoutText.isEmpty() == false)
    {
        PaintGizmoReadout(inoutPainter,
                          overlay.ReadoutText,
                          PlaceGizmoReadout(overlay.ReadoutAnchorPixel,
                                            MeasureGizmoReadout(overlay.ReadoutText),
                                            overlay.FramePixel));
    }
    inoutPainter.restore();
}

QRectF ComputeViewportObjectOverlayBounds(const ViewportObjectOverlay& overlay)
{
    QRectF boundsPixel;
    const float64 casingWidthPixels = static_cast<float64>(ViewportObjectOverlay::CASING_WIDTH_PIXELS);
    for (const QLineF& linePixel : overlay.OutlineLinesPixel)
    {
        boundsPixel |= GetStrokeBounds(linePixel, casingWidthPixels);
    }
    for (const ViewportHandleStroke& stroke : overlay.HandleStrokes)
    {
        boundsPixel |= GetStrokeBounds(stroke.LinePixel, static_cast<float64>(stroke.WidthPixels));
    }
    for (const ViewportHandleFill& fill : overlay.HandleFills)
    {
        const usize cornerCount = math::Min(fill.CornerCount, ViewportHandleFill::MAX_CORNERS);
        QPolygonF polygonPixel;
        polygonPixel.reserve(static_cast<qsizetype>(cornerCount));
        for (usize i = 0; i < cornerCount; ++i)
        {
            polygonPixel.append(fill.CornersPixel[i]);
        }
        boundsPixel |= polygonPixel.boundingRect().adjusted(-ANTIALIAS_MARGIN_PIXELS,
                                                            -ANTIALIAS_MARGIN_PIXELS,
                                                            ANTIALIAS_MARGIN_PIXELS,
                                                            ANTIALIAS_MARGIN_PIXELS);
    }
    if (overlay.ReadoutText.isEmpty() == false)
    {
        const QSizeF sizePixel = MeasureGizmoReadout(overlay.ReadoutText);
        const QRectF labelPixel(PlaceGizmoReadout(overlay.ReadoutAnchorPixel, sizePixel, overlay.FramePixel),
                                sizePixel);
        boundsPixel |= labelPixel.adjusted(-ANTIALIAS_MARGIN_PIXELS,
                                           -ANTIALIAS_MARGIN_PIXELS,
                                           ANTIALIAS_MARGIN_PIXELS,
                                           ANTIALIAS_MARGIN_PIXELS);
    }
    return boundsPixel.intersected(overlay.FramePixel);
}

bool ViewportOverlayLayer::Update(const ViewportObjectOverlay& overlay, QSize layerSizePhysical, float64 pixelRatio)
{
    mClearedRegion = mPaintedRegion;
    mPaintedRegion = QRect();
    const QRectF boundsPixel = ComputeViewportObjectOverlayBounds(overlay);
    if (boundsPixel.isEmpty() == false && pixelRatio > 0)
    {
        const QRectF boundsPhysical(boundsPixel.topLeft() * pixelRatio, boundsPixel.size() * pixelRatio);
        mPaintedRegion = boundsPhysical.toAlignedRect().intersected(QRect(QPoint(), layerSizePhysical));
    }
    if (mPaintedRegion.isEmpty() == true)
    {
        mPaintedRegion = QRect();
        mPixels.Clear();
        return mClearedRegion.isEmpty() == false;
    }
    const int32 width = mPaintedRegion.width();
    const int32 height = mPaintedRegion.height();
    if (mCanvas.width() < width || mCanvas.height() < height)
    {
        mCanvas = QImage(math::Max(mCanvas.width(), width),
                         math::Max(mCanvas.height(), height),
                         QImage::Format_ARGB32_Premultiplied);
    }
    const usize pixelBytes = static_cast<usize>(width) * static_cast<usize>(height) * RGBA_BYTES;
    if (mCanvas.isNull() == true || mPixels.TryResize(pixelBytes) == false)
    {
        // Out of memory: show nothing rather than stale pixels; the next update tries again.
        mCanvas = QImage();
        mPaintedRegion = QRect();
        mPixels.Clear();
        return mClearedRegion.isEmpty() == false;
    }
    mCanvas.setDevicePixelRatio(pixelRatio);
    {
        QPainter painter(&mCanvas);
        const QRectF regionPixel(0,
                                 0,
                                 static_cast<float64>(width) / pixelRatio,
                                 static_cast<float64>(height) / pixelRatio);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(regionPixel, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.translate(-QPointF(mPaintedRegion.topLeft()) / pixelRatio);
        PaintViewportObjectOverlay(painter, overlay);
    }
    // Canvas image data is straight alpha in R, G, B, A byte order.
    uint8* destination = mPixels.GetData();
    for (int32 y = 0; y < height; ++y)
    {
        const QRgb* row = reinterpret_cast<const QRgb*>(mCanvas.constScanLine(y));
        for (int32 x = 0; x < width; ++x)
        {
            const QRgb premultiplied = row[x];
            const QRgb straight = qAlpha(premultiplied) == 0 ? 0 : qUnpremultiply(premultiplied);
            destination[0] = static_cast<uint8>(qRed(straight));
            destination[GREEN_BYTE] = static_cast<uint8>(qGreen(straight));
            destination[BLUE_BYTE] = static_cast<uint8>(qBlue(straight));
            destination[ALPHA_BYTE] = static_cast<uint8>(qAlpha(straight));
            destination += RGBA_BYTES;
        }
    }
    return true;
}

void ViewportOverlayLayer::Invalidate() noexcept
{
    mClearedRegion = QRect();
    mPaintedRegion = QRect();
    mPixels.Clear();
}

QRect ViewportOverlayLayer::GetClearedRegion() const noexcept
{
    return mClearedRegion;
}

QRect ViewportOverlayLayer::GetPaintedRegion() const noexcept
{
    return mPaintedRegion;
}

const uint8* ViewportOverlayLayer::GetPixels() const noexcept
{
    return mPixels.GetData();
}

usize ViewportOverlayLayer::GetPixelBytes() const noexcept
{
    return mPixels.GetSize();
}
} // namespace ludus::editor
