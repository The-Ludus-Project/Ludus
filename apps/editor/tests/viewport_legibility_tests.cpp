// Legibility of the viewport chrome: the empty-viewport caption, the controls row on narrow windows and the
// size of the first window (docs/architecture/editor-art-direction.md).

#include "internal/empty_world_view.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/theme_colour.h"
#include "internal/window_size.h"
#include "internal/workspace_style.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/world_grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QPalette>
#include <QRect>
#include <QSize>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>

using namespace ludus::editor;

namespace
{
/// Caption colour painted over the viewport background (src/empty_world_view.cpp).
constexpr QRgb CAPTION = GetViewportArgb(design::ThemeColour::TextHud);
/// Fixed viewport background (src/empty_world_view.cpp).
constexpr QRgb BACKGROUND = GetViewportArgb(design::ThemeColour::SurfaceViewport);
/// WCAG 2.2 success criterion 1.4.3 minimum for body text.
constexpr double MINIMUM_TEXT_CONTRAST = 4.5;
/// Gamma-encoded channel threshold and offsets from WCAG relative luminance.
constexpr double LINEAR_THRESHOLD = 0.04045;
constexpr double LINEAR_DIVISOR = 12.92;
constexpr double GAMMA_OFFSET = 0.055;
constexpr double GAMMA_SCALE = 1.055;
constexpr double GAMMA_EXPONENT = 2.4;
constexpr double LUMINANCE_RED = 0.2126;
constexpr double LUMINANCE_GREEN = 0.7152;
constexpr double LUMINANCE_BLUE = 0.0722;
constexpr double CONTRAST_FLARE = 0.05;
constexpr double CHANNEL_MAXIMUM = 255.0;
/// Height of the strip at the bottom of the viewport that holds the caption.
constexpr int CAPTION_STRIP_HEIGHT = 48;
/// Width of the viewport used by the roomy and narrow cases.
/// Pixels of an axis letter's exact colour (glyph cores) that must appear on the viewport.
constexpr int MINIMUM_LETTER_PIXELS = 4;
constexpr int ROOMY_WIDTH = 1600;
constexpr int NARROW_WIDTH = 360;
constexpr int VIEWPORT_HEIGHT = 480;

double Linearize(int channel)
{
    const double value = static_cast<double>(channel) / CHANNEL_MAXIMUM;
    return value <= LINEAR_THRESHOLD ? value / LINEAR_DIVISOR
                                     : std::pow((value + GAMMA_OFFSET) / GAMMA_SCALE, GAMMA_EXPONENT);
}

double RelativeLuminance(QRgb colour)
{
    return LUMINANCE_RED * Linearize(qRed(colour)) + LUMINANCE_GREEN * Linearize(qGreen(colour)) +
           LUMINANCE_BLUE * Linearize(qBlue(colour));
}

double ContrastRatio(QRgb first, QRgb second)
{
    const double firstLuminance = RelativeLuminance(first);
    const double secondLuminance = RelativeLuminance(second);
    const double lighter = firstLuminance > secondLuminance ? firstLuminance : secondLuminance;
    const double darker = firstLuminance > secondLuminance ? secondLuminance : firstLuminance;
    return (lighter + CONTRAST_FLARE) / (darker + CONTRAST_FLARE);
}

QPalette MakePalette(bool isDark)
{
    QPalette palette;
    palette.setColor(QPalette::Window, isDark ? QColor(30, 30, 30) : QColor(240, 240, 240));
    return palette;
}

/// A glyph pixel is a blend of the caption colour over the background. It counts as caption-coloured when it lies
/// on the background-to-caption line: at least CAPTION_COVERAGE of the way along, with all three channels agreeing
/// within BLEND_SPREAD. Exact matches are not required because hinting and font choice decide whether any glyph
/// pixel is fully covered.
constexpr double CAPTION_COVERAGE = 0.4;
constexpr double BLEND_SPREAD = 0.1;

bool IsCaptionBlend(QRgb pixel)
{
    const auto fraction = [](int value, int caption, int background) {
        return static_cast<double>(value - background) / static_cast<double>(caption - background);
    };
    const double red = fraction(qRed(pixel), qRed(CAPTION), qRed(BACKGROUND));
    const double green = fraction(qGreen(pixel), qGreen(CAPTION), qGreen(BACKGROUND));
    const double blue = fraction(qBlue(pixel), qBlue(CAPTION), qBlue(BACKGROUND));
    const double lowest = std::min({red, green, blue});
    const double highest = std::max({red, green, blue});
    return lowest >= CAPTION_COVERAGE && highest <= 1.0 && highest - lowest <= BLEND_SPREAD;
}

/// Counts pixels in the bottom strip that are mostly the caption colour, i.e. glyph cores that no style sheet or
/// palette recoloured.
int CountCaptionPixels(const QImage& image)
{
    int count = 0;
    for (int y = image.height() - CAPTION_STRIP_HEIGHT; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            count += IsCaptionBlend(image.pixel(x, y)) ? 1 : 0;
        }
    }
    return count;
}
} // namespace

TEST_CASE("Empty viewport caption keeps its contrast under both workspace palettes", "[editor][viewport][legibility]")
{
    CHECK(ContrastRatio(CAPTION, BACKGROUND) >= MINIMUM_TEXT_CONTRAST);
    const QPalette original = QApplication::palette();
    for (const bool isDark : {true, false})
    {
        QApplication::setPalette(MakePalette(isDark));
        RenderPreview preview;
        QWidget host;
        ApplyWorkspaceStyle(&host);
        QVBoxLayout layout(&host);
        layout.setContentsMargins(0, 0, 0, 0);
        PreviewViewport viewport(&preview);
        layout.addWidget(&viewport);
        host.resize(ROOMY_WIDTH, VIEWPORT_HEIGHT);
        host.show();
        QApplication::processEvents();
        QLabel* caption = viewport.findChild<QLabel*>(QStringLiteral("emptyWorldView"));
        REQUIRE(caption != nullptr);
        REQUIRE(caption->text().isEmpty() == false);
        REQUIRE(caption->isVisible() == true);
        const QImage image = caption->grab().toImage();
        CHECK(CountCaptionPixels(image) > 0);
    }
    QApplication::setPalette(original);
}

