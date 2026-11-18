#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/tone_mapping.hpp>

namespace ludus::editor
{
/// Display curve of the Editor's raster viewport. The viewport presentation, the debug shading views and the
/// reference comparison all use it, so they agree on what a scene-linear colour looks like.
inline constexpr graphics::renderer::ToneCurve VIEWPORT_TONE_CURVE = graphics::renderer::ToneCurve::PbrNeutral;

/// How a viewport output turns scene-linear radiance into display colour: times Exposure, Curve, saturate, then
/// sRGB. The default is the viewport presentation's own transform at exposure one. A render graph output with a
/// ToneMap uses its pass's exposure and curve instead (ComputeReferenceDisplayTransform).
struct ViewportDisplayTransform final
{
    /// Linear multiplier applied before the curve. Finite and positive.
    foundation::float32 Exposure = 1;
    /// Display curve applied after Exposure.
    graphics::renderer::ToneCurve Curve = VIEWPORT_TONE_CURVE;
};
} // namespace ludus::editor
