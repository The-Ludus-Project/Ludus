#pragma once

#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

namespace ludus::graphics::renderer::internal
{
/// Append positive-area authored emitter triangles, preserving existing words on failure.
/// Textured triangles split into four patches with fixed-mip centroid emission.
[[nodiscard]] rhi::RasterStatus
AppendRasterMeshLights(const MeshDescription& mesh,
                       const foundation::math::Affine3& meshToWorld,
                       const SceneShaderMaterial& material,
                       foundation::Array<foundation::math::Vector4>& inoutWords) noexcept;
/// CPU check of the same unshadowed diffuse/LTC approximation used by the raster resolve.
[[nodiscard]] foundation::math::Vector3
EvaluateRasterMeshLight(foundation::BorrowedView<const foundation::math::Vector4> words,
                        foundation::math::Vector3 positionWorld,
                        foundation::math::Vector3 normalWorld,
                        foundation::math::Vector3 viewWorld,
                        foundation::math::Vector3 baseColor,
                        foundation::float32 metallic,
                        foundation::float32 roughness) noexcept;
} // namespace ludus::graphics::renderer::internal
