#pragma once

// The renderer's GGX linearly transformed cosine (LTC) table.
//
// One LTC per perceptual roughness, view inclination and Schlick F0 approximates
// the specular lobe of the reference BSDF (GGX, height-correlated Smith, Schlick;
// see reference.hpp) times the cosine. Thanks to Eric Heitz, Jonathan Dupuy,
// Stephen Hill and David Neubelt, "Real-Time Polygonal-Light Shading with Linearly
// Transformed Cosines", ACM Transactions on Graphics 35(4), Article 41, 2016,
// section 4 (https://doi.org/10.1145/2897824.2925895), for the fit and its axes,
// and to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM
// Transactions on Graphics 40(4), Article 140, 2021, section 4.1
// (https://doi.org/10.1145/3450626.3459672), for the F0 axis and the 64 x 64 x 51
// resolution. tools/ltc-fitter fits the table offline; the checked-in data lives
// in modules/graphics/renderer/data with its provenance.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
/// @brief Format version of the table data and its lookup.
inline constexpr ludus::foundation::uint32 LTC_TABLE_VERSION = 1;
/// @brief Perceptual roughness sqrt(alpha) samples, uniform over [0, 1] (Heitz et al. section 4).
inline constexpr ludus::foundation::usize LTC_TABLE_ROUGHNESS_COUNT = 64;
/// @brief View inclination theta samples, uniform over [0, pi/2] (Heitz et al. section 4).
inline constexpr ludus::foundation::usize LTC_TABLE_INCLINATION_COUNT = 64;
/// @brief Schlick F0 samples, uniform over [0, 1] (Peters section 4.1).
inline constexpr ludus::foundation::usize LTC_TABLE_FRESNEL_COUNT = 51;
/// @brief Fitted transforms: one per roughness, inclination and F0.
inline constexpr ludus::foundation::usize LTC_TABLE_TRANSFORM_COUNT =
    LTC_TABLE_ROUGHNESS_COUNT * LTC_TABLE_INCLINATION_COUNT * LTC_TABLE_FRESNEL_COUNT;
/// @brief Albedo entries: one per roughness and inclination.
inline constexpr ludus::foundation::usize LTC_TABLE_ALBEDO_COUNT =
    LTC_TABLE_ROUGHNESS_COUNT * LTC_TABLE_INCLINATION_COUNT;
/// @brief Little-endian float32 values per serialized transform entry.
inline constexpr ludus::foundation::usize LTC_TABLE_TRANSFORM_FLOATS = 4;
/// @brief Little-endian float32 values per serialized albedo entry.
inline constexpr ludus::foundation::usize LTC_TABLE_ALBEDO_FLOATS = 2;
/// @brief Size of the serialized table: every transform entry, then every albedo entry.
inline constexpr ludus::foundation::usize LTC_TABLE_BYTES =
    (LTC_TABLE_TRANSFORM_COUNT * LTC_TABLE_TRANSFORM_FLOATS + LTC_TABLE_ALBEDO_COUNT * LTC_TABLE_ALBEDO_FLOATS) *
    sizeof(ludus::foundation::float32);

/// @brief The four free entries of one inverse transform, scaled so its last diagonal entry is one.
///
/// M^-1 = [[InverseM00, 0, InverseM02], [0, InverseM11, 0], [InverseM20, 0, 1]], the
/// isotropic form of Heitz et al. Equation (6), in the LTC tangent frame (see
/// TryMakeLtcTangentFrame). Entry (roughness r, inclination i, F0 f) is at index
/// (f * LTC_TABLE_INCLINATION_COUNT + i) * LTC_TABLE_ROUGHNESS_COUNT + r.
struct LtcTableEntry final
{
    ludus::foundation::float32 InverseM00 = 1.0F; ///< Row 0, column 0.
    ludus::foundation::float32 InverseM02 = 0.0F; ///< Row 0, column 2.
    ludus::foundation::float32 InverseM11 = 1.0F; ///< Row 1, column 1.
    ludus::foundation::float32 InverseM20 = 0.0F; ///< Row 2, column 0.
};

/// @brief The specular albedo of one roughness and inclination, split by Schlick's F0.
///
/// The albedo integral of the cosine-weighted specular lobe is F0 * FresnelScale +
/// FresnelBias for each color channel. Entry (r, i) is at index i * LTC_TABLE_ROUGHNESS_COUNT + r.
struct LtcAlbedoEntry final
{
    ludus::foundation::float32 FresnelScale = 0.0F; ///< Integral of D V cos (1 - (1 - v.h)^5).
    ludus::foundation::float32 FresnelBias = 0.0F;  ///< Integral of D V cos (1 - v.h)^5.
};

/// @brief The embedded transforms, LTC_TABLE_TRANSFORM_COUNT entries with static storage.
[[nodiscard]] ludus::foundation::BorrowedView<const LtcTableEntry> GetLtcTableTransforms() noexcept;
/// @brief The embedded albedo split, LTC_TABLE_ALBEDO_COUNT entries with static storage.
[[nodiscard]] ludus::foundation::BorrowedView<const LtcAlbedoEntry> GetLtcTableAlbedo() noexcept;

/// @brief The specular LTC of one shading point.
struct SpecularLtc final
{
    ludus::foundation::math::Matrix3 InverseTransform{}; ///< M^-1 in the LTC tangent frame.
    ludus::foundation::math::Vector3 Albedo{};           ///< Linear RGB specular albedo, the LTC's scale.
};

/// @brief World-to-tangent rotation of the LTC frame: z along the normal, the view in the xz half-plane with x >= 0.
///
/// When the view is parallel to the normal, any perpendicular x is chosen.
/// @param normalWorld Unit shading normal.
/// @param viewWorld Unit direction toward the viewer.
/// @param outWorldToTangent Rows are the tangent, bitangent and normal; written only on success.
/// @return Ready, or InvalidDescription for a nonfinite or non-unit input. outWorldToTangent is unchanged on failure.
[[nodiscard]] rhi::RasterStatus TryMakeLtcTangentFrame(ludus::foundation::math::Vector3 normalWorld,
                                                       ludus::foundation::math::Vector3 viewWorld,
                                                       ludus::foundation::math::Matrix3& outWorldToTangent) noexcept;

/// @brief Interpolate the specular LTC for a reference BSDF.
///
/// Transforms are interpolated trilinearly in roughness, inclination and the
/// Rec. 709 luminance of F0, as Heitz et al. interpolate M^-1 linearly. The albedo
/// is exact per channel from the Schlick split. Roughness below the reference BSDF's
/// 0.045 floor uses that floor's fit.
/// @param perceptualRoughness sqrt(alpha), in [0, 1].
/// @param viewCosine dot(normal, view), in [0, 1].
/// @param specularF0 Linear RGB Schlick F0, each channel in [0, 1].
/// @param outLtc Written only on success.
/// @return Ready, or InvalidDescription for a nonfinite or out-of-range input. outLtc is unchanged on failure.
[[nodiscard]] rhi::RasterStatus TryLookupSpecularLtc(ludus::foundation::float32 perceptualRoughness,
                                                     ludus::foundation::float32 viewCosine,
                                                     ludus::foundation::math::Vector3 specularF0,
                                                     SpecularLtc& outLtc) noexcept;
} // namespace ludus::graphics::renderer
