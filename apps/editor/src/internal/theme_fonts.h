#pragma once

#include <QFont>

namespace ludus::editor
{
/// Type roles of the Editor. Sizes are multipliers of the system font (see `type` in config/design-tokens.json),
/// so they follow the person's system text size.
enum class TypeRole : unsigned char
{
    /// Hints, units and captions.
    Caption,
    /// Everything else.
    Body,
    /// Dialog and panel headings.
    Title,
    /// The Welcome heading.
    Heading,
};

/// Multiplier of the system font size for role.
[[nodiscard]] double GetTypeScale(TypeRole role) noexcept;

/// base scaled to role. Titles and headings are bold; weight is the only other change.
[[nodiscard]] QFont MakeTypeFont(const QFont& base, TypeRole role);

/// base with tabular figures, so digits keep one width and values do not jitter while they are scrubbed.
/// Qt older than 6.7 cannot set the feature and returns base unchanged.
[[nodiscard]] QFont MakeTabularFont(const QFont& base);

/// The widget classes a platform theme may give their own font (Qt's per-class font table), which ApplyThemeFonts sets
/// back to the body font.
inline constexpr const char* BODY_FONT_CLASS_NAMES[] = {
    "QAbstractItemView",
    "QCheckBox",
    "QComboBox",
    "QComboLineEdit",
    "QComboMenuItem",
    "QDockWidgetTitle",
    "QGroupBox",
    "QHeaderView",
    "QLabel",
    "QLineEdit",
    "QListView",
    "QMenu",
    "QMenuBar",
    "QPushButton",
    "QRadioButton",
    "QStatusBar",
    "QTabBar",
    "QToolButton",
};

/// Give every widget class the body font, so a platform theme's smaller per-class fonts do not apply, and every spin
/// box tabular figures. Call once after the application font is known.
void ApplyThemeFonts();
} // namespace ludus::editor
