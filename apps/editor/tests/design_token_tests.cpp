// Design tokens: one JSON source (config/design-tokens.json) generates the Editor's colours, so contrast is
// proven for every pair in both appearances and the workspace style really reads the tokens.

#include "design_tokens.h"
#include "internal/workspace_style.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>
#include <QWidget>

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
        CHECK(widget.styleSheet().contains(
                  QColor::fromRgb(design::GetThemeArgb(design::ThemeColour::AccentPrimary, isDark)).name()) == true);
    }
    QApplication::setPalette(original);
}
