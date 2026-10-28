#include "internal/ltc_table.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::graphics::renderer::internal
{
namespace
{
namespace math = foundation::math;
using foundation::float32;
using foundation::uint32;
using foundation::usize;

/// One four-float entry of the generated table.
struct LtcTableEntry final
{
    float32 X = 0.0F;
    float32 Y = 0.0F;
    float32 Z = 0.0F;
    float32 W = 0.0F;
};

#define LUDUS_LTC_TABLE_ENTRY LtcTableEntry
#include "../shaders/ltc_table_data.inc"
#undef LUDUS_LTC_TABLE_ENTRY

constexpr uint32 ROUGHNESS_COUNT = LUDUS_LTC_TABLE_ROUGHNESS_COUNT;
constexpr uint32 VIEW_COUNT = LUDUS_LTC_TABLE_VIEW_COUNT;
constexpr uint32 ENTRIES_PER_CELL = LUDUS_LTC_TABLE_ENTRIES_PER_CELL;
constexpr float32 MINIMUM_ROUGHNESS = LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS;
static_assert(ROUGHNESS_COUNT >= 2 && VIEW_COUNT >= 2);

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
/// Bilinear interpolation of one entry of the four cells around (row, column).
[[nodiscard]] LtcTableEntry
Interpolate(uint32 row, float32 rowFraction, uint32 column, float32 columnFraction, uint32 entry) noexcept
{
    const auto at = [entry](uint32 r, uint32 c) noexcept {
        return LUDUS_LTC_TABLE[(usize{r} * VIEW_COUNT + c) * ENTRIES_PER_CELL + entry];
    };
    return Mix(Mix(at(row, column), at(row, column + 1), columnFraction),
               Mix(at(row + 1, column), at(row + 1, column + 1), columnFraction),
               rowFraction);
}
} // namespace

SpecularLtc LookupSpecularLtc(float32 roughness, math::Vector3 viewTangent) noexcept
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
    const LtcTableEntry transform = Interpolate(row, rowFraction, column, columnFraction, 0);
    const LtcTableEntry moments = Interpolate(row, rowFraction, column, columnFraction, 1);
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
