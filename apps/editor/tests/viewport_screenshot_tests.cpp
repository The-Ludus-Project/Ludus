#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/viewport_screenshot.h"

#include <ludus/foundation/base/core.h>

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTime>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
/// Background of the composited test images.
constexpr QRgb BACKGROUND = 0xff204060;
} // namespace

TEST_CASE("Viewport screenshot names sort by time and avoid reserved characters", "[editor][viewport][screenshot]")
{
    const QDateTime time(QDate(2026, 11, 2), QTime(14, 3, 7));
    const QString name = MakeViewportScreenshotFileName(time);
    CHECK(name == QStringLiteral("Ludus 2026-11-02 at 14.03.07.png"));
    for (const QChar reserved : QStringLiteral("/\\:*?\"<>|"))
    {
        CHECK(name.contains(reserved) == false);
    }
}

TEST_CASE("Viewport screenshots are opaque and keep their texels", "[editor][viewport][screenshot]")
{
    QImage translucent(4, 2, QImage::Format_ARGB32_Premultiplied);
    translucent.fill(Qt::transparent);
    translucent.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    translucent.setPixelColor(1, 0, QColor(0, 255, 0, 128));
    // A read-back from a Retina window carries its device pixel ratio; it must not shrink the copy.
    translucent.setDevicePixelRatio(2);
    const QImage opaque = MakeOpaqueViewportImage(translucent, BACKGROUND);
    REQUIRE(opaque.size() == translucent.size());
    CHECK(opaque.hasAlphaChannel() == false);
    CHECK(opaque.pixel(0, 0) == qRgb(255, 0, 0));
    CHECK(opaque.pixel(3, 1) == BACKGROUND);
    const QColor blended = opaque.pixelColor(1, 0);
    CHECK(qAbs(blended.red() - 0x10) <= 1);
    CHECK(qAbs(blended.green() - 0xa0) <= 1);
    CHECK(qAbs(blended.blue() - 0x30) <= 1);
    CHECK(MakeOpaqueViewportImage({}, BACKGROUND).isNull() == true);
}

TEST_CASE("Viewport screenshot grabs read a frame back one texel per pixel", "[editor][viewport][screenshot]")
{
    ViewportScreenshotGrab grab;
    SECTION("A frame filling the item at twice its logical size")
    {
        REQUIRE(ComputeViewportScreenshotGrab(QSizeF(320, 240), QRectF(0, 0, 320, 240), QSize(640, 480), grab) == true);
        CHECK(grab.ItemPixels == QSize(640, 480));
        CHECK(grab.Crop == QRect(0, 0, 640, 480));
    }
    SECTION("A project graph frame letterboxed inside a wider item")
    {
        // A 4:3 frame at 1600 x 1200 shown in a 400 x 200 item occupies the centred 266.67 x 200 rectangle.
        const QRectF frame(66.6667, 0, 266.6667, 200);
        REQUIRE(ComputeViewportScreenshotGrab(QSizeF(400, 200), frame, QSize(1600, 1200), grab) == true);
        CHECK(grab.ItemPixels == QSize(2400, 1200));
        CHECK(grab.Crop == QRect(400, 0, 1600, 1200));
    }
    SECTION("Rounding never moves the crop outside the item")
    {
        REQUIRE(ComputeViewportScreenshotGrab(QSizeF(101, 75), QRectF(0, 0, 101, 75), QSize(1024, 761), grab) == true);
        CHECK(QRect(QPoint(0, 0), grab.ItemPixels).contains(grab.Crop) == true);
        CHECK(grab.Crop.size() == QSize(1024, 761));
    }
    SECTION("Empty or outside rectangles are rejected and leave the output unchanged")
    {
        grab = { .ItemPixels = QSize(1, 2), .Crop = QRect(3, 4, 5, 6) };
        CHECK(ComputeViewportScreenshotGrab(QSizeF(0, 240), QRectF(0, 0, 320, 240), QSize(640, 480), grab) == false);
        CHECK(ComputeViewportScreenshotGrab(QSizeF(320, 240), QRectF(), QSize(640, 480), grab) == false);
        CHECK(ComputeViewportScreenshotGrab(QSizeF(320, 240), QRectF(10, 0, 320, 240), QSize(640, 480), grab) == false);
        CHECK(ComputeViewportScreenshotGrab(QSizeF(320, 240), QRectF(0, 0, 320, 240), QSize(), grab) == false);
        CHECK(grab.ItemPixels == QSize(1, 2));
        CHECK(grab.Crop == QRect(3, 4, 5, 6));
    }
}

