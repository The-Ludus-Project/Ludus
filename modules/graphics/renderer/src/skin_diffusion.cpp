#include "internal/skin_diffusion.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
// Radii beyond this many scattering distances carry exp(-60) of the slower exponential's energy.
constexpr float64 OUTER_RADIUS_PER_DISTANCE = 180;
// Simpson intervals of the tail integral; even.
constexpr usize TAIL_INTERVALS = 1024;
// Bisection steps of FindSkinStripHalfWidth over [0, OUTER_RADIUS_PER_DISTANCE]: under 1e-12 of the interval.
constexpr usize BISECTION_STEPS = 64;
// Weight of the slower exponential's energy in the radial CDF.
constexpr float64 SLOW_LOBE_WEIGHT = 0.75;
constexpr float64 SLOW_LOBE_SCALE = 3;
constexpr float64 SIMPSON_ODD_WEIGHT = 4;
constexpr float64 SIMPSON_DIVISOR = 3;
constexpr float64 LOG2_MIN = LUDUS_SKIN_ENERGY_LOG2_MIN;
constexpr float64 LOG2_MAX = LUDUS_SKIN_ENERGY_LOG2_MAX;
constexpr float64 CAPTURED_ENERGY = LUDUS_SKIN_KERNEL_CAPTURED_ENERGY;
constexpr usize COMPONENTS = SKIN_KERNEL_WORD_COMPONENTS;
// Natural logarithm of two, for powers and logarithms of two through Exp and Log.
constexpr float64 LN2 = 0.6931471805599453;
float64 ComputePowerOfTwo(float64 exponent) noexcept
{
    return math::Exp(exponent * LN2);
}

// Radial density 2 pi r R(r) of the profile with l = 1; it integrates to one over [0, infinity).
float64 ComputeRadialDensity(float64 radius) noexcept
{
    return ((1 - SLOW_LOBE_WEIGHT) * math::Exp(-radius) +
            SLOW_LOBE_WEIGHT / SLOW_LOBE_SCALE * math::Exp(-radius / SLOW_LOBE_SCALE));
}
// Radial CDF from section 2 of Christensen's talk.
float64 ComputeRadialEnergy(float64 radius) noexcept
{
    return 1 - (1 - SLOW_LOBE_WEIGHT) * math::Exp(-radius) - SLOW_LOBE_WEIGHT * math::Exp(-radius / SLOW_LOBE_SCALE);
}
float32 GetComponent(const math::Vector4& word, usize component) noexcept
{
    return component == 0 ? word.X : (component == 1 ? word.Y : (component == 2 ? word.Z : word.W));
}
void SetComponent(math::Vector4& inoutWord, usize component, float32 value) noexcept
{
    (component == 0 ? inoutWord.X : (component == 1 ? inoutWord.Y : (component == 2 ? inoutWord.Z : inoutWord.W))) =
        value;
}
} // namespace

// The strip |x| <= u holds every ring of radius r <= u, and of a larger ring the arcs within it: the fraction
// (2 / pi) asin(u / r) of its circumference. So the strip energy is the radial CDF at u plus the tail
// (2 / pi) * integral over r > u of density(r) asin(u / r). The tail is integrated over r = u exp(s^2), whose
// Jacobian 2 s r removes the square-root behaviour of asin(u / r) at r = u.
float64 ComputeSkinStripEnergy(float64 halfWidthPerDistance) noexcept
{
    const float64 u = halfWidthPerDistance;
    if ((u > 0) == false)
    {
        return 0;
    }
    if (u >= OUTER_RADIUS_PER_DISTANCE)
    {
        return 1;
    }
    const float64 end = math::Sqrt(math::Log(OUTER_RADIUS_PER_DISTANCE / u));
    const float64 step = end / static_cast<float64>(TAIL_INTERVALS);
    float64 tail = 0;
    for (usize i = 0; i <= TAIL_INTERVALS; ++i)
    {
        const float64 s = step * static_cast<float64>(i);
        const float64 radius = u * math::Exp(s * s);
        // asin(u / r), the half-angle of the ring's arcs inside the strip.
        const float64 angle = math::Atan2(u, math::Sqrt(math::Max(radius * radius - u * u, 0.0)));
        const float64 value = ComputeRadialDensity(radius) * angle * 2 * s * radius;
        const float64 weight = i == 0 || i == TAIL_INTERVALS ? 1 : (i % 2 == 1 ? SIMPSON_ODD_WEIGHT : 2);
        tail += weight * value;
    }
    tail *= step / SIMPSON_DIVISOR;
    return math::Clamp(ComputeRadialEnergy(u) + 2 / math::kPi * tail, 0.0, 1.0);
}

