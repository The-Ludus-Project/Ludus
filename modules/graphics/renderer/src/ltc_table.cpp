#include "internal/ltc_table.h"
#include "internal/ltc_table_data.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::graphics::renderer::internal
{
namespace
{
namespace math = foundation::math;
using foundation::float32;
using foundation::uint32;
using foundation::usize;

constexpr uint32 ROUGHNESS_COUNT = LUDUS_LTC_TABLE_ROUGHNESS_COUNT;
constexpr uint32 VIEW_COUNT = LUDUS_LTC_TABLE_VIEW_COUNT;
constexpr uint32 FRESNEL_COUNT = LUDUS_LTC_TABLE_FRESNEL_COUNT;
constexpr usize SLICE_ENTRY_COUNT = LTC_TABLE_SLICE_ENTRY_COUNT;
constexpr usize MOMENT_OFFSET = LTC_TABLE_MOMENT_OFFSET;
constexpr float32 MINIMUM_ROUGHNESS = LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS;
static_assert(ROUGHNESS_COUNT >= 2 && VIEW_COUNT >= 2 && FRESNEL_COUNT >= 2);
// Rec.709 luminance weights, with which Peters' renderer reduces an RGB F0 to the scalar that selects a slice.
constexpr math::Vector3 FRESNEL_LUMINANCE_WEIGHTS{0.2126F, 0.7152F, 0.0722F};

[[nodiscard]] LtcTableEntry Mix(LtcTableEntry a, LtcTableEntry b, float32 t) noexcept
{
    return LtcTableEntry
    {
        .X = a.X + (b.X - a.X) * t,
        .Y = a.Y + (b.Y - a.Y) * t,
        .Z = a.Z + (b.Z - a.Z) * t,
        .W = a.W + (b.W - a.W) * t,
    };
}
/// Bilinear interpolation of the four entries around (row, column) of the roughness x view grid starting at offset.
// Row then column, each followed by its fraction, is the table's (roughness, view cosine) order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] LtcTableEntry
Interpolate(usize offset, uint32 row, float32 rowFraction, uint32 column, float32 columnFraction) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const auto at = [offset](uint32 r, uint32 c) noexcept { return LTC_TABLE[offset + usize{r} * VIEW_COUNT + c]; };
    return Mix(Mix(at(row, column), at(row, column + 1), columnFraction),
               Mix(at(row + 1, column), at(row + 1, column + 1), columnFraction),
               rowFraction);
}
} // namespace

uint32 GetLtcTableFresnelSlice(math::Vector3 f0) noexcept
{
    const float32 luminance = math::Clamp(math::Dot(f0, FRESNEL_LUMINANCE_WEIGHTS), 0.0F, 1.0F);
    // The coordinate is non-negative, so truncating it plus one half rounds to nearest. It is the Slang lookup's
    // uint(x + 0.5) in ltc_table.slang, so the CPU and GPU choose the same slice.
    // NOLINTNEXTLINE(bugprone-incorrect-roundings)
    return math::Min(static_cast<uint32>(luminance * static_cast<float32>(FRESNEL_COUNT - 1) + 0.5F),
                     FRESNEL_COUNT - 1);
}

// The view direction and F0 are distinct quantities, in the order of the Slang LookupSpecularLtc.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
SpecularLtc LookupSpecularLtc(float32 roughness, math::Vector3 viewTangent, math::Vector3 f0) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    // Rows are linear in perceptual roughness, columns linear in the view inclination (Heitz et al. 2016, section 4).
    const float32 rowCoordinate =
        math::Clamp((roughness - MINIMUM_ROUGHNESS) / (1.0F - MINIMUM_ROUGHNESS), 0.0F, 1.0F) * (ROUGHNESS_COUNT - 1);
    const float32 sine = math::Sqrt(viewTangent.X * viewTangent.X + viewTangent.Y * viewTangent.Y);
    const float32 theta = math::Atan2(sine, math::Max(viewTangent.Z, 0.0F));
    const float32 columnCoordinate = math::Clamp(theta / math::kHalfPiF, 0.0F, 1.0F) * (VIEW_COUNT - 1);
    const uint32 row = math::Min(static_cast<uint32>(rowCoordinate), ROUGHNESS_COUNT - 2);
    const uint32 column = math::Min(static_cast<uint32>(columnCoordinate), VIEW_COUNT - 2);
    const float32 rowFraction = rowCoordinate - static_cast<float32>(row);
    const float32 columnFraction = columnCoordinate - static_cast<float32>(column);
    const usize transformOffset = usize{GetLtcTableFresnelSlice(f0)} * SLICE_ENTRY_COUNT;
    const LtcTableEntry transform = Interpolate(transformOffset, row, rowFraction, column, columnFraction);
    const LtcTableEntry moments = Interpolate(MOMENT_OFFSET, row, rowFraction, column, columnFraction);
    // M^-1 in the table's frame is [[1, 0, M02], [0, M11, 0], [M20, 0, M22]]. The tangent frame is that frame rotated
    // about z by the view's azimuth R, so the tangent-space inverse is M^-1 R^T.
    const math::Matrix3 inverseCanonical{{
        math::Vector3{1.0F, 0.0F, transform.Z},
        math::Vector3{0.0F, transform.Y, 0.0F},
        math::Vector3{transform.X, 0.0F, transform.W},
    }};
    const bool hasAzimuth = sine > 0.0F;
    const float32 cosineAzimuth = hasAzimuth == true ? viewTangent.X / sine : 1.0F;
    const float32 sineAzimuth = hasAzimuth == true ? viewTangent.Y / sine : 0.0F;
    const math::Matrix3 rotationTransposed{{
        math::Vector3{cosineAzimuth, -sineAzimuth, 0.0F},
        math::Vector3{sineAzimuth, cosineAzimuth, 0.0F},
        math::Vector3{0.0F, 0.0F, 1.0F},
    }};
    SpecularLtc result;
    result.InverseTransform = inverseCanonical * rotationTransposed;
    result.Norm = moments.X;
    result.FresnelNorm = moments.Y;
    return result;
}

math::Vector3 ComputeSpecularLtcAlbedo(const SpecularLtc& ltc, math::Vector3 f0) noexcept
{
    const float32 difference = ltc.Norm - ltc.FresnelNorm;
    return math::Vector3{
        f0.X * difference + ltc.FresnelNorm,
        f0.Y * difference + ltc.FresnelNorm,
        f0.Z * difference + ltc.FresnelNorm,
    };
}
} // namespace ludus::graphics::renderer::internal
