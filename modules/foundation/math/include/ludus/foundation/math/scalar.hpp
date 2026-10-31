#pragma once

// Scalar vocabulary for FoundationMath: constants, angle conversion, the
// trusted finite-input helpers (Abs/Min/Max/Clamp/finite detection), the libm
// wrappers used by the rest of the module (Sqrt/Sin/Cos/Atan2), and the
// interpolation helpers (Lerp, SmoothStep01, ranged smoothstep, NearlyEqual,
// TryApproachExponential).
//
// Numerical contract (ADR 0010 §4):
//  - float32/float64 are IEEE binary32/64, round-to-nearest, NaN/Inf/signed-zero
//    preserved. This module never changes rounding, FTZ/DAZ or FP traps.
//  - There is NO universal epsilon. NearlyEqual takes explicit absolute and
//    relative tolerances; comparisons at geometric call sites pass their own.
//  - Cheap pure-arithmetic helpers are inline/constexpr. Sqrt/Sin/Cos/Atan2 and
//    the overflow-conscious Lerp/approach reference live in scalar.cpp so they
//    are not forced constexpr and inherit the module's precise .cpp flags.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/status.hpp>

// FoundationMath's checked algorithms assume ordinary IEEE semantics. Reject a
// translation unit that compiled this header under fast math or finite-only
// math: those change NaN/Inf, signed-zero and reassociation assumptions the
// module's contracts depend on. (See ADR 0010 §4.)
#if defined(__FAST_MATH__)
#    error "FoundationMath requires IEEE floating point; do not compile consumers with -ffast-math."
#endif
#if defined(__FINITE_MATH_ONLY__) && (__FINITE_MATH_ONLY__ != 0)
#    error "FoundationMath relies on NaN/Inf detection; do not compile consumers with -ffinite-math-only."
#endif

