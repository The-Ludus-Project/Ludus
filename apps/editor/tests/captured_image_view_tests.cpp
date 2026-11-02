// Captured image views: right-click copy and save of the full-resolution image behind a scaled preview.

#include "internal/captured_image_view.h"
#include "internal/reference_capture_window.h"

#include <catch2/catch_test_macros.hpp>

#include <ludus/foundation/base/types.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

using namespace ludus::editor;
using ludus::foundation::int32;

namespace
{
/// Larger than the view so a copy or save of the scaled pixmap would be caught.
constexpr int32 IMAGE_WIDTH = 640;
constexpr int32 IMAGE_HEIGHT = 424;
constexpr int32 VIEW_EXTENT = 200;

QImage MakeGradientImage()
{
    QImage image(IMAGE_WIDTH, IMAGE_HEIGHT, QImage::Format_RGB32);
    for (int32 y = 0; y < IMAGE_HEIGHT; ++y)
    {
        for (int32 x = 0; x < IMAGE_WIDTH; ++x)
        {
            image.setPixelColor(x, y, QColor(x % 256, y % 256, (x + y) % 256));
        }
    }
    return image;
}
} // namespace

TEST_CASE("Captured image view offers copy and save only with an image", "[editor][reference-capture]")
{
    CapturedImageView view(QStringLiteral("reference"));
    QAction* copy = view.findChild<QAction*>(QStringLiteral("capturedImageCopy"));
    QAction* save = view.findChild<QAction*>(QStringLiteral("capturedImageSave"));
    REQUIRE(copy != nullptr);
    REQUIRE(save != nullptr);
    CHECK(copy->isEnabled() == false);
    CHECK(save->isEnabled() == false);
    CHECK(view.CopyImage() == false);
    QString error;
    CHECK(view.SaveImage(QStringLiteral("unused.png"), error) == false);
    CHECK(error.isEmpty() == false);

    view.SetImage(MakeGradientImage());
    CHECK(copy->isEnabled() == true);
    CHECK(save->isEnabled() == true);
    view.SetImage({});
    CHECK(copy->isEnabled() == false);
    CHECK(save->isEnabled() == false);
}

TEST_CASE("Captured image view copies and saves the full-resolution image", "[editor][reference-capture]")
{
    const QImage image = MakeGradientImage();
    CapturedImageView view(QStringLiteral("reference"));
    view.resize(VIEW_EXTENT, VIEW_EXTENT);
    view.SetImage(image);

    QApplication::clipboard()->clear();
    view.findChild<QAction*>(QStringLiteral("capturedImageCopy"))->trigger();
    const QImage copied = QApplication::clipboard()->image();
    CHECK(copied.size() == image.size());
    CHECK(copied.convertToFormat(QImage::Format_RGB32) == image);

    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    const QString path = QDir(root.path()).filePath(QStringLiteral("reference.png"));
    QString error;
    REQUIRE(view.SaveImage(path, error) == true);
    const QImage saved(path);
    CHECK(saved.convertToFormat(QImage::Format_RGB32) == image);

    // A failed write reports the path and leaves no partial file behind.
    const QString missing = QDir(root.path()).filePath(QStringLiteral("missing/reference.png"));
    CHECK(view.SaveImage(missing, error) == false);
    CHECK(error.contains(missing) == true);
    CHECK(QFile::exists(missing) == false);
}

TEST_CASE("Reference capture window shows each image tab in a captured image view", "[editor][reference-capture]")
{
    ReferenceCaptureWindow window(nullptr);
    for (const QString& name : {QStringLiteral("referenceImage"),
                                QStringLiteral("referenceRasterImage"),
                                QStringLiteral("referenceDifferenceImage")})
    {
        CapturedImageView* view = window.findChild<CapturedImageView*>(name);
        REQUIRE(view != nullptr);
        CHECK(view->contextMenuPolicy() == Qt::DefaultContextMenu);
        // Nothing has been captured, so there is nothing to copy yet.
        CHECK(view->GetImage().isNull() == true);
        CHECK(view->findChild<QAction*>(QStringLiteral("capturedImageCopy"))->isEnabled() == false);
    }
}
