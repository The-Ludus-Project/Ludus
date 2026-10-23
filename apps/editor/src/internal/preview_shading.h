#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QString>

namespace ludus::graphics::renderer
{
enum class MaterialDebugView : foundation::uint8;
struct ViewDescription;
} // namespace ludus::graphics::renderer

namespace ludus::editor
{
/// Editor visualization mode; does not modify authored materials or lights.
/// Values are combo-box indices and stay stable: append new modes at the end.
enum class PreviewShading : foundation::uint8
{
    Solid,
    Albedo,
    Lit,
    /// Shaded world-space normal as RGB.
    Normal,
    /// Linear camera distance between the camera's near and far planes, near black to far white.
    Depth,
    /// Perceptual roughness as grey.
    Roughness,
    /// Metallic fraction as grey.
    Metallic,
    /// Fractional mesh UV0 as red/green.
    Uv,
    /// Direct lights affecting each pixel, as a heat ramp.
    LightCount,
};
/// Number of PreviewShading values, for enumeration and validation.
inline constexpr foundation::usize PREVIEW_SHADING_COUNT =
    static_cast<foundation::usize>(PreviewShading::LightCount) + 1;

/// Whether the mode is a renderer debug view rather than an ordinary shading mode.
[[nodiscard]] bool IsPreviewDebugShading(PreviewShading shading) noexcept;
/// Renderer debug view for the mode; None for Solid, Albedo and Lit.
[[nodiscard]] graphics::renderer::MaterialDebugView GetMaterialDebugView(PreviewShading shading) noexcept;
/// Stable lower-case persistence key, such as "lit" or "lightCount".
[[nodiscard]] QString GetPreviewShadingKey(PreviewShading shading);
/// Parse a persistence key; unknown keys return false and preserve outShading.
[[nodiscard]] bool TryParsePreviewShading(const QString& key, PreviewShading& outShading);
/// Viewport dropdown label: the mode name and a short explanation.
[[nodiscard]] QString GetPreviewShadingLabel(PreviewShading shading);
/// Short status-line suffix describing what the viewport shows, starting with " · ".
[[nodiscard]] QString GetPreviewShadingStatus(PreviewShading shading);
/// Depth-view distance range {near, far} for a prepared view and its camera clip planes.
/// Starts from the camera's near plane (at least zero) and finite far plane. With
/// boundsWorldOrNull, both tighten to the scene's nearest and farthest extent (radial distance
/// for perspective, along the view axis for orthographic) so visible geometry spans the full
/// black-to-white ramp. Always returns a range the renderer admits (0 <= near < far).
[[nodiscard]] foundation::math::Vector2
ComputeDebugDepthRange(const graphics::renderer::ViewDescription& view,
                       foundation::float32 nearPlane,
                       foundation::float32 farPlane,
                       const foundation::math::Aabb3* boundsWorldOrNull) noexcept;
} // namespace ludus::editor