namespace ludus::foundation::math
{
using core::float32;
using core::float64;

// --- Constants --------------------------------------------------------------
// Pi/Tau at both widths. The float32 values are the correctly-rounded
// nearest-representable constants, not truncations of the double literal.
inline constexpr float64 kPi = 3.14159265358979323846;
inline constexpr float64 kTau = 6.28318530717958647692;
inline constexpr float64 kHalfPi = 1.57079632679489661923;
inline constexpr float32 kPiF = 3.14159265358979323846f;
inline constexpr float32 kTauF = 6.28318530717958647692f;
inline constexpr float32 kHalfPiF = 1.57079632679489661923f;

// --- Angle conversion (named; no implicit units) ----------------------------
[[nodiscard]] constexpr float32 DegreesToRadians(float32 degrees) noexcept
{
    return degrees * (kPiF / 180.0f);
}
[[nodiscard]] constexpr float32 RadiansToDegrees(float32 radians) noexcept
{
    return radians * (180.0f / kPiF);
}
[[nodiscard]] constexpr float64 DegreesToRadians(float64 degrees) noexcept
{
    return degrees * (kPi / 180.0);
}
[[nodiscard]] constexpr float64 RadiansToDegrees(float64 radians) noexcept
{
    return radians * (180.0 / kPi);
}

// --- Trusted finite-input helpers -------------------------------------------
// Abs/Min/Max are ordinary arithmetic. For a NaN operand Min/Max follow the
// comparison, so callers that must exclude NaN validate with IsFinite first.
[[nodiscard]] constexpr float32 Abs(float32 value) noexcept
{
    return value < 0.0f ? -value : value;
}
[[nodiscard]] constexpr float64 Abs(float64 value) noexcept
{
    return value < 0.0 ? -value : value;
}
template <typename ScalarType>
[[nodiscard]] constexpr ScalarType Min(ScalarType a, ScalarType b) noexcept
{
    return b < a ? b : a;
}
template <typename ScalarType>
[[nodiscard]] constexpr ScalarType Max(ScalarType a, ScalarType b) noexcept
{
    return a < b ? b : a;
}

/// @brief Clamp value into the inclusive interval [lo, hi].
///
/// Finite operands and ordered bounds are caller obligations. Reversed bounds
/// trigger a development assertion; there is no checked-domain status result.
template <typename ScalarType>
[[nodiscard]] constexpr ScalarType Clamp(ScalarType value, ScalarType lo, ScalarType hi) noexcept
{
    LUDUS_ASSERT((hi < lo) == false);
    return value < lo ? lo : (hi < value ? hi : value);
}

/// True for a finite value; false for NaN and either infinity.
[[nodiscard]] constexpr bool IsFinite(float32 value) noexcept
{
    return value == value && value < __builtin_huge_valf() && value > -__builtin_huge_valf();
}
/// @copydoc ludus::foundation::math::IsFinite(float32)
[[nodiscard]] constexpr bool IsFinite(float64 value) noexcept
{
    return value == value && value < __builtin_huge_val() && value > -__builtin_huge_val();
}
[[nodiscard]] constexpr bool IsNaN(float32 value) noexcept
{
    return value != value;
}
[[nodiscard]] constexpr bool IsNaN(float64 value) noexcept
{
    return value != value;
}

// --- libm wrappers (defined in scalar.cpp under precise FP flags) -----------
// Documented domains: Sqrt requires value >= 0 (NaN otherwise, per libm);
// Sin/Cos accept any finite angle (large angles inherit libm argument
// reduction); Atan2 is the full four-quadrant arctangent.
[[nodiscard]] float32 Sqrt(float32 value) noexcept;
[[nodiscard]] float64 Sqrt(float64 value) noexcept;
[[nodiscard]] float32 Sin(float32 radians) noexcept;
[[nodiscard]] float64 Sin(float64 radians) noexcept;
[[nodiscard]] float32 Cos(float32 radians) noexcept;
[[nodiscard]] float64 Cos(float64 radians) noexcept;
[[nodiscard]] float32 Atan2(float32 y, float32 x) noexcept;
[[nodiscard]] float64 Atan2(float64 y, float64 x) noexcept;

// Trusted libm wrappers. Callers validate their domain/range at ingress.
[[nodiscard]] float64 Exp(float64 value) noexcept;
/// Natural logarithm with the platform libm log (NaN for negative input, -infinity at zero).
[[nodiscard]] float64 Log(float64 value) noexcept;
[[nodiscard]] float64 Floor(float64 value) noexcept;
[[nodiscard]] float64 Ceil(float64 value) noexcept;
/// Raise base to exponent with the platform libm pow (IEEE special cases apply).
/// Offline cookers use it to reproduce reference transfer functions bit for bit.
[[nodiscard]] float64 Pow(float64 base, float64 exponent) noexcept;

/// Decode a finite [0,1] sRGB channel into linear light with the IEC/W3C piecewise
/// transfer. Output stays in [0,1], with exactly preserved black/white endpoints.
/// Values outside this bounded domain return NaN. No allocation/state.
/// Alpha/data channels must not use this color transfer.
[[nodiscard]] float32 DecodeSrgb(float32 value) noexcept;
/// Encode a finite [0,1] linear-light channel into bounded sRGB. Outside-domain
/// values return NaN; exposure/tone mapping remain the renderer's responsibility.
[[nodiscard]] float32 EncodeSrgb(float32 value) noexcept;
/// Adjacent float32 toward negative infinity. Finite inputs have the IEEE nextafter
/// result, including subnormals/zero; NaN remains NaN. No allocation/state. Callers
/// retain interval/domain obligations when excluding a visibility endpoint.
[[nodiscard]] float32 NextFloatDown(float32 value) noexcept;

/// @brief Inverse cumulative distribution function (quantile) of the standard normal distribution.
///
/// Returns the x with Phi(x) = probability, where Phi is the standard normal CDF, by Wichura's rational
/// approximation (Algorithm AS241, PPND16; about 1e-16 relative error in float64). The float32 overload evaluates
/// the same coefficients in float32 arithmetic, as FoundationMath's shader ComputeStandardNormalQuantile does, and is
/// accurate to a few float32 ulp. The domain is the open interval (0, 1): 0, 1, values outside it and NaN return
/// NaN, so a caller that maps a [0, 1) random number must first move it into the open interval. Pure, allocation free.
[[nodiscard]] float64 ComputeStandardNormalQuantile(float64 probability) noexcept;
/// @copydoc ludus::foundation::math::ComputeStandardNormalQuantile(float64)
[[nodiscard]] float32 ComputeStandardNormalQuantile(float32 probability) noexcept;

// --- Interpolation ----------------------------------------------------------
/// @brief Lerp is UNCLAMPED and preserves exactly a at t=0 and b at t=1 for finite inputs (it is not the naive a +
/// t*(b-a), which loses the b endpoint). t outside [0,1] extrapolates and may overflow.
///
/// Defined in scalar.cpp.
[[nodiscard]] float32 Lerp(float32 a, float32 b, float32 t) noexcept;
/// @copydoc ludus::foundation::math::Lerp(float32, float32, float32)
[[nodiscard]] float64 Lerp(float64 a, float64 b, float64 t) noexcept;

/// @brief SmoothStep01 clamps finite t into [0,1] then returns t*t*(3-2*t).
///
/// A NaN t yields NaN. Defined in scalar.cpp.
[[nodiscard]] float32 SmoothStep01(float32 t) noexcept;
/// @copydoc ludus::foundation::math::SmoothStep01(float32)
[[nodiscard]] float64 SmoothStep01(float64 t) noexcept;

/// @brief TrySmoothStep maps x through the Hermite smoothstep over [edge0, edge1], clamping to [0,1].
///
/// Rejects non-finite inputs (NonFiniteInput) and equal or reversed bounds (InvalidArgument). On failure *out is
/// unchanged.
[[nodiscard]] MathStatus TrySmoothStep(float32 edge0, float32 edge1, float32 x, float32& out) noexcept;
/// @copydoc ludus::foundation::math::TrySmoothStep(float32, float32, float32, float32&)
[[nodiscard]] MathStatus TrySmoothStep(float64 edge0, float64 edge1, float64 x, float64& out) noexcept;

/// @brief NearlyEqual: abs(a-b) <= max(absTol, relTol*max(abs(a),abs(b))).
///
/// Negative or non-finite tolerances or operands return false; infinities are never nearly equal (even two
/// identical infinities return false, since the comparison is about finite proximity). float32 uses a float64
/// intermediate. There is no default tolerance — callers must pass tolerances meaningful for their units.
[[nodiscard]] bool NearlyEqual(float32 a, float32 b, float32 absTol, float32 relTol) noexcept;
/// @copydoc ludus::foundation::math::NearlyEqual(float32, float32, float32, float32)
[[nodiscard]] bool NearlyEqual(float64 a, float64 b, float64 absTol, float64 relTol) noexcept;

/// @brief TryApproachExponential: exact first-order exponential decay of `current` toward a constant `target` over
/// dt at the given rate.
///
/// Requires finite current and target, and finite non-negative rate and dt (else NonFiniteInput / InvalidArgument,
/// output unchanged). rate==0 or dt==0 preserves current. The blend is computed as -expm1(-rate*dt) in float64; a
/// very large rate*dt saturates the blend to 1 without forming an invalid product. This is NOT a spring, velocity
/// model or moving-target integrator.
[[nodiscard]] MathStatus
TryApproachExponential(float32 current, float32 target, float32 rate, float32 dt, float32& out) noexcept;
/// @copydoc ludus::foundation::math::TryApproachExponential(float32, float32, float32, float32, float32&)
[[nodiscard]] MathStatus
TryApproachExponential(float64 current, float64 target, float64 rate, float64 dt, float64& out) noexcept;
} // namespace ludus::foundation::math
