#pragma once

#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

namespace ludus::graphics::renderer::internal
{
/// Append variable-length emitter polygon records (deferred_layout.h) for positive-area authored emitter triangles,
/// adding the number appended to inoutRecordCount. Constant-emission coplanar regions merge into convex polygons
/// (TryPartitionRasterEmitterRegions); other triangles stay three-corner records, and textured or masked ones split
/// into four fixed-mip centroid patches. Masked patches test texture and interpolated vertex coverage before emitting
/// a record; subpatch variation is approximated. Both outputs are preserved on failure.
[[nodiscard]] rhi::RasterStatus AppendRasterMeshLights(const MeshDescription& mesh,
                                                       const foundation::math::Affine3& meshToWorld,
                                                       const SceneShaderMaterial& material,
                                                       foundation::Array<foundation::math::Vector4>& inoutWords,
                                                       foundation::uint32& inoutRecordCount) noexcept;
/// Words of the record starting at offset, or zero for a malformed corner count or a truncated record.
[[nodiscard]] foundation::usize
GetRasterMeshLightRecordWords(foundation::BorrowedView<const foundation::math::Vector4> words,
                              foundation::usize offset) noexcept;
/// CPU check of the same unshadowed diffuse/LTC integrals used by the raster resolve, for exactly one record. It fans
/// the record into clip-safe pieces for FoundationMath's sampling preparation instead of streaming its edges; any
/// other view contributes zero.
[[nodiscard]] foundation::math::Vector3
EvaluateRasterMeshLight(foundation::BorrowedView<const foundation::math::Vector4> words,
                        foundation::math::Vector3 positionWorld,
                        foundation::math::Vector3 normalWorld,
                        foundation::math::Vector3 viewWorld,
                        foundation::math::Vector3 baseColor,
                        foundation::float32 metallic,
                        foundation::float32 roughness) noexcept;
} // namespace ludus::graphics::renderer::internal
