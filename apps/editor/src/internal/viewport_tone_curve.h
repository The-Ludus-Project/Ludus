#pragma once

#include <ludus/graphics/renderer/tone_mapping.hpp>

namespace ludus::editor
{
/// Display curve of the Editor's raster viewport. The viewport presentation, the debug shading views and the
/// reference comparison all use it, so they agree on what a scene-linear colour looks like.
inline constexpr graphics::renderer::ToneCurve VIEWPORT_TONE_CURVE = graphics::renderer::ToneCurve::PbrNeutral;
} // namespace ludus::editor
