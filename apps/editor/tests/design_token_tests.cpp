// Design tokens: one JSON source (config/design-tokens.json) generates the Editor's colours, so contrast is
// proven for every pair in both appearances and the workspace style really reads the tokens.

#include "design_tokens.h"
#include "internal/editor_style.h"
#include "internal/overlay_widgets.h"
#include "internal/theme_colour.h"
#include "internal/theme_preferences.h"
#include "internal/workspace_style.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QString>
#include <QStyle>
#include <QWidget>

#include <algorithm>
#include <cmath>

using namespace ludus::editor;
using ludus::foundation::uint32;
using ludus::foundation::usize;

namespace
{
/// WCAG relative-luminance constants.
constexpr double LINEAR_THRESHOLD = 0.04045;
constexpr double LINEAR_DIVISOR = 12.92;
constexpr double GAMMA_OFFSET = 0.055;
constexpr double GAMMA_SCALE = 1.055;
constexpr double GAMMA_EXPONENT = 2.4;
constexpr double CONTRAST_FLARE = 0.05;
constexpr double CHANNEL_MAXIMUM = 255.0;
constexpr double LUMINANCE_RED = 0.2126;
constexpr double LUMINANCE_GREEN = 0.7152;
constexpr double LUMINANCE_BLUE = 0.0722;
/// Lightness below which the application palette counts as dark (matches the workspace style).
constexpr int DARK_LIGHTNESS = 40;
constexpr int LIGHT_LIGHTNESS = 230;

double Linearize(int channel)
{
    const double value = static_cast<double>(channel) / CHANNEL_MAXIMUM;
    return value <= LINEAR_THRESHOLD ? value / LINEAR_DIVISOR
                                     : std::pow((value + GAMMA_OFFSET) / GAMMA_SCALE, GAMMA_EXPONENT);
}

double RelativeLuminance(uint32 rgb)
{
    const QColor colour = QColor::fromRgb(rgb);
    return LUMINANCE_RED * Linearize(colour.red()) + LUMINANCE_GREEN * Linearize(colour.green()) +
           LUMINANCE_BLUE * Linearize(colour.blue());
}

double ContrastRatio(uint32 first, uint32 second)
{
    const double firstLuminance = RelativeLuminance(first);
    const double secondLuminance = RelativeLuminance(second);
    const double lighter = firstLuminance > secondLuminance ? firstLuminance : secondLuminance;
    const double darker = firstLuminance > secondLuminance ? secondLuminance : firstLuminance;
    return (lighter + CONTRAST_FLARE) / (darker + CONTRAST_FLARE);
}

QPalette MakeApplicationPalette(bool isDark)
{
    QPalette palette;
    palette.setColor(QPalette::Window,
                     QColor(isDark ? DARK_LIGHTNESS : LIGHT_LIGHTNESS,
                            isDark ? DARK_LIGHTNESS : LIGHT_LIGHTNESS,
                            isDark ? DARK_LIGHTNESS : LIGHT_LIGHTNESS));
    return palette;
}
} // namespace

TEST_CASE("Every design token contrast pair holds in both appearances", "[editor][design-tokens]")
{
    REQUIRE(design::CONTRAST_REQUIREMENTS.GetSize() == design::CONTRAST_REQUIREMENT_COUNT);
    for (const design::ContrastRequirement& requirement : design::CONTRAST_REQUIREMENTS)
    {
        for (const bool isDark : {true, false})
        {
            INFO(requirement.Label << (isDark == true ? " (dark)" : " (light)"));
            const double ratio = ContrastRatio(design::GetThemeColour(requirement.Foreground, isDark),
                                               design::GetThemeColour(requirement.Background, isDark));
            CHECK(ratio >= requirement.Minimum);
        }
    }
}

TEST_CASE("Every design token contrast pair reaches its high-contrast minimum in both appearances",
          "[editor][design-tokens]")
{
    constexpr double HIGH_CONTRAST_TEXT_RATIO = 7.0;
    constexpr double NORMAL_TEXT_RATIO = 4.5;
    for (const design::ContrastRequirement& requirement : design::CONTRAST_REQUIREMENTS)
    {
        CHECK(requirement.HighContrastMinimum >= requirement.Minimum);
        for (const bool isDark : {true, false})
        {
            INFO(requirement.Label << (isDark == true ? " (dark, high contrast)" : " (light, high contrast)"));
            const double ratio = ContrastRatio(design::GetThemeColour(requirement.Foreground, isDark, true),
                                               design::GetThemeColour(requirement.Background, isDark, true));
            CHECK(ratio >= requirement.HighContrastMinimum);
            if (requirement.Minimum == NORMAL_TEXT_RATIO)
            {
                CHECK(requirement.HighContrastMinimum >= HIGH_CONTRAST_TEXT_RATIO);
            }
        }
    }
}

