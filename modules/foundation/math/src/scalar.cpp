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

namespace
{
// Thanks to Michael J. Wichura, "Algorithm AS 241: The Percentage Points of the Normal Distribution", Journal of the
// Royal Statistical Society, Series C (Applied Statistics) 37(3), 1988, pp. 477-484, https://doi.org/10.2307/2347330 :
// PPND16, three rational approximations of degree 7 in q^2 near the median and in sqrt(-log(tail)) beyond it. The
// coefficients are the published ones. FoundationMath's shader ComputeStandardNormalQuantile (shaders/math.slang)
// evaluates the same polynomials in the same order in float32, which the float32 overload mirrors.
constexpr float64 QUANTILE_CENTRAL_LIMIT = 0.425;
constexpr float64 QUANTILE_CENTRAL_OFFSET = 0.180625; // 0.425^2
constexpr float64 QUANTILE_INTERMEDIATE_LIMIT = 5;
constexpr float64 QUANTILE_INTERMEDIATE_OFFSET = 1.6;
constexpr usize QUANTILE_TERM_COUNT = 8;
// Coefficients of each polynomial from the highest degree to the constant term. FoundationMath depends only on
// FoundationBase, so these are plain constexpr arrays rather than FoundationContainers' StaticArray.
constexpr float64 QUANTILE_CENTRAL_NUMERATOR[QUANTILE_TERM_COUNT] = {
    2.5090809287301226727e+3,
    3.3430575583588128105e+4,
    6.7265770927008700853e+4,
    4.5921953931549871457e+4,
    1.3731693765509461125e+4,
    1.9715909503065514427e+3,
    1.3314166789178437745e+2,
    3.3871328727963666080e+0,
};
constexpr float64 QUANTILE_CENTRAL_DENOMINATOR[QUANTILE_TERM_COUNT] = {
    5.2264952788528545610e+3,
    2.8729085735721942674e+4,
    3.9307895800092710610e+4,
    2.1213794301586595867e+4,
    5.3941960214247511077e+3,
    6.8718700749205790830e+2,
    4.2313330701600911252e+1,
    1.0,
};
constexpr float64 QUANTILE_INTERMEDIATE_NUMERATOR[QUANTILE_TERM_COUNT] = {
    7.74545014278341407640e-4,
    2.27238449892691845833e-2,
    2.41780725177450611770e-1,
    1.27045825245236838258e+0,
    3.64784832476320460504e+0,
    5.76949722146069140550e+0,
    4.63033784615654529590e+0,
    1.42343711074968357734e+0,
};
constexpr float64 QUANTILE_INTERMEDIATE_DENOMINATOR[QUANTILE_TERM_COUNT] = {
    1.05075007164441684324e-9,
    5.47593808499534494600e-4,
    1.51986665636164571966e-2,
    1.48103976427480074590e-1,
    6.89767334985100004550e-1,
    1.67638483018380384940e+0,
    2.05319162663775882187e+0,
    1.0,
};
constexpr float64 QUANTILE_TAIL_NUMERATOR[QUANTILE_TERM_COUNT] = {
    2.01033439929228813265e-7,
    2.71155556874348757815e-5,
    1.24266094738807843860e-3,
    2.65321895265761230930e-2,
    2.96560571828504891230e-1,
    1.78482653991729133580e+0,
    5.46378491116411436990e+0,
    6.65790464350110377720e+0,
};
constexpr float64 QUANTILE_TAIL_DENOMINATOR[QUANTILE_TERM_COUNT] = {
    2.04426310338993978564e-15,
    1.42151175831644588870e-7,
    1.84631831751005468180e-5,
    7.86869131145613259100e-4,
    1.48753612908506148525e-2,
    1.36929880922735805310e-1,
    5.99832206555887937690e-1,
    1.0,
};

/// Horner evaluation of coefficients ordered from the highest degree, in ScalarType arithmetic.
template <typename ScalarType>
[[nodiscard]] ScalarType EvaluateQuantilePolynomial(const float64 (&coefficients)[QUANTILE_TERM_COUNT],
                                                    ScalarType x) noexcept
{
    ScalarType result = static_cast<ScalarType>(coefficients[0]);
    for (usize i = 1; i < QUANTILE_TERM_COUNT; ++i)
    {
        result = result * x + static_cast<ScalarType>(coefficients[i]);
    }
    return result;
}

template <typename ScalarType>
[[nodiscard]] ScalarType ComputeStandardNormalQuantileImpl(ScalarType probability) noexcept
{
    // Written so that NaN fails the comparison and is rejected too.
    if ((probability > ScalarType{0} && probability < ScalarType{1}) == false)
    {
        return std::numeric_limits<ScalarType>::quiet_NaN();
    }
    const ScalarType q = probability - static_cast<ScalarType>(0.5);
    if (std::abs(q) <= static_cast<ScalarType>(QUANTILE_CENTRAL_LIMIT))
    {
        const ScalarType r = static_cast<ScalarType>(QUANTILE_CENTRAL_OFFSET) - q * q;
        return q * EvaluateQuantilePolynomial(QUANTILE_CENTRAL_NUMERATOR, r) /
               EvaluateQuantilePolynomial(QUANTILE_CENTRAL_DENOMINATOR, r);
    }
    // The smaller tail probability, so that 1 - probability does not cancel below the median.
    const ScalarType tail = q < ScalarType{0} ? probability : ScalarType{1} - probability;
    ScalarType r = std::sqrt(-std::log(tail));
    ScalarType magnitude = 0;
    if (r <= static_cast<ScalarType>(QUANTILE_INTERMEDIATE_LIMIT))
    {
        r -= static_cast<ScalarType>(QUANTILE_INTERMEDIATE_OFFSET);
        magnitude = EvaluateQuantilePolynomial(QUANTILE_INTERMEDIATE_NUMERATOR, r) /
                    EvaluateQuantilePolynomial(QUANTILE_INTERMEDIATE_DENOMINATOR, r);
    }
    else
    {
        r -= static_cast<ScalarType>(QUANTILE_INTERMEDIATE_LIMIT);
        magnitude = EvaluateQuantilePolynomial(QUANTILE_TAIL_NUMERATOR, r) /
                    EvaluateQuantilePolynomial(QUANTILE_TAIL_DENOMINATOR, r);
    }
    return q < ScalarType{0} ? -magnitude : magnitude;
}
} // namespace

float64 ComputeStandardNormalQuantile(float64 probability) noexcept
{
    return ComputeStandardNormalQuantileImpl(probability);
}
float32 ComputeStandardNormalQuantile(float32 probability) noexcept
{
    return ComputeStandardNormalQuantileImpl(probability);
}
} // namespace ludus::foundation::math
