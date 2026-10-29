#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/lighting.hpp>

#include <QString>

namespace ludus::editor
{
/// Antialiasing a new editor viewport starts with. Fast costs five texel loads per pixel plus the edge walk of the
/// pixels that are edges (docs/architecture/renderer-systems.md); Off restores the unfiltered image.
inline constexpr graphics::renderer::SpatialAntialiasing DEFAULT_VIEWPORT_ANTIALIASING =
    graphics::renderer::SpatialAntialiasing::Fast;
/// Number of antialiasing choices the viewport offers, in toolbar order: Off, Fast, Balanced.
inline constexpr foundation::usize VIEWPORT_ANTIALIASING_COUNT = 3;
/// Stable key of an antialiasing choice for saved navigation state.
[[nodiscard]] QString GetViewportAntialiasingKey(graphics::renderer::SpatialAntialiasing antialiasing);
/// Choice named by key. Failure preserves outAntialiasing.
[[nodiscard]] bool TryParseViewportAntialiasing(const QString& key,
                                                graphics::renderer::SpatialAntialiasing& outAntialiasing);
/// Toolbar label of an antialiasing choice.
[[nodiscard]] QString GetViewportAntialiasingLabel(graphics::renderer::SpatialAntialiasing antialiasing);
} // namespace ludus::editor
