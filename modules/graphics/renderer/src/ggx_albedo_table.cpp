#include "internal/ggx_albedo_table.h"

#include <ludus/foundation/math/scalar.hpp>

// Lookup of RT1's grazing-resolved GGX albedo table; ggx_albedo_table.slang repeats this arithmetic on the GPU.
// The stretched-view columns follow the experiment in docs/development/ltc-table.md, "Can the moments drive GGX energy
// compensation?". Thanks to Christopher Kulla and Alejandro Conty, "Revisiting Physically Based Shading at
// Imageworks", SIGGRAPH 2017, slides 9-14,
// https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_slides_v2.pdf#page=12,
// for tabulating the directional albedo and average deficit that drive the compensation lobe.
namespace ludus::graphics::renderer::internal
{
namespace
{
namespace math = foundation::math;
using foundation::float32;
using foundation::uint32;
using foundation::usize;

constexpr uint32 ROUGHNESS_COUNT = LUDUS_GGX_ALBEDO_TABLE_ROUGHNESS_COUNT;
constexpr uint32 VIEW_COUNT = LUDUS_GGX_ALBEDO_TABLE_VIEW_COUNT;
// The layout macro multiplies in int; index the table in usize.
constexpr usize DEFICIT_OFFSET = usize{ROUGHNESS_COUNT} * VIEW_COUNT;
static_assert(ROUGHNESS_COUNT >= 2 && VIEW_COUNT >= 2);

/// Fractional row of alpha: rows are linear in sqrt(alpha) over [0, 1].
[[nodiscard]] float32 GetRowCoordinate(float32 alpha) noexcept
{
    return math::Sqrt(math::Clamp(alpha, 0.0F, 1.0F)) * float32(ROUGHNESS_COUNT - 1);
}
[[nodiscard]] float32 Mix(float32 a, float32 b, float32 t) noexcept
{
    return a + (b - a) * t;
}
} // namespace

float32 LookupGgxDirectionalAlbedo(float32 alpha, float32 cosine) noexcept
{
    const float32 clampedAlpha = math::Clamp(alpha, 0.0F, 1.0F);
    const float32 clampedCosine = math::Clamp(cosine, 0.0F, 1.0F);
    const float32 sine = math::Sqrt(math::Max(0.0F, 1 - clampedCosine * clampedCosine));
    const float32 stretchedInclination = math::Atan2(clampedAlpha * sine, clampedCosine);
    const float32 x = 1 - math::Sqrt(math::Max(0.0F, 1 - stretchedInclination / math::kHalfPiF));
    const float32 columnCoordinate = math::Clamp(x, 0.0F, 1.0F) * float32(VIEW_COUNT - 1);
    const float32 rowCoordinate = GetRowCoordinate(clampedAlpha);
    const uint32 row = math::Min(uint32(rowCoordinate), ROUGHNESS_COUNT - 2);
    const uint32 column = math::Min(uint32(columnCoordinate), VIEW_COUNT - 2);
    const float32 rowFraction = rowCoordinate - float32(row);
    const float32 columnFraction = columnCoordinate - float32(column);
    const usize low = usize{row} * VIEW_COUNT + column;
    const usize high = low + VIEW_COUNT;
    const float32 lowRow = Mix(GGX_ALBEDO_TABLE[low], GGX_ALBEDO_TABLE[low + 1], columnFraction);
    const float32 highRow = Mix(GGX_ALBEDO_TABLE[high], GGX_ALBEDO_TABLE[high + 1], columnFraction);
    return math::Clamp(Mix(lowRow, highRow, rowFraction), 0.0F, 1.0F);
}
float32 LookupGgxAverageDeficit(float32 alpha) noexcept
{
    const float32 rowCoordinate = GetRowCoordinate(alpha);
    const uint32 row = math::Min(uint32(rowCoordinate), ROUGHNESS_COUNT - 2);
    const float32 rowFraction = rowCoordinate - float32(row);
    const usize low = DEFICIT_OFFSET + row;
    return math::Clamp(Mix(GGX_ALBEDO_TABLE[low], GGX_ALBEDO_TABLE[low + 1], rowFraction), 0.0F, 1.0F);
}
} // namespace ludus::graphics::renderer::internal
