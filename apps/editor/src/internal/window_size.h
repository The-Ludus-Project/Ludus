#pragma once

#include <QSize>

namespace ludus::editor
{
/// Share of the available screen, in percent, that the first Editor window covers.
inline constexpr int DEFAULT_WINDOW_SCREEN_PERCENT = 80;
/// Narrowest first window that still shows the viewport controls row next to the World list.
inline constexpr int COMFORTABLE_WINDOW_WIDTH = 1280;
/// Shortest first window that still shows the default panels without collapsing the bottom dock.
inline constexpr int COMFORTABLE_WINDOW_HEIGHT = 800;
/// Size used when the application has no primary screen to measure.
inline constexpr QSize FALLBACK_WINDOW_SIZE{COMFORTABLE_WINDOW_WIDTH, COMFORTABLE_WINDOW_HEIGHT};

/// Size of the first Editor window: 80 percent of the available screen, at least 1280x800 where
/// the screen has room, and never larger than the available area.
/// @param availableSize Screen area not covered by system bars, in logical pixels.
[[nodiscard]] QSize ComputeDefaultWindowSize(QSize availableSize) noexcept;
} // namespace ludus::editor