TEST_CASE("High contrast changes the workspace style and keeps the viewport", "[editor][design-tokens]")
{
    const QPalette original = QApplication::palette();
    qputenv(HIGH_CONTRAST_VARIABLE, QByteArrayLiteral("1"));
    for (const bool isDark : {true, false})
    {
        INFO((isDark == true ? "dark" : "light"));
        QApplication::setPalette(MakeApplicationPalette(isDark));
        QWidget widget;
        ApplyWorkspaceStyle(&widget);
        CHECK(widget.palette().color(QPalette::WindowText).rgb() ==
              design::GetThemeArgb(design::ThemeColour::TextPrimary, isDark, true));
        CHECK(widget.palette().color(QPalette::Mid).rgb() ==
              design::GetThemeArgb(design::ThemeColour::BorderControl, isDark, true));
        const EditorStyle* style = GetEditorStyleOrNull();
        REQUIRE(style != nullptr);
        CHECK(style->GetFocusRingWidth() == 3);
        CHECK(style->GetEdgeWidth() == 2);
        QListWidget list;
        CHECK(style->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, &list) == 2);
    }
    qunsetenv(HIGH_CONTRAST_VARIABLE);
    QApplication::setPalette(MakeApplicationPalette(true));
    QWidget normal;
    ApplyWorkspaceStyle(&normal);
    REQUIRE(GetEditorStyleOrNull() != nullptr);
    CHECK(GetEditorStyleOrNull()->GetFocusRingWidth() == 2);
    CHECK(GetEditorStyleOrNull()->GetEdgeWidth() == 1);
    // The viewport colour never changes with the interface theme.
    CHECK(design::GetThemeColour(design::ThemeColour::SurfaceViewport, true, true) ==
          design::GetThemeColour(design::ThemeColour::SurfaceViewport, true, false));
    QApplication::setPalette(original);
}

TEST_CASE("Design tokens are well formed", "[editor][design-tokens]")
{
    REQUIRE(design::THEME_COLOURS.GetSize() == design::THEME_COLOUR_COUNT);
    for (const design::ThemeColourDefinition& definition : design::THEME_COLOURS)
    {
        INFO(definition.Name);
        CHECK(QString::fromLatin1(definition.Name).contains(QLatin1Char('.')) == true);
        CHECK(definition.Dark <= 0xFFFFFFU);
        CHECK(definition.Light <= 0xFFFFFFU);
    }
    // Spacing and radius scales grow from small to large.
    CHECK(design::SPACING_EXTRA_SMALL < design::SPACING_SMALL);
    CHECK(design::SPACING_SMALL < design::SPACING_MEDIUM);
    CHECK(design::SPACING_MEDIUM < design::SPACING_LARGE);
    CHECK(design::SPACING_LARGE < design::SPACING_EXTRA_LARGE);
    CHECK(design::RADIUS_CONTROL < design::RADIUS_GROUP);
    CHECK(design::RADIUS_GROUP < design::RADIUS_CARD);
    // The viewport is dark in both appearances.
    CHECK(design::GetThemeColour(design::ThemeColour::SurfaceViewport, true) ==
          design::GetThemeColour(design::ThemeColour::SurfaceViewport, false));
}

TEST_CASE("Workspace style takes its palette from the design tokens", "[editor][design-tokens]")
{
    const QPalette original = QApplication::palette();
    for (const bool isDark : {true, false})
    {
        INFO((isDark == true ? "dark" : "light"));
        QApplication::setPalette(MakeApplicationPalette(isDark));
        QWidget widget;
        ApplyWorkspaceStyle(&widget);
        const QPalette applied = widget.palette();
        CHECK(applied.color(QPalette::Window).rgb() ==
              design::GetThemeArgb(design::ThemeColour::SurfaceWorkspace, isDark));
        CHECK(applied.color(QPalette::Base).rgb() == design::GetThemeArgb(design::ThemeColour::SurfacePanel, isDark));
        CHECK(applied.color(QPalette::Button).rgb() ==
              design::GetThemeArgb(design::ThemeColour::SurfaceRaised, isDark));
        CHECK(applied.color(QPalette::WindowText).rgb() ==
              design::GetThemeArgb(design::ThemeColour::TextPrimary, isDark));
        CHECK(applied.color(QPalette::Mid).rgb() == design::GetThemeArgb(design::ThemeColour::BorderControl, isDark));
        CHECK(applied.color(QPalette::Highlight).rgb() ==
              design::GetThemeArgb(design::ThemeColour::AccentSurface, isDark));
        CHECK(applied.color(QPalette::Link).rgb() == design::GetThemeArgb(design::ThemeColour::AccentPrimary, isDark));
        CHECK(applied.color(QPalette::Disabled, QPalette::WindowText).rgb() ==
              design::GetThemeArgb(design::ThemeColour::TextSecondary, isDark));
        // Colours come from the palette and EditorStyle; no stylesheet is applied.
        CHECK(widget.styleSheet().isEmpty() == true);
        CHECK(GetEditorStyleOrNull() != nullptr);
    }
    QApplication::setPalette(original);
}

