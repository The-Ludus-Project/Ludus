#include "internal/viewport_screenshot.h"

#include <ludus/foundation/base/core.h>

#include <QPainter>
#include <QSaveFile>
#include <QtMath>

namespace ludus::editor
{
using namespace foundation;

QString MakeViewportScreenshotFileName(const QDateTime& time)
{
    return QStringLiteral("Ludus %1.png").arg(time.toString(QStringLiteral("yyyy-MM-dd 'at' HH.mm.ss")));
}

QImage MakeOpaqueViewportImage(const QImage& image, QRgb background)
{
    if (image.isNull() == true)
    {
        return {};
    }
    // Composite texel for texel: a read-back from a high-density window carries its device pixel ratio,
    // which QPainter would otherwise apply as a scale.
    QImage texels = image;
    texels.setDevicePixelRatio(1);
    QImage opaque(image.size(), QImage::Format_RGB32);
    opaque.fill(background);
    QPainter painter(&opaque);
    painter.drawImage(QPoint(0, 0), texels);
    painter.end();
    return opaque;
}

bool ComputeViewportScreenshotGrab(const QSizeF& itemSize,
                                   const QRectF& imageRectangle,
                                   const QSize& imageSize,
                                   ViewportScreenshotGrab& outGrab)
{
    if (itemSize.isEmpty() == true || imageRectangle.isEmpty() == true || imageSize.isEmpty() == true ||
        QRectF(QPointF(0, 0), itemSize).contains(imageRectangle) == false)
    {
        return false;
    }
    // The frame keeps its aspect inside the item, so one scale maps its rectangle onto its texels.
    const float64 scale = static_cast<float64>(imageSize.width()) / imageRectangle.width();
    const QSize itemPixels(qMax(imageSize.width(), qRound(itemSize.width() * scale)),
                           qMax(imageSize.height(), qRound(itemSize.height() * scale)));
    const QPoint origin(qBound(0, qRound(imageRectangle.x() * scale), itemPixels.width() - imageSize.width()),
                        qBound(0, qRound(imageRectangle.y() * scale), itemPixels.height() - imageSize.height()));
    outGrab = { .ItemPixels = itemPixels, .Crop = QRect(origin, imageSize) };
    return true;
}

QSize GetViewportScreenshotGrabSize(const ViewportScreenshotGrab& grab, qreal devicePixelRatio)
{
    const float64 ratio = devicePixelRatio > 0 ? static_cast<float64>(devicePixelRatio) : 1.0;
    return {qCeil(static_cast<float64>(grab.ItemPixels.width()) / ratio),
            qCeil(static_cast<float64>(grab.ItemPixels.height()) / ratio)};
}

QImage CropViewportScreenshot(const QImage& grabbed, const ViewportScreenshotGrab& grab)
{
    if (grabbed.isNull() == true || grab.ItemPixels.isEmpty() == true)
    {
        return {};
    }
    QImage texels = grabbed;
    texels.setDevicePixelRatio(1);
    if (texels.size() == grab.ItemPixels)
    {
        return texels.copy(grab.Crop);
    }
    const float64 scaleX = static_cast<float64>(texels.width()) / grab.ItemPixels.width();
    const float64 scaleY = static_cast<float64>(texels.height()) / grab.ItemPixels.height();
    const QRect mapped(qRound(grab.Crop.x() * scaleX),
                       qRound(grab.Crop.y() * scaleY),
                       qRound(grab.Crop.width() * scaleX),
                       qRound(grab.Crop.height() * scaleY));
    const QImage frame = texels.copy(mapped);
    return frame.size() == grab.Crop.size()
               ? frame
               : frame.scaled(grab.Crop.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

bool WriteViewportScreenshot(const QString& path, const QImage& image, QString& outError)
{
    if (image.isNull() == true)
    {
        outError = QStringLiteral("Could not write %1: there is no image to save.").arg(path);
        return false;
    }
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) == false || image.save(&file, "PNG") == false || file.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}
} // namespace ludus::editor
