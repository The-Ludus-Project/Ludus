// Editor icon system: the shipped Lucide files parse, draw in the palette, and rasterise into one bounded cache.

#include "internal/editor_icons.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QRectF>
#include <QSize>

#include <cmath>

using namespace ludus::editor;
using ludus::foundation::usize;

namespace
{
constexpr qreal TOLERANCE = 0.01;
constexpr qreal HALF_CIRCLE_RADIUS = 12.0;
constexpr qreal SQUARE_SIDE = 10.0;
/// Overshoot allowed beyond the grid by curves that bulge past their control points.
constexpr qreal GRID_MARGIN = 0.5;
constexpr int HIGH_DENSITY_SCALE = 2;
constexpr int SIZE_SWEEP_LIMIT = 200;
constexpr int OPAQUE_ALPHA = 255;
constexpr int MINIMUM_VISIBLE_PIXELS = 20;

[[nodiscard]] int CountInkPixels(const QImage& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            if (qAlpha(image.pixel(x, y)) > 0)
            {
                ++count;
            }
        }
    }
    return count;
}

[[nodiscard]] QColor FindOpaqueColour(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            if (qAlpha(image.pixel(x, y)) == OPAQUE_ALPHA)
            {
                return QColor::fromRgb(image.pixel(x, y));
            }
        }
    }
    return {};
}
} // namespace

TEST_CASE("Every shipped editor icon parses into the 24 unit grid", "[editor][icons]")
{
    for (usize index = 0; index < EDITOR_ICON_COUNT; ++index)
    {
        const QPainterPath& outline = GetEditorIconOutline(static_cast<EditorIcon>(index));
        INFO("icon index " << index);
        REQUIRE(outline.isEmpty() == false);
        const QRectF bounds = outline.boundingRect();
        CHECK(bounds.left() >= -GRID_MARGIN);
        CHECK(bounds.top() >= -GRID_MARGIN);
        CHECK(bounds.right() <= EDITOR_ICON_GRID + GRID_MARGIN);
        CHECK(bounds.bottom() <= EDITOR_ICON_GRID + GRID_MARGIN);
    }
}

TEST_CASE("Icon path parser handles relative, repeated, arc and malformed data", "[editor][icons]")
{
    QPainterPath path;
    REQUIRE(TryParseIconPathData(QStringLiteral("M0 0h10v10"), path) == true);
    CHECK(path.boundingRect() == QRectF(0.0, 0.0, SQUARE_SIDE, SQUARE_SIDE));
    // Pairs after a moveto are implicit linetos, and numbers may run together at a sign.
    REQUIRE(TryParseIconPathData(QStringLiteral("m1 1 9 0-0 9z"), path) == true);
    CHECK(path.boundingRect() == QRectF(1.0, 1.0, SQUARE_SIDE - 1.0, SQUARE_SIDE - 1.0));
    // A half circle from the left to the right edge bulges to its radius, with compact arc flags.
    REQUIRE(TryParseIconPathData(QStringLiteral("M0 12a12 12 0 01 24 0"), path) == true);
    const QRectF arc = path.boundingRect();
    CHECK(std::abs(arc.left()) < TOLERANCE);
    CHECK(std::abs(arc.right() - EDITOR_ICON_GRID) < TOLERANCE);
    CHECK(std::abs(arc.top() - (HALF_CIRCLE_RADIUS - HALF_CIRCLE_RADIUS)) < TOLERANCE);
    CHECK(std::abs(arc.bottom() - HALF_CIRCLE_RADIUS) < TOLERANCE);
    // Malformed data fails and leaves nothing behind.
    for (const QString& broken : {QString(),
                                  QStringLiteral("M0"),
                                  QStringLiteral("M0 0 L1 x"),
                                  QStringLiteral("M0 0 Y1 1"),
                                  QStringLiteral("M0 0 z 1 1")})
    {
        INFO(broken.toStdString());
        CHECK(TryParseIconPathData(broken, path) == false);
        CHECK(path.isEmpty() == true);
    }
}

TEST_CASE("Editor icons draw in the palette and cache one pixmap per variant", "[editor][icons]")
{
    ClearEditorIconCache();
    const QIcon icon = GetEditorIcon(EditorIcon::Save);
    REQUIRE(icon.isNull() == false);
    const QSize size(EDITOR_ICON_SIZE, EDITOR_ICON_SIZE);
    const QPixmap normal = icon.pixmap(size, QIcon::Normal);
    CHECK(normal.size() == size);
    const QImage normalImage = normal.toImage();
    CHECK(CountInkPixels(normalImage) > MINIMUM_VISIBLE_PIXELS);
    const QPalette palette = QApplication::palette();
    CHECK(FindOpaqueColour(normalImage) == palette.color(QPalette::Active, QPalette::ButtonText));

    const QImage disabledImage = icon.pixmap(size, QIcon::Disabled).toImage();
    CHECK(FindOpaqueColour(disabledImage) == palette.color(QPalette::Disabled, QPalette::ButtonText));

    // Asking again reuses the cached pixmaps instead of drawing them again.
    const int cached = GetEditorIconCacheCount();
    CHECK(cached == 2);
    static_cast<void>(icon.pixmap(size, QIcon::Normal));
    static_cast<void>(icon.pixmap(size, QIcon::Disabled));
    CHECK(GetEditorIconCacheCount() == cached);

    // A high-density pixmap has more device pixels for the same logical size.
    const QPixmap dense = icon.pixmap(size, HIGH_DENSITY_SCALE, QIcon::Normal);
    CHECK(dense.width() == EDITOR_ICON_SIZE * HIGH_DENSITY_SCALE);
    CHECK(GetEditorIconCacheCount() == cached + 1);
}

TEST_CASE("Editor icon cache stays bounded", "[editor][icons]")
{
    ClearEditorIconCache();
    const QIcon icon = GetEditorIcon(EditorIcon::Play);
    for (int side = 1; side <= SIZE_SWEEP_LIMIT; ++side)
    {
        static_cast<void>(icon.pixmap(QSize(side, side), QIcon::Normal));
        CHECK(GetEditorIconCacheCount() <= GetEditorIconCacheLimit());
    }
    ClearEditorIconCache();
    CHECK(GetEditorIconCacheCount() == 0);
}