TEST_CASE("Viewport screenshot read-backs account for the window's device pixel ratio",
          "[editor][viewport][screenshot]")
{
    const ViewportScreenshotGrab grab = { .ItemPixels = QSize(8, 4), .Crop = QRect(2, 0, 4, 4) };
    // Qt Quick renders a grab at its logical target size times the ratio, so a Retina window asks for half.
    CHECK(GetViewportScreenshotGrabSize(grab, 2) == QSize(4, 2));
    CHECK(GetViewportScreenshotGrabSize(grab, 1) == QSize(8, 4));
    CHECK(GetViewportScreenshotGrabSize(grab, 0) == QSize(8, 4));
    CHECK(GetViewportScreenshotGrabSize(grab, 1.5) == QSize(6, 3));

    QImage item(grab.ItemPixels, QImage::Format_RGB32);
    item.fill(BACKGROUND);
    for (int32 y = 0; y < item.height(); ++y)
    {
        for (int32 x = 2; x < 6; ++x)
        {
            item.setPixel(x, y, qRgb(x * 40, y * 40, 200));
        }
    }
    item.setDevicePixelRatio(2);
    SECTION("A read-back at the requested pixels is cropped texel for texel")
    {
        const QImage frame = CropViewportScreenshot(item, grab);
        REQUIRE(frame.size() == QSize(4, 4));
        CHECK(frame.devicePixelRatio() == 1);
        CHECK(frame.pixel(0, 0) == qRgb(80, 0, 200));
        CHECK(frame.pixel(3, 3) == qRgb(200, 120, 200));
    }
    SECTION("A larger read-back from a fractional ratio is mapped and resampled to the frame size")
    {
        const QImage larger = item.scaled(QSize(16, 8));
        const QImage frame = CropViewportScreenshot(larger, grab);
        REQUIRE(frame.size() == QSize(4, 4));
        CHECK(frame.pixel(0, 0) == qRgb(80, 0, 200));
    }
    CHECK(CropViewportScreenshot({}, grab).isNull() == true);
}

TEST_CASE("Viewport screenshots are written as PNG files or explain why not", "[editor][viewport][screenshot]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(BACKGROUND);
    image.setPixel(2, 1, qRgb(1, 2, 3));
    const QString path = QDir(directory.path()).filePath(QStringLiteral("shot.png"));
    QString error;
    REQUIRE(WriteViewportScreenshot(path, image, error) == true);
    CHECK(error.isEmpty() == true);
    QImage written;
    REQUIRE(written.load(path, "PNG") == true);
    CHECK(written.convertToFormat(QImage::Format_RGB32) == image);

    const QString missingDirectory = QDir(directory.path()).filePath(QStringLiteral("missing/shot.png"));
    CHECK(WriteViewportScreenshot(missingDirectory, image, error) == false);
    CHECK(error.contains(missingDirectory) == true);
    CHECK(QFile::exists(missingDirectory) == false);
    error.clear();
    CHECK(WriteViewportScreenshot(path, QImage(), error) == false);
    CHECK(error.isEmpty() == false);
    // A refused write keeps the earlier file.
    REQUIRE(written.load(path, "PNG") == true);
    CHECK(written.convertToFormat(QImage::Format_RGB32) == image);
}

TEST_CASE("A viewport without a rendered frame refuses a screenshot and writes nothing",
          "[editor][viewport][screenshot]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    const QString path = QDir(directory.path()).filePath(QStringLiteral("shot.png"));
    QString error;
    CHECK(viewport.SaveScreenshot(path, error) == false);
    CHECK(error.isEmpty() == false);
    CHECK(viewport.IsTakingScreenshot() == false);
    CHECK(viewport.GetScreenshotPath().isEmpty() == true);
    CHECK(QFile::exists(path) == false);
}
