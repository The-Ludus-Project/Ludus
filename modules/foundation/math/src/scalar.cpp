// Scalar reference implementations. Compiled with precise FP flags
// (-ffp-contract=off, no fast math; see CMakeLists.txt) so the documented
// numerical behaviour holds. <cmath>/<limits> are permitted here (ADR 0003,
// ADR 0010): standard libm functions, no allocation, no exceptions.

#include <ludus/foundation/math/scalar.hpp>

#include <cmath>
#include <limits>

namespace ludus::foundation::math
{
float32 Sqrt(float32 value) noexcept
{
    return std::sqrt(value);
}
float64 Sqrt(float64 value) noexcept
{
    return std::sqrt(value);
}
float32 Sin(float32 radians) noexcept
{
    return std::sin(radians);
}
float64 Sin(float64 radians) noexcept
{
    return std::sin(radians);
}
float32 Cos(float32 radians) noexcept
{
    return std::cos(radians);
}
float64 Cos(float64 radians) noexcept
{
    return std::cos(radians);
}
float32 Atan2(float32 y, float32 x) noexcept
{
    return std::atan2(y, x);
}
float64 Atan2(float64 y, float64 x) noexcept
{
    return std::atan2(y, x);
}

namespace
{
// Endpoint-preserving lerp. a + t*(b-a) does not reproduce b exactly at t==1
// for all finite inputs; the two-sided form (1-t)*a + t*b does, and is the
// standard overflow-conscious choice. Done at the operand width.
template <typename ScalarType>
[[nodiscard]] ScalarType LerpImpl(ScalarType a, ScalarType b, ScalarType t) noexcept
{
    return (ScalarType{1} - t) * a + t * b;
}

template <typename ScalarType>
[[nodiscard]] ScalarType SmoothStep01Impl(ScalarType t) noexcept
{
    // Clamp first; a NaN t propagates (both comparisons false -> returns NaN
    // through the arithmetic below only if not clamped, so handle explicitly).
    if (std::isnan(t))
    {
        return t;
    }
    const ScalarType u = t < ScalarType{0} ? ScalarType{0} : (t > ScalarType{1} ? ScalarType{1} : t);
    return u * u * (ScalarType{3} - ScalarType{2} * u);
}

template <typename ScalarType>
[[nodiscard]] bool NearlyEqualImpl(ScalarType a, ScalarType b, ScalarType absTol, ScalarType relTol) noexcept
{
    if (std::isfinite(a) == false || std::isfinite(b) == false || std::isfinite(absTol) == false ||
        std::isfinite(relTol) == false)
    {
        return false;
    }
    if (absTol < ScalarType{0} || relTol < ScalarType{0})
    {
        return false;
    }
    // Widen float32 comparisons so the subtraction and max do not lose bits.
    const float64 wa = static_cast<float64>(a);
    const float64 wb = static_cast<float64>(b);
    const float64 diff = std::fabs(wa - wb);
    const float64 scale = std::fmax(std::fabs(wa), std::fabs(wb));
    const float64 tol = std::fmax(static_cast<float64>(absTol), static_cast<float64>(relTol) * scale);
    return diff <= tol;
}

template <typename ScalarType>
[[nodiscard]] MathStatus TrySmoothStepImpl(ScalarType edge0, ScalarType edge1, ScalarType x, ScalarType& out) noexcept
{
    if (std::isfinite(edge0) == false || std::isfinite(edge1) == false || std::isfinite(x) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if ((edge0 < edge1) == false)
    {
        return MathStatus::InvalidArgument;
    }
    const ScalarType span = edge1 - edge0;
    ScalarType u = (x - edge0) / span;
    u = u < ScalarType{0} ? ScalarType{0} : (u > ScalarType{1} ? ScalarType{1} : u);
    out = u * u * (ScalarType{3} - ScalarType{2} * u);
    return MathStatus::Success;
}

template <typename ScalarType>
[[nodiscard]] MathStatus TryApproachExponentialImpl(ScalarType current,
                                                    ScalarType target,
                                                    ScalarType rate,
                                                    ScalarType dt,
                                                    ScalarType& out) noexcept
{
    if (std::isfinite(current) == false || std::isfinite(target) == false || std::isfinite(rate) == false ||
        std::isfinite(dt) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (rate < ScalarType{0} || dt < ScalarType{0})
    {
        return MathStatus::InvalidArgument;
    }
    // rate==0 or dt==0 -> no movement.
    if (rate == ScalarType{0} || dt == ScalarType{0})
    {
        out = current;
        return MathStatus::Success;
    }
    // blend = 1 - exp(-rate*dt), computed via -expm1 for accuracy near 0. A huge
    // product saturates expm1(-x) toward -1, so blend -> 1, without forming an
    // invalid product (expm1 of -inf is -1). Work in float64 then narrow.
    const float64 product = static_cast<float64>(rate) * static_cast<float64>(dt);
    const float64 blend = -std::expm1(-product);
    const float64 result =
        static_cast<float64>(current) + blend * (static_cast<float64>(target) - static_cast<float64>(current));
    out = static_cast<ScalarType>(result);
    return MathStatus::Success;
}
} // namespace

float32 Lerp(float32 a, float32 b, float32 t) noexcept
{
    return LerpImpl(a, b, t);
}
float64 Lerp(float64 a, float64 b, float64 t) noexcept
{
    return LerpImpl(a, b, t);
}
float32 SmoothStep01(float32 t) noexcept
{
    return SmoothStep01Impl(t);
}
float64 SmoothStep01(float64 t) noexcept
{
    return SmoothStep01Impl(t);
}
MathStatus TrySmoothStep(float32 edge0, float32 edge1, float32 x, float32& out) noexcept
{
    return TrySmoothStepImpl(edge0, edge1, x, out);
}
MathStatus TrySmoothStep(float64 edge0, float64 edge1, float64 x, float64& out) noexcept
{
    return TrySmoothStepImpl(edge0, edge1, x, out);
}
bool NearlyEqual(float32 a, float32 b, float32 absTol, float32 relTol) noexcept
{
    return NearlyEqualImpl(a, b, absTol, relTol);
}
bool NearlyEqual(float64 a, float64 b, float64 absTol, float64 relTol) noexcept
{
    return NearlyEqualImpl(a, b, absTol, relTol);
}
MathStatus TryApproachExponential(float32 current, float32 target, float32 rate, float32 dt, float32& out) noexcept
{
    return TryApproachExponentialImpl(current, target, rate, dt, out);
}
MathStatus TryApproachExponential(float64 current, float64 target, float64 rate, float64 dt, float64& out) noexcept
{
    return TryApproachExponentialImpl(current, target, rate, dt, out);
}
float64 Exp(float64 value) noexcept
{
    return std::exp(value);
}
float64 Log(float64 value) noexcept
{
    return std::log(value);
}
float64 Floor(float64 value) noexcept
{
    return std::floor(value);
}
float64 Pow(float64 base, float64 exponent) noexcept
{
    return std::pow(base, exponent);
}
float64 Ceil(float64 value) noexcept
{
    return std::ceil(value);
}
} // namespace ludus::foundation::math
