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
/// CPU check of the resolve's far-field evaluation of exactly one record. When the record's bounding sphere centre
/// lies farther than LUDUS_MESH_LIGHT_FAR_DISTANCE_RATIO radii from positionWorld, and, for a receiver with a
/// specular lobe, the sphere's angular radius is below LUDUS_MESH_LIGHT_FAR_SPECULAR_ANGLE_PER_ALPHA times the GGX
/// alpha (roughness squared), the polygon is integrated as a point at the centre: the differential form factor
/// area * emitter cosine / distance^2 times the Lambert cosine, plus the specular LTC density in that direction.
/// Thanks to Michael F. Cohen and John R. Wallace, "Radiosity and Realistic Image Synthesis", Academic Press, 1993,
/// chapter 4 (the differential-area form factor), and to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
/// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", TOG 35(4), Article 41 (2016), section 3.1,
/// https://doi.org/10.1145/2897824.2925895, for the LTC density.
/// @return True with outContribution set, zero for a receiver the record cannot reach, when the point is used; false,
/// leaving outContribution unchanged, when the record is too near and EvaluateRasterMeshLight applies, or for a
/// malformed record.
[[nodiscard]] bool TryEvaluateRasterMeshLightFar(foundation::BorrowedView<const foundation::math::Vector4> words,
                                                 foundation::math::Vector3 positionWorld,
                                                 foundation::math::Vector3 normalWorld,
                                                 foundation::math::Vector3 viewWorld,
                                                 foundation::math::Vector3 baseColor,
                                                 foundation::float32 metallic,
                                                 foundation::float32 roughness,
                                                 foundation::math::Vector3& outContribution) noexcept;
} // namespace ludus::graphics::renderer::internal
