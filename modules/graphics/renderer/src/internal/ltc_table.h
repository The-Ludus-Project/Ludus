#pragma once

// CPU lookup of the renderer's linearly transformed cosine (LTC) table, fitted by tools/ltc-fit into
// data/ltc_table.bin and embedded as internal::LTC_TABLE (ltc_table_data.h). shaders/ltc_table.slang performs the same
// lookup for graph and reference shaders on the GPU copy of the table. Thanks to Eric Heitz, Jonathan Dupuy, Stephen
// Hill and David Neubelt, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", ACM Transactions on
// Graphics 35(4), Article 41, 2016 (SIGGRAPH 2016), section 4, https://doi.org/10.1145/2897824.2925895, for the
// table's parameterization and bilinear lookup, and to Christoph Peters, "BRDF Importance Sampling for Polygonal
// Lights", ACM Transactions on Graphics 40(4), Article 140, 2021, section 4.1, https://doi.org/10.1145/3450626.3459672,
// for the F0 axis. See docs/development/ltc-table.md.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::internal
{
/// The specular LTC of one receiver, interpolated from the table.
struct SpecularLtc final
{
    /// M^-1 up to a positive scale, in the caller's tangent frame (+z the shading normal); pass it to
    /// foundation::math::TryMakeLinearlyTransformedCosine.
    foundation::math::Matrix3 InverseTransform = foundation::math::Matrix3::Identity();
    /// Directional albedo of the specular lobe with a Fresnel factor of one.
    foundation::float32 Norm = 0.0F;
    /// The (1 - v.h)^5-weighted albedo; see ComputeSpecularLtcAlbedo.
    foundation::float32 FresnelNorm = 0.0F;
};

/// Look up the specular LTC for a perceptual roughness, a view direction and Schlick's F0.
/// @param roughness Perceptual roughness, clamped to [LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS, 1].
/// @param viewTangent Unit direction towards the viewer in the tangent frame; its cosine is clamped to [0, 1]. The
/// table's frame is rotated about +z so that the view lies in its x-z plane; a view along the normal uses +x.
/// @param f0 Schlick's F0 per channel. The transform comes from the F0 slice nearest the Rec.709 luminance of f0,
/// clamped to [0, 1], as in Peters 2021; roughness and view are interpolated bilinearly within that slice.
[[nodiscard]] SpecularLtc LookupSpecularLtc(foundation::float32 roughness,
                                            foundation::math::Vector3 viewTangent,
                                            foundation::math::Vector3 f0) noexcept;

/// The table slice LookupSpecularLtc uses for f0: the nearest slice to its clamped Rec.709 luminance.
[[nodiscard]] foundation::uint32 GetLtcTableFresnelSlice(foundation::math::Vector3 f0) noexcept;

/// Specular albedo with Schlick's Fresnel per channel: F0 * (Norm - FresnelNorm) + FresnelNorm.
[[nodiscard]] foundation::math::Vector3 ComputeSpecularLtcAlbedo(const SpecularLtc& ltc,
                                                                 foundation::math::Vector3 f0) noexcept;
} // namespace ludus::graphics::renderer::internal
