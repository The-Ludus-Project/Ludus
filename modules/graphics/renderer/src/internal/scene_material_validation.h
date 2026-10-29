#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/renderer/scene_shader.hpp>

namespace ludus::graphics::renderer::internal
{
/// Scene inputs admit analytic sources only; area quadrature proxies would duplicate mesh emitters.
[[nodiscard]] LUDUS_INLINE bool IsSceneShaderLightKindSupported(LightKind kind) noexcept
{
    return kind == LightKind::Directional || kind == LightKind::Point || kind == LightKind::Spot;
}
/// Shared ray/forward material admission, including map extents and sampler semantics.
[[nodiscard]] bool IsSceneShaderMaterialValid(const SceneShaderMaterial& material) noexcept;
/// Shared nonnegative finite scene radiance bound, including the authored environment.
[[nodiscard]] bool IsSceneRadianceValid(foundation::math::Vector3 radiance) noexcept;
} // namespace ludus::graphics::renderer::internal
