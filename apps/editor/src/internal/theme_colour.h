#pragma once

#include "design_tokens.h"
#include "internal/theme_preferences.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QRgb>

namespace ludus::editor
{
/// Window lightness (of 255) below which the application palette counts as the dark appearance.
inline constexpr int DARK_APPEARANCE_LIGHTNESS_LIMIT = 128;
/// Bits above the colour channels in a QRgb.
inline constexpr unsigned ALPHA_SHIFT = 24;
inline constexpr QRgb COLOUR_CHANNEL_MASK = 0x00FFFFFFU;

/// A design token for the current appearance and contrast preference.
[[nodiscard]] inline QColor GetThemeQColour(design::ThemeColour role)
{
    const bool isDark = QApplication::palette().color(QPalette::Window).lightness() < DARK_APPEARANCE_LIGHTNESS_LIMIT;
    return QColor::fromRgb(design::GetThemeArgb(role, isDark, GetThemePreferences().IsHighContrast));
}

/// A viewport token. The viewport and the text drawn over it are dark in both appearances.
[[nodiscard]] constexpr QRgb GetViewportArgb(design::ThemeColour role) noexcept
{
    return design::GetThemeArgb(role, true);
}

/// The colour with the given 0 to 255 opacity in place of its own.
[[nodiscard]] constexpr QRgb WithAlpha(QRgb colour, int alpha) noexcept
{
    return (colour & COLOUR_CHANNEL_MASK) | (static_cast<QRgb>(alpha) << ALPHA_SHIFT);
}
} // namespace ludus::editor
