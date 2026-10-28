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

/// Give every spin box tabular figures. Call once after the application font is known.
void ApplyThemeFonts();
} // namespace ludus::editor
