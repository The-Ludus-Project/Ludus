#pragma once
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/wave_tracing.hpp>

// Fresnel-Kirchhoff references shared by the wave connection tests. Fresnel integrals are integrated by composite
// Simpson quadrature with an asymptotic tail; they share no code or series with the production operators.
namespace wave_test
{
using ludus::foundation::float64;
using ludus::foundation::usize;
using ludus::graphics::renderer::WaveField;
namespace math = ludus::foundation::math;
/// Simpson intervals over the finite part of each reference integral; the tail uses four asymptotic terms.
inline constexpr usize SIMPSON_INTERVALS = 600000;
inline constexpr float64 QUADRATURE_SPAN = 40;
inline constexpr float64 QUADRATURE_MINIMUM_END = 60;
/// Integral of exp(-i t^2) from `begin` to infinity, by Simpson quadrature and four asymptotic tail terms.
inline WaveField IntegrateFresnelTail(float64 begin)
{
    const float64 end = math::Max(begin + QUADRATURE_SPAN, QUADRATURE_MINIMUM_END);
    const float64 step = (end - begin) / static_cast<float64>(SIMPSON_INTERVALS);
    WaveField sum{math::Cos(begin * begin) + math::Cos(end * end), -math::Sin(begin * begin) - math::Sin(end * end)};
    for (usize index = 1; index < SIMPSON_INTERVALS; ++index)
    {
        const float64 t = begin + static_cast<float64>(index) * step;
        const float64 weight = index % 2 == 1 ? 4 : 2;
        sum.Real += weight * math::Cos(t * t);
        sum.Imaginary -= weight * math::Sin(t * t);
    }
    WaveField integral{sum.Real * step / 3, sum.Imaginary * step / 3};
    // exp(-i W^2) / (2iW) (1 - 1/(2iW^2) + 3/(2iW^2)^2 - 15/(2iW^2)^3); 1/(2iW^2) = -i/(2W^2).
    const float64 inverse = 1 / (2 * end * end);
    const WaveField series{1 - 3 * inverse * inverse, inverse - 15 * inverse * inverse * inverse};
    const WaveField phase{math::Cos(end * end), -math::Sin(end * end)};
    const WaveField leading{phase.Imaginary / (2 * end), -phase.Real / (2 * end)};
    integral.Real += leading.Real * series.Real - leading.Imaginary * series.Imaginary;
    integral.Imaginary += leading.Real * series.Imaginary + leading.Imaginary * series.Real;
    return integral;
}
/// Fresnel-Kirchhoff field ratio behind an absorbing screen that covers Fresnel numbers below v (ITU-R P.526
/// convention): (1 + i)/2 times the integral of exp(-i pi t^2 / 2) from v to infinity.
inline WaveField EvaluateFresnelEdge(float64 v)
{
    // The integral of exp(-i pi t^2 / 2) from v is sqrt(2/pi) times that of exp(-i tau^2) from v sqrt(pi/2).
    const float64 scale = math::Sqrt(math::kPi * 0.5);
    const float64 begin = v * scale;
    WaveField value = IntegrateFresnelTail(math::Abs(begin));
    if (begin < 0)
    {
        // The whole line integrates to sqrt(pi) exp(-i pi/4).
        const float64 whole = math::Sqrt(math::kPi);
        value = {whole * math::Cos(math::kPi / 4) - value.Real, -whole * math::Sin(math::kPi / 4) - value.Imaginary};
    }
    const WaveField integral{value.Real / scale, value.Imaginary / scale};
    return {0.5 * (integral.Real - integral.Imaginary), 0.5 * (integral.Real + integral.Imaginary)};
}
} // namespace wave_test
