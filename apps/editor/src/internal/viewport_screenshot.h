#pragma once

#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

namespace ludus::editor
{
/// Default file name of a viewport screenshot taken at time, such as "Ludus 2026-11-02 at 14.03.07.png".
/// The name sorts by time and contains no characters that file systems reject.
[[nodiscard]] QString MakeViewportScreenshotFileName(const QDateTime& time);

/// Opaque copy of a viewport image composited over background, as the viewport shows a translucent frame.
/// @param image Displayed frame in any QImage format; premultiplied and straight alpha are both honoured.
/// @param background Colour the viewport paints behind its frame.
/// @return An RGB32 image of the same size; null when image is null.
[[nodiscard]] QImage MakeOpaqueViewportImage(const QImage& image, QRgb background);

/// How to read a GPU frame back from the Qt Quick item that draws it, at the frame's own resolution.
struct ViewportScreenshotGrab final
{
    /// Pixel size to render the whole item at, so the frame inside it covers its own texel count.
    QSize ItemPixels;
    /// Pixels of the frame inside an ItemPixels image; its size is the frame's.
    QRect Crop;
};

/// Scale an item drawing a (possibly letterboxed) frame so that the frame is read back one texel per pixel.
/// @param itemSize Logical size of the item.
/// @param imageRectangle Logical rectangle of the item the frame is drawn into.
/// @param imageSize Frame size in texels.
/// @param outGrab Receives the render size and the frame's crop; unchanged on failure.
/// @return False when a size is empty or the rectangle lies outside the item.
[[nodiscard]] bool ComputeViewportScreenshotGrab(const QSizeF& itemSize,
                                                 const QRectF& imageRectangle,
                                                 const QSize& imageSize,
                                                 ViewportScreenshotGrab& outGrab);

/// Logical size to pass to QQuickItem::grabToImage for grab, which renders that size times devicePixelRatio.
/// @param devicePixelRatio The window's effective device pixel ratio; non-positive values count as one.
[[nodiscard]] QSize GetViewportScreenshotGrabSize(const ViewportScreenshotGrab& grab, qreal devicePixelRatio);

/// Cut the frame out of a Qt Quick read-back of its item.
/// @param grabbed The read-back; when a fractional device pixel ratio made it differ from grab.ItemPixels, the crop
/// is mapped onto it and resampled to the frame size.
/// @return The frame at grab.Crop's size with a device pixel ratio of one; null when grabbed is null.
[[nodiscard]] QImage CropViewportScreenshot(const QImage& grabbed, const ViewportScreenshotGrab& grab);

/// Write image as a PNG file, replacing path only after the whole file was written.
/// @return False with outError naming path and the reason; an existing file at path is then unchanged.
[[nodiscard]] bool WriteViewportScreenshot(const QString& path, const QImage& image, QString& outError);
} // namespace ludus::editor