float64 FindSkinStripHalfWidth(float64 energy) noexcept
{
    float64 low = 0;
    float64 high = OUTER_RADIUS_PER_DISTANCE;
    for (usize step = 0; step < BISECTION_STEPS; ++step)
    {
        const float64 middle = (low + high) / 2;
        (ComputeSkinStripEnergy(middle) < energy ? low : high) = middle;
    }
    return (low + high) / 2;
}

SkinDiffusionKernel MakeSkinDiffusionKernel() noexcept
{
    SkinDiffusionKernel kernel;
    const float64 log2Step = (LOG2_MAX - LOG2_MIN) / static_cast<float64>(SKIN_ENERGY_ENTRIES - 1);
    for (usize entry = 0; entry < SKIN_ENERGY_ENTRIES; ++entry)
    {
        const float64 halfWidth = ComputePowerOfTwo(LOG2_MIN + log2Step * static_cast<float64>(entry));
        SetComponent(kernel.Words[entry / COMPONENTS],
                     entry % COMPONENTS,
                     static_cast<float32>(ComputeSkinStripEnergy(halfWidth)));
    }
    // The centre bin and each pair of side bins hold an equal share of the captured energy; each side tap sits at
    // the energy median of its bin.
    constexpr float64 BIN_COUNT = static_cast<float64>(2 * SKIN_KERNEL_SIDE_TAPS + 1);
    math::Vector4& offsets = kernel.Words[LUDUS_SKIN_KERNEL_OFFSET_WORD];
    math::Vector4& boundaries = kernel.Words[LUDUS_SKIN_KERNEL_BOUNDARY_WORD];
    for (usize bin = 0; bin <= SKIN_KERNEL_SIDE_TAPS; ++bin)
    {
        const float64 boundaryEnergy = CAPTURED_ENERGY * static_cast<float64>(2 * bin + 1) / BIN_COUNT;
        SetComponent(boundaries, bin, static_cast<float32>(FindSkinStripHalfWidth(boundaryEnergy)));
        if (bin > 0)
        {
            const float64 medianEnergy = CAPTURED_ENERGY * static_cast<float64>(2 * bin) / BIN_COUNT;
            SetComponent(offsets, bin - 1, static_cast<float32>(FindSkinStripHalfWidth(medianEnergy)));
        }
    }
    return kernel;
}

float32 LookupSkinStripEnergy(const SkinDiffusionKernel& kernel, float32 halfWidthPerDistance) noexcept
{
    constexpr float64 SPAN = LOG2_MAX - LOG2_MIN;
    constexpr usize LAST = SKIN_ENERGY_ENTRIES - 1;
    const float32 first = GetComponent(kernel.Words[0], 0);
    if ((halfWidthPerDistance > 0) == false)
    {
        return 0;
    }
    const float32 position = static_cast<float32>(
        (math::Log(static_cast<float64>(halfWidthPerDistance)) / LN2 - LOG2_MIN) / SPAN * static_cast<float64>(LAST));
    if (position <= 0)
    {
        // Below the table the strip energy falls to zero about linearly in the half-width.
        return first * halfWidthPerDistance / static_cast<float32>(ComputePowerOfTwo(LOG2_MIN));
    }
    if (position >= static_cast<float32>(LAST))
    {
        return GetComponent(kernel.Words[LAST / COMPONENTS], LAST % COMPONENTS);
    }
    const usize index = static_cast<usize>(position);
    const float32 fraction = position - static_cast<float32>(index);
    const float32 low = GetComponent(kernel.Words[index / COMPONENTS], index % COMPONENTS);
    const float32 high = GetComponent(kernel.Words[(index + 1) / COMPONENTS], (index + 1) % COMPONENTS);
    return low + (high - low) * fraction;
}

StaticArray<float32, SKIN_KERNEL_SIDE_TAPS + 1> ComputeSkinTapWeights(const SkinDiffusionKernel& kernel,
                                                                      float32 distanceRatio) noexcept
{
    StaticArray<float32, SKIN_KERNEL_SIDE_TAPS + 1> weights{};
    weights[0] = 1;
    if ((distanceRatio > 0) == false)
    {
        return weights;
    }
    const math::Vector4& boundaries = kernel.Words[LUDUS_SKIN_KERNEL_BOUNDARY_WORD];
    float32 previous = LookupSkinStripEnergy(kernel, GetComponent(boundaries, 0) / distanceRatio);
    weights[0] = previous;
    float32 total = previous;
    for (usize tap = 1; tap <= SKIN_KERNEL_SIDE_TAPS; ++tap)
    {
        const float32 energy = LookupSkinStripEnergy(kernel, GetComponent(boundaries, tap) / distanceRatio);
        // The bin pair's energy divided between its two sides.
        weights[tap] = (energy - previous) / 2;
        total = energy;
        previous = energy;
    }
    for (float32& weight : weights)
    {
        weight /= total;
    }
    return weights;
}
} // namespace ludus::graphics::renderer::internal
