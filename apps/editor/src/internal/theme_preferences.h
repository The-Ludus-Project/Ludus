#pragma once

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QMainWindow;
QT_END_NAMESPACE

namespace ludus::editor
{
/// What the person asked of the interface. Each flag is the system setting unless an environment override wins.
struct ThemePreferences final
{
    /// Skip animated dock moves and menu, combo and tooltip effects; motion helpers return zero duration.
    bool IsReducedMotion = false;
    /// Use the high-contrast token set, thicker control edges and a thicker focus ring.
    bool IsHighContrast = false;
};

/// Answer of one environment variable that forces a preference on or off, or leaves the system value alone.
enum class EnvironmentOverride : foundation::uint8
{
    Unset,
    On,
    Off,
};

/// Environment variable that forces reduced motion on ("1", "true", "on") or off ("0", "false", "off").
inline constexpr const char* REDUCE_MOTION_VARIABLE = "LUDUS_REDUCE_MOTION";
/// Environment variable that forces high contrast on or off with the same values.
inline constexpr const char* HIGH_CONTRAST_VARIABLE = "LUDUS_HIGH_CONTRAST";

/// Parse one override value; anything unrecognised (including empty) leaves the system value alone.
[[nodiscard]] EnvironmentOverride ParseEnvironmentOverride(const QByteArray& value) noexcept;

/// The system setting with each environment override applied; an override beats the system either way.
[[nodiscard]] ThemePreferences ResolveThemePreferences(ThemePreferences system,
                                                       EnvironmentOverride motionOverride,
                                                       EnvironmentOverride contrastOverride) noexcept;

/// Preferences reported by Qt for the running platform. Contrast needs Qt 6.10 and motion Qt 6.12; older Qt
/// reports false for the ones it cannot read, so the environment overrides are the only way to ask for them.
[[nodiscard]] ThemePreferences ReadSystemThemePreferences();

/// ReadSystemThemePreferences with the LUDUS_REDUCE_MOTION and LUDUS_HIGH_CONTRAST overrides applied.
[[nodiscard]] ThemePreferences GetThemePreferences();

/// Duration to use for a transition: zero when motion is reduced, otherwise normalMilliseconds.
[[nodiscard]] foundation::int32 GetMotionDurationMilliseconds(const ThemePreferences& preferences,
                                                              foundation::int32 normalMilliseconds) noexcept;

/// Switch the application-wide menu, combo box and tooltip effects, and the dock animation of window, to match
/// preferences. Safe to call again when the preferences change.
void ApplyMotionPreference(const ThemePreferences& preferences, QMainWindow* windowOrNull);
} // namespace ludus::editor
