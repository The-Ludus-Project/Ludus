#pragma once

#include <ludus/graphics/renderer/scene_shader.hpp>

namespace ludus::graphics::renderer::internal
{
/// Shared ray/forward material admission, including map extents and sampler semantics.
[[nodiscard]] bool IsSceneShaderMaterialValid(const SceneShaderMaterial& material) noexcept;
/// Shared nonnegative finite scene radiance bound, including the authored environment.
[[nodiscard]] bool IsSceneRadianceValid(foundation::math::Vector3 radiance) noexcept;
} // namespace ludus::graphics::renderer::internal
