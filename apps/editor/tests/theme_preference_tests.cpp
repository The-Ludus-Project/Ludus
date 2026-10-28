// Theme preferences: environment overrides beat the system either way, reduced motion removes durations
// and effects, and type roles scale the system font with tabular figures in spin boxes.

#include "design_tokens.h"
#include "internal/theme_fonts.h"
#include "internal/theme_preferences.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QByteArray>
#include <QDoubleSpinBox>
#include <QMainWindow>

using namespace ludus::editor;

TEST_CASE("Environment overrides parse the documented values", "[editor][theme-preferences]")
{
    for (const char* on : {"1", "true", "TRUE", " on "})
    {
        CHECK(ParseEnvironmentOverride(QByteArray(on)) == EnvironmentOverride::On);
    }
    for (const char* off : {"0", "false", "Off"})
    {
        CHECK(ParseEnvironmentOverride(QByteArray(off)) == EnvironmentOverride::Off);
    }
    for (const char* ignored : {"", "yes", "2", "maybe"})
    {
        CHECK(ParseEnvironmentOverride(QByteArray(ignored)) == EnvironmentOverride::Unset);
    }
}

TEST_CASE("An override beats the system setting in both directions", "[editor][theme-preferences]")
{
    const ThemePreferences quiet{ .IsReducedMotion = false, .IsHighContrast = false };
    const ThemePreferences requested{ .IsReducedMotion = true, .IsHighContrast = true };
    SECTION("Unset keeps the system value")
    {
        CHECK(ResolveThemePreferences(requested, EnvironmentOverride::Unset, EnvironmentOverride::Unset)
                  .IsReducedMotion == true);
        CHECK(ResolveThemePreferences(quiet, EnvironmentOverride::Unset, EnvironmentOverride::Unset).IsHighContrast ==
              false);
    }
    SECTION("On forces the preference")
    {
        const ThemePreferences resolved =
            ResolveThemePreferences(quiet, EnvironmentOverride::On, EnvironmentOverride::On);
        CHECK(resolved.IsReducedMotion == true);
        CHECK(resolved.IsHighContrast == true);
    }
    SECTION("Off clears the preference")
    {
        const ThemePreferences resolved =
            ResolveThemePreferences(requested, EnvironmentOverride::Off, EnvironmentOverride::Off);
        CHECK(resolved.IsReducedMotion == false);
        CHECK(resolved.IsHighContrast == false);
    }
    SECTION("Motion and contrast resolve independently")
    {
        const ThemePreferences resolved =
            ResolveThemePreferences(quiet, EnvironmentOverride::On, EnvironmentOverride::Unset);
        CHECK(resolved.IsReducedMotion == true);
        CHECK(resolved.IsHighContrast == false);
    }
}

TEST_CASE("The process environment reaches the resolved preferences", "[editor][theme-preferences]")
{
    qputenv(REDUCE_MOTION_VARIABLE, QByteArrayLiteral("1"));
    qputenv(HIGH_CONTRAST_VARIABLE, QByteArrayLiteral("1"));
    CHECK(GetThemePreferences().IsReducedMotion == true);
    CHECK(GetThemePreferences().IsHighContrast == true);
    qputenv(REDUCE_MOTION_VARIABLE, QByteArrayLiteral("0"));
    qputenv(HIGH_CONTRAST_VARIABLE, QByteArrayLiteral("0"));
    CHECK(GetThemePreferences().IsReducedMotion == false);
    CHECK(GetThemePreferences().IsHighContrast == false);
    qunsetenv(REDUCE_MOTION_VARIABLE);
    qunsetenv(HIGH_CONTRAST_VARIABLE);
}

TEST_CASE("Reduced motion zeroes durations and stops dock and menu animation", "[editor][theme-preferences]")
{
    constexpr int NORMAL_DURATION = 180;
    CHECK(GetMotionDurationMilliseconds({ .IsReducedMotion = false }, NORMAL_DURATION) == NORMAL_DURATION);
    CHECK(GetMotionDurationMilliseconds({ .IsReducedMotion = true }, NORMAL_DURATION) == 0);

    QMainWindow window;
    ApplyMotionPreference({ .IsReducedMotion = false }, &window);
    const bool wasAnimated = window.isAnimated();
    const bool wasMenuEffectEnabled = QApplication::isEffectEnabled(Qt::UI_AnimateMenu);
    ApplyMotionPreference({ .IsReducedMotion = true }, &window);
    CHECK(window.isAnimated() == false);
    CHECK(QApplication::isEffectEnabled(Qt::UI_AnimateMenu) == false);
    CHECK(QApplication::isEffectEnabled(Qt::UI_FadeTooltip) == false);
    // Turning it off again restores what the platform had, rather than forcing effects on.
    ApplyMotionPreference({ .IsReducedMotion = false }, &window);
    CHECK(window.isAnimated() == wasAnimated);
    CHECK(QApplication::isEffectEnabled(Qt::UI_AnimateMenu) == wasMenuEffectEnabled);
}

TEST_CASE("Type roles scale the system font and only titles are bold", "[editor][theme-preferences]")
{
    CHECK(GetTypeScale(TypeRole::Body) == Catch::Approx(1.0));
    CHECK(GetTypeScale(TypeRole::Caption) < GetTypeScale(TypeRole::Body));
    CHECK(GetTypeScale(TypeRole::Body) < GetTypeScale(TypeRole::Title));
    CHECK(GetTypeScale(TypeRole::Title) < GetTypeScale(TypeRole::Heading));
    QFont base;
    base.setPointSizeF(10.0);
    CHECK(MakeTypeFont(base, TypeRole::Body).pointSizeF() == Catch::Approx(10.0));
    CHECK(MakeTypeFont(base, TypeRole::Caption).pointSizeF() < 10.0);
    CHECK(MakeTypeFont(base, TypeRole::Heading).pointSizeF() == Catch::Approx(10.0 * design::TYPE_SCALE_HEADING));
    CHECK(MakeTypeFont(base, TypeRole::Heading).bold() == true);
    CHECK(MakeTypeFont(base, TypeRole::Title).bold() == true);
    CHECK(MakeTypeFont(base, TypeRole::Body).bold() == false);
    QFont pixelBase;
    pixelBase.setPixelSize(20);
    CHECK(MakeTypeFont(pixelBase, TypeRole::Heading).pixelSize() ==
          static_cast<int>(20 * design::TYPE_SCALE_HEADING + 0.5));
}

TEST_CASE("Spin boxes use tabular figures so scrubbed values do not jitter", "[editor][theme-preferences]")
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    const QFont tabular = MakeTabularFont(QFont());
    CHECK(tabular.isFeatureSet(QFont::Tag("tnum")) == true);
    CHECK(tabular.featureValue(QFont::Tag("tnum")) == 1);
    ApplyThemeFonts();
    QDoubleSpinBox spinBox;
    spinBox.ensurePolished();
    CHECK(spinBox.font().isFeatureSet(QFont::Tag("tnum")) == true);
    QWidget plain;
    CHECK(plain.font().isFeatureSet(QFont::Tag("tnum")) == false);
#else
    CHECK(MakeTabularFont(QFont()) == QFont());
#endif
}