TEST_CASE("Empty viewport letters its axes in a colour that keeps text contrast", "[editor][viewport][legibility]")
{
    namespace renderer = ludus::graphics::renderer;
    namespace foundation = ludus::foundation;
    RenderPreview preview;
    QWidget host;
    ApplyWorkspaceStyle(&host);
    QVBoxLayout layout(&host);
    layout.setContentsMargins(0, 0, 0, 0);
    PreviewViewport viewport(&preview);
    layout.addWidget(&viewport);
    host.resize(ROOMY_WIDTH, VIEWPORT_HEIGHT);
    host.show();
    QApplication::processEvents();
    QLabel* caption = viewport.findChild<QLabel*>(QStringLiteral("emptyWorldView"));
    REQUIRE(caption != nullptr);
    renderer::ViewDescription view;
    QString error;
    REQUIRE(preview.DescribeView(static_cast<foundation::uint32>(caption->width()),
                                 static_cast<foundation::uint32>(caption->height()),
                                 view,
                                 error) == true);
    foundation::StaticArray<renderer::WorldGridLine, renderer::WORLD_GRID_LINE_CAPACITY> lines;
    foundation::usize count = 0;
    foundation::float32 step = 0;
    REQUIRE(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, step) == true);
    const QImage image = caption->grab().toImage();
    int letteredAxes = 0;
    for (foundation::usize i = 0; i < count; ++i)
    {
        const foundation::int32 axis = GetAxisIndexOrNegative(lines[i]);
        if (axis < 0)
        {
            continue;
        }
        const QColor axisColour =
            QColor::fromRgbF(static_cast<qreal>(ludus::foundation::math::EncodeSrgb(lines[i].Color.X)),
                             static_cast<qreal>(ludus::foundation::math::EncodeSrgb(lines[i].Color.Y)),
                             static_cast<qreal>(ludus::foundation::math::EncodeSrgb(lines[i].Color.Z)));
        const QRgb letterColour = GetAxisLetterColour(axisColour).rgb();
        CHECK(ContrastRatio(letterColour, BACKGROUND) >= MINIMUM_TEXT_CONTRAST);
        int letterPixels = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                letterPixels += image.pixel(x, y) == letterColour ? 1 : 0;
            }
        }
        INFO("axis " << axis);
        CHECK(letterPixels >= MINIMUM_LETTER_PIXELS);
        ++letteredAxes;
    }
    CHECK(letteredAxes == static_cast<int>(renderer::WORLD_GRID_AXIS_COUNT));
}

TEST_CASE("Viewport controls overflow instead of truncating", "[editor][viewport][legibility]")
{
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(ROOMY_WIDTH, VIEWPORT_HEIGHT);
    viewport.show();
    QApplication::processEvents();
    QToolBar* controls = viewport.findChild<QToolBar*>(QStringLiteral("viewportControls"));
    REQUIRE(controls != nullptr);
    QToolButton* chevron = controls->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
    REQUIRE(chevron != nullptr);
    const auto isFullyShown = [controls](const char* objectName) {
        const QWidget* control = controls->findChild<QWidget*>(QString::fromLatin1(objectName));
        return control != nullptr && control->isVisible() == true &&
               controls->rect().contains(control->geometry()) == true &&
               control->width() >= control->minimumSizeHint().width();
    };
    // Roomy: every control is on the row and no chevron is needed.
    CHECK(chevron->isVisible() == false);
    CHECK(isFullyShown("viewportShading") == true);
    CHECK(isFullyShown("viewportNavigationHint") == true);
    // Narrow: whatever stays on the row is complete, and the rest sits behind the chevron.
    viewport.resize(NARROW_WIDTH, VIEWPORT_HEIGHT);
    QApplication::processEvents();
    CHECK(chevron->isVisible() == true);
    for (QAction* action : controls->actions())
    {
        const QWidgetAction* widgetAction = qobject_cast<QWidgetAction*>(action);
        if (widgetAction == nullptr || widgetAction->defaultWidget() == nullptr ||
            widgetAction->defaultWidget()->isVisible() == false)
        {
            continue;
        }
        const QWidget* control = widgetAction->defaultWidget();
        CHECK(controls->rect().contains(control->geometry()) == true);
        CHECK(control->width() >= control->minimumSizeHint().width());
    }
}

TEST_CASE("First window size follows the screen with a comfortable minimum", "[editor][window]")
{
    // A large display gets 80 percent of its available area.
    CHECK(ComputeDefaultWindowSize({2560, 1440}) == QSize(2048, 1152));
    // A 1080p display would give 1536x864; both exceed the minimum.
    CHECK(ComputeDefaultWindowSize({1920, 1080}) == QSize(1536, 864));
    // A small laptop raises 80 percent up to the minimum where it fits.
    CHECK(ComputeDefaultWindowSize({1440, 900}) == QSize(1280, 800));
    // A screen smaller than the minimum is never exceeded.
    CHECK(ComputeDefaultWindowSize({1024, 768}) == QSize(1024, 768));
    // No usable screen falls back to the minimum.
    CHECK(ComputeDefaultWindowSize({}) == FALLBACK_WINDOW_SIZE);
    CHECK(ComputeDefaultWindowSize({0, 0}) == FALLBACK_WINDOW_SIZE);
}