TEST_CASE("Viewport and HUD colours come from the tokens", "[editor][design-tokens]")
{
    // The empty viewport, the statistics panel and the overlays draw in the dark viewport set in both appearances.
    CHECK(GetViewportArgb(design::ThemeColour::SurfaceViewport) ==
          design::GetThemeArgb(design::ThemeColour::SurfaceViewport, true));
    CHECK(GetViewportArgb(design::ThemeColour::SurfaceViewport) ==
          design::GetThemeArgb(design::ThemeColour::SurfaceViewport, false));
    constexpr int OPACITY = 190;
    constexpr double NORMAL_TEXT_CONTRAST = 4.5;
    const QRgb translucent = WithAlpha(GetViewportArgb(design::ThemeColour::SurfaceViewport), OPACITY);
    CHECK(qAlpha(translucent) == OPACITY);
    CHECK((translucent & COLOUR_CHANNEL_MASK) ==
          (GetViewportArgb(design::ThemeColour::SurfaceViewport) & COLOUR_CHANNEL_MASK));
    // Text on the viewport keeps the body contrast.
    CHECK(ContrastRatio(GetViewportArgb(design::ThemeColour::TextHud),
                        GetViewportArgb(design::ThemeColour::SurfaceViewport)) >= NORMAL_TEXT_CONTRAST);
    CHECK(ContrastRatio(GetViewportArgb(design::ThemeColour::TextHudQuiet),
                        GetViewportArgb(design::ThemeColour::SurfaceViewport)) >= NORMAL_TEXT_CONTRAST);
}

TEST_CASE("Overlay widgets draw their token colours without a style sheet", "[editor][design-tokens]")
{
    const QPalette original = QApplication::palette();
    QApplication::setPalette(MakeApplicationPalette(true));
    QWidget host;
    host.resize(240, 120);
    QLineEdit field(&host);
    field.setGeometry(20, 20, 120, 30);
    DiagnosticOutline::Attach(&field);
    DiagnosticOutline::Attach(&field);
    host.show();
    QApplication::processEvents();
    CHECK(field.findChildren<QWidget*>(QStringLiteral("diagnosticOutline"), Qt::FindDirectChildrenOnly).size() == 1);
    CHECK(field.styleSheet().isEmpty() == true);
    const QImage frame = host.grab().toImage();
    const QColor danger = GetThemeQColour(design::ThemeColour::StatusDanger);
    CHECK(QColor(frame.pixel(60, 20)) == danger);
    CHECK(QColor(frame.pixel(20, 35)) == danger);
    // The field's own text area is untouched.
    CHECK(QColor(frame.pixel(60, 35)) != danger);

    HudLabel hud(&host);
    hud.setText(QStringLiteral("60 fps"));
    hud.move(100, 70);
    hud.show();
    QApplication::processEvents();
    CHECK(hud.styleSheet().isEmpty() == true);
    const QImage withHud = host.grab().toImage();
    // The panel is darker than the host behind it and carries the HUD text colour.
    CHECK(QColor(withHud.pixel(102, 80)).lightness() < QColor(withHud.pixel(5, 5)).lightness());
    CHECK(hud.palette().color(QPalette::WindowText).rgb() == GetViewportArgb(design::ThemeColour::TextHud));

    LeftAlignedButton button(QStringLiteral("Make a graph"), &host);
    button.setFlat(true);
    button.setGeometry(0, 0, 200, 24);
    button.move(20, 90);
    button.show();
    QApplication::processEvents();
    const QImage buttonImage = button.grab().toImage();
    int firstInk = buttonImage.width();
    int lastInk = 0;
    const QColor surface = QColor(buttonImage.pixel(1, 1));
    for (int x = 0; x < buttonImage.width(); ++x)
    {
        for (int y = 0; y < buttonImage.height(); ++y)
        {
            if (QColor(buttonImage.pixel(x, y)) != surface)
            {
                firstInk = std::min(firstInk, x);
                lastInk = std::max(lastInk, x);
            }
        }
    }
    // Text starts near the left edge instead of centring in the 200 pixel button.
    CHECK(firstInk < button.width() / 4);
    CHECK(lastInk < button.width() / 2);
    QApplication::setPalette(original);
}
