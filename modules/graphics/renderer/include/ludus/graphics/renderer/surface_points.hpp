#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
struct RenderGraphSceneDescription;
/// One sampled static surface, four explicit sixteen-byte feature records.
/// RGB values are linear, normals unit length, positions in World units. Values
/// remain in authored scale; model-specific normalization belongs to the model adapter.
struct SurfacePoint final
{
    foundation::math::Vector3 PositionWorld{}; ///< Barycentrically sampled transformed position.
    foundation::uint32 TriangleIndex = 0;      ///< Preparation-local triangle identity; not a source mesh index.
    foundation::math::Vector3 NormalWorld{};   ///< Interpolated shading normal, including admitted normal map.
    foundation::uint32 MaterialIndex = 0;      ///< Source material identity or DEFAULT_SCENE_MATERIAL.
    foundation::math::Vector3 Albedo{};   ///< Textured base reflectance, including vertex and instance multipliers.
    foundation::uint32 SourceIdLow = 0;   ///< Low half of the source instance's diagnostic identity.
    foundation::math::Vector3 Emission{}; ///< Authored textured emission, without reflected illumination.
    foundation::uint32 SourceIdHigh = 0;  ///< High half of the source instance's diagnostic identity.
};
/// Owned surface features, reusable across camera and image-resolution changes.
/// This represents geometry/emissive surfaces only; analytic lights and environment
/// remain separate scene inputs and must be handled explicitly by a model adapter.
struct SurfacePointCloud final
{
    foundation::Array<SurfacePoint> Points; ///< Exactly the requested sample count after successful preparation.
    foundation::math::Aabb3 BoundsWorld{};  ///< Bounds of all admitted nondegenerate surface geometry.
    foundation::float64 ProposalAreaWorld =
        0;                                   ///< Unmasked triangle area in squared World units, before mask rejection.
    foundation::uint64 SourceGeneration = 0; ///< Copied scene identity; not a GPU submission number.
};
/// Explicit point-count, sampling-work and logical setup-memory budgets.
struct SurfacePointSampling final
{
    foundation::uint32 Count = 0;           ///< Positive output count, unrelated to triangle vertex count.
    foundation::uint64 Seed = 0;            ///< Deterministic PCG stream seed; no camera/time/global RNG input.
    foundation::uint64 MaximumAttempts = 0; ///< At least Count; masks can require extra proposals.
    foundation::usize MaximumBytes = 0;     ///< Candidate scene, sampling scratch and output logical bytes.
};
/// Sample geometry proportional to transformed triangle area, uniformly within
/// each triangle, rejecting masked holes. Reuses scene material/texture admission
/// and evaluation; no transport, lighting prediction or GPU work is performed.
/// Texture detail and emissive surfaces are included; occluded surfaces are not pruned.
/// @param scene Complete static extraction with HasMaterialInterface=true. Consumed synchronously.
/// @param sampling Positive explicit budgets; source textures are borrowed only for this call.
/// @param outCloud Replaced only on success. Previous output is excluded from MaximumBytes.
/// @return Ready; InvalidDescription for malformed input or empty/non-sampleable geometry;
/// CapacityExceeded for memory/work exhaustion (raise the corresponding budget or
/// reduce Count; fully masked geometry may have no accepted points); OutOfMemory;
/// Unsupported for source capabilities rejected by ordinary scene admission.
[[nodiscard]] rhi::RasterStatus PrepareSurfacePointCloud(const RenderGraphSceneDescription& scene,
                                                         const SurfacePointSampling& sampling,
                                                         SurfacePointCloud& outCloud) noexcept;
} // namespace ludus::graphics::renderer
