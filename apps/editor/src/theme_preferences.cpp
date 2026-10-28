#include "internal/theme_preferences.h"

#include <QApplication>
#include <QGuiApplication>
#include <QMainWindow>
#include <QStyleHints>

#include <iterator>

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#    include <QAccessibilityHints>
#endif

namespace ludus::editor
{
namespace
{
using foundation::int32;
using foundation::usize;

/// Effects Qt animates or fades; each is turned off while motion is reduced.
constexpr Qt::UIEffect ANIMATED_EFFECTS[] = {
    Qt::UI_AnimateMenu,
    Qt::UI_FadeMenu,
    Qt::UI_AnimateCombo,
    Qt::UI_AnimateTooltip,
    Qt::UI_FadeTooltip,
    Qt::UI_AnimateToolBox,
};

bool Applies(EnvironmentOverride choice, bool isSystemValue) noexcept
{
    return choice == EnvironmentOverride::Unset ? isSystemValue : choice == EnvironmentOverride::On;
}
} // namespace

EnvironmentOverride ParseEnvironmentOverride(const QByteArray& value) noexcept
{
    const QByteArray lowered = value.trimmed().toLower();
    if (lowered == "1" || lowered == "true" || lowered == "on")
    {
        return EnvironmentOverride::On;
    }
    if (lowered == "0" || lowered == "false" || lowered == "off")
    {
        return EnvironmentOverride::Off;
    }
    return EnvironmentOverride::Unset;
}

ThemePreferences ResolveThemePreferences(ThemePreferences system,
                                         EnvironmentOverride motionOverride,
                                         EnvironmentOverride contrastOverride) noexcept
{
    return
    {
        .IsReducedMotion = Applies(motionOverride, system.IsReducedMotion),
        .IsHighContrast = Applies(contrastOverride, system.IsHighContrast),
    };
}

ThemePreferences ReadSystemThemePreferences()
{
    ThemePreferences system;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    const QStyleHints* hints = QGuiApplication::styleHints();
    if (hints != nullptr)
    {
        system.IsHighContrast = hints->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
#    if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        system.IsReducedMotion = hints->accessibility()->motionPreference() == Qt::MotionPreference::ReducedMotion;
#    endif
    }
#endif
    return system;
}

ThemePreferences GetThemePreferences()
{
    return ResolveThemePreferences(ReadSystemThemePreferences(),
                                   ParseEnvironmentOverride(qgetenv(REDUCE_MOTION_VARIABLE)),
                                   ParseEnvironmentOverride(qgetenv(HIGH_CONTRAST_VARIABLE)));
}

int32 GetMotionDurationMilliseconds(const ThemePreferences& preferences, int32 normalMilliseconds) noexcept
{
    return preferences.IsReducedMotion == true ? 0 : normalMilliseconds;
}

void ApplyMotionPreference(const ThemePreferences& preferences, QMainWindow* windowOrNull)
{
    // The platform's own effect settings are remembered once, so turning reduced motion off restores them
    // instead of forcing effects on.
    static bool isPlatformEffectEnabled[std::size(ANIMATED_EFFECTS)];
    static bool hasPlatformEffects = false;
    if (hasPlatformEffects == false)
    {
        for (usize index = 0; index < std::size(ANIMATED_EFFECTS); ++index)
        {
            isPlatformEffectEnabled[index] = QApplication::isEffectEnabled(ANIMATED_EFFECTS[index]);
        }
        hasPlatformEffects = true;
    }
    const bool isReduced = preferences.IsReducedMotion == true;
    for (usize index = 0; index < std::size(ANIMATED_EFFECTS); ++index)
    {
        QApplication::setEffectEnabled(ANIMATED_EFFECTS[index], isReduced == false && isPlatformEffectEnabled[index]);
    }
    if (windowOrNull != nullptr)
    {
        windowOrNull->setAnimated(isReduced == false);
    }
}
} // namespace ludus::editor
