#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/int128.hpp>

// Exact integer arithmetic for the foliage cook: binary32 inputs are exact dyadic
// rationals, so every conversion below is a single specified rounding of an exact
// value. No floating-point operation affects a cooked byte. Contract:
// docs/architecture/foliage.md, "Fixed-point sampling".
namespace ludus::content::foliage_cook::internal
{
using foundation::float32;
using foundation::int16;
using foundation::int32;
using foundation::int64;
using foundation::uint32;
using foundation::uint64;

/// A finite binary32 value as Mantissa * 2^Exponent exactly; zero has a zero mantissa.
struct DyadicValue final
{
    int64 Mantissa = 0;
    int32 Exponent = 0;
};

/// Exact decomposition of a finite binary32 value.
/// @return False for NaN and infinities; outValue is unchanged then.
[[nodiscard]] bool TryDecomposeFloat32(float32 value, DyadicValue& outValue) noexcept;

/// floor(value / 2^shift) for shift in [1, 127], exactly.
[[nodiscard]] foundation::Int128 ShiftRightFloor(foundation::Int128 value, uint32 shift) noexcept;

/// Nearest integer to value / 2^shift, half-way values toward +infinity, for any shift >= 1.
[[nodiscard]] foundation::Int128 ShiftRightRoundHalfUp(foundation::Int128 value, uint32 shift) noexcept;

/// value * 2^shift, failing on overflow; outValue is unchanged on failure.
[[nodiscard]] bool TryShiftLeft(foundation::Int128 value, uint32 shift, foundation::Int128& outValue) noexcept;

/// @brief Round (origin + quantum * code) * 2^scaleBits to the nearest integer, half-way toward +infinity.
/// @return False when the exact value or the result does not fit int64; outValue is unchanged then.
[[nodiscard]] bool
TryRoundDyadicSum(DyadicValue origin, DyadicValue quantum, int64 code, int32 scaleBits, int64& outValue) noexcept;

/// floor(value / divisor) for a positive divisor.
[[nodiscard]] int64 FloorDivide(int64 value, int64 divisor) noexcept;

/// value - FloorDivide(value, divisor) * divisor, in [0, divisor).
[[nodiscard]] int64 FloorModulo(int64 value, int64 divisor) noexcept;

/// ceil(value * scale / SCALE_UNITS_PER_ONE) for a nonnegative value; exact in uint64.
[[nodiscard]] uint64 ScaleRadius(uint64 value, uint32 scale) noexcept;

/// @brief Slope units floor(FOLIAGE_SLOPE_UNITS_PER_ONE * |gradient| / spacing), at most FOLIAGE_MAXIMUM_SLOPE_UNITS.
/// @pre |gradientX|, |gradientZ| < 2^46 and 0 < spacing < 2^45, all in the same height units per sample.
[[nodiscard]] uint32 ComputeSlopeUnits(int64 gradientX, int64 gradientZ, int64 spacing) noexcept;

/// @brief One horizontal component of the unit up normal (-gradientX, spacing, -gradientZ), truncated toward zero.
///
/// Returns -sign(component) * floor(NORMAL_UNITS * |component| / sqrt(gradientX^2 + spacing^2 + gradientZ^2)),
/// so both truncated components keep the up direction no longer than one.
/// @pre The same bounds as ComputeSlopeUnits; component is gradientX or gradientZ.
[[nodiscard]] int16 ComputeNormalComponent(int64 component, int64 gradientX, int64 gradientZ, int64 spacing) noexcept;

/// @brief Admission threshold at suitability one: floor(density * cellSize^2 * spacing^2 / slots).
///
/// With density in FOLIAGE_DENSITY_UNITS_PER_PLACEMENT per square metre, cellSize in lattice units and
/// spacing in metres per terrain sample, this is 2^32 times the per-slot admission probability.
/// @return False when the threshold exceeds 2^32 (more than one placement per slot); outThreshold is unchanged.
[[nodiscard]] bool TryComputeAdmissionThreshold(uint32 density,
                                                uint32 cellSize,
                                                DyadicValue spacingMetres,
                                                uint32 slots,
                                                uint64& outThreshold) noexcept;
} // namespace ludus::content::foliage_cook::internal
