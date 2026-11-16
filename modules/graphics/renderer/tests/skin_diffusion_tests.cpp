// CPU checks of the raster skin diffusion kernel (src/internal/skin_diffusion.h): the strip energy of the normalized
// diffusion profile, its inverse, the equal-energy bins and the shader's tap weights.

#include "../src/internal/skin_diffusion.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;

// K0(x) = integral over t in [0, infinity) of exp(-x cosh t), by Simpson's rule up to where x cosh t reaches 60.
float64 ComputeBesselK0(float64 x)
{
    constexpr usize INTERVALS = 8192;
    constexpr float64 EXPONENT_CUTOFF = 60;
    // cosh t >= exp(t) / 2, so exp(t) = 2 * cutoff / x bounds the range.
    const float64 end = math::Log(2 * EXPONENT_CUTOFF / x);
    const float64 step = end / static_cast<float64>(INTERVALS);
    float64 sum = 0;
    for (usize i = 0; i <= INTERVALS; ++i)
    {
        const float64 t = step * static_cast<float64>(i);
        const float64 coshT = (math::Exp(t) + math::Exp(-t)) / 2;
        const float64 weight = i == 0 || i == INTERVALS ? 1 : (i % 2 == 1 ? 4 : 2);
        sum += weight * math::Exp(-x * coshT);
    }
    return sum * step / 3;
}
} // namespace

TEST_CASE("Skin strip energy is the integral of the pre-integrated normalized-diffusion kernel", "[skin][renderer]")
{
    // The quadrature of K0 itself: K0(1) from Abramowitz and Stegun, table 9.8.
    CHECK(math::Abs(ComputeBesselK0(1) - 0.42102443824070834) < 1e-9);
    CHECK(internal::ComputeSkinStripEnergy(0) == 0);
    CHECK(internal::ComputeSkinStripEnergy(-1) == 0);
    CHECK(internal::ComputeSkinStripEnergy(150) > 1 - 1e-12);
    // The derivative of the two-sided strip energy is twice the 1D kernel a(u) = (K0(u) + K0(u / 3)) / (4 pi), the
    // profile's marginal along one axis (Jimenez et al., equation 4), here reached through a different integral.
    constexpr float64 STEP = 1e-4;
    for (const float64 u : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0})
    {
        const float64 derivative =
            (internal::ComputeSkinStripEnergy(u + STEP) - internal::ComputeSkinStripEnergy(u - STEP)) / (2 * STEP);
        const float64 kernel = (ComputeBesselK0(u) + ComputeBesselK0(u / 3)) / (4 * math::kPi);
        CHECK(math::Abs(derivative - 2 * kernel) < 1e-6 * kernel + 1e-9);
    }
    float64 previous = 0;
    // Half-widths from 1/256 to 64 scattering distances, doubling.
    constexpr usize DOUBLINGS = 15;
    float64 u = 1.0 / 256;
    for (usize doubling = 0; doubling < DOUBLINGS; ++doubling, u *= 2)
    {
        const float64 energy = internal::ComputeSkinStripEnergy(u);
        CHECK(energy > previous);
        previous = energy;
        CHECK(math::Abs(internal::ComputeSkinStripEnergy(internal::FindSkinStripHalfWidth(energy)) - energy) < 1e-9);
    }
}

TEST_CASE("Skin diffusion kernel places equal-energy bins and normalized per-channel weights", "[skin][renderer]")
{
    const internal::SkinDiffusionKernel kernel = internal::MakeSkinDiffusionKernel();
    const math::Vector4& offsets = kernel.Words[LUDUS_SKIN_KERNEL_OFFSET_WORD];
    const math::Vector4& boundaries = kernel.Words[LUDUS_SKIN_KERNEL_BOUNDARY_WORD];
    const StaticArray<float32, internal::SKIN_KERNEL_SIDE_TAPS + 1> bins{
        {boundaries.X, boundaries.Y, boundaries.Z, boundaries.W}};
    const StaticArray<float32, internal::SKIN_KERNEL_SIDE_TAPS + 1> taps{{0, offsets.X, offsets.Y, offsets.Z}};
    static_assert(internal::SKIN_KERNEL_SIDE_TAPS == 3);
    for (usize tap = 1; tap <= internal::SKIN_KERNEL_SIDE_TAPS; ++tap)
    {
        CHECK(bins[tap - 1] < taps[tap]);
        CHECK(taps[tap] < bins[tap]);
    }
    // The outermost bin captures the declared share of the dominant channel's energy.
    CHECK(math::Abs(internal::ComputeSkinStripEnergy(static_cast<float64>(bins[internal::SKIN_KERNEL_SIDE_TAPS])) -
                    LUDUS_SKIN_KERNEL_CAPTURED_ENERGY) < 1e-6);
    // The shader's table lookup follows the quadrature between its entries.
    for (const float32 u : {0.003F, 0.04F, 0.3F, 1.7F, 9.0F, 60.0F})
    {
        CHECK(math::Abs(internal::LookupSkinStripEnergy(kernel, u) -
                        static_cast<float32>(internal::ComputeSkinStripEnergy(static_cast<float64>(u)))) < 5e-3F);
    }
    constexpr float32 WEIGHT_TOLERANCE = 5e-3F;
    // The dominant channel gives every bin the same weight, one seventh after renormalization.
    using Weights = StaticArray<float32, internal::SKIN_KERNEL_SIDE_TAPS + 1>;
    const Weights dominant = internal::ComputeSkinTapWeights(kernel, 1);
    const float32 share = 1.0F / static_cast<float32>(2 * internal::SKIN_KERNEL_SIDE_TAPS + 1);
    CHECK(math::Abs(dominant[0] - share) < WEIGHT_TOLERANCE);
    for (usize tap = 1; tap <= internal::SKIN_KERNEL_SIDE_TAPS; ++tap)
    {
        CHECK(math::Abs(dominant[tap] - share) < WEIGHT_TOLERANCE);
    }
    // Every channel's weights are nonnegative, fall off from the centre and sum to one over both sides.
    for (const float32 ratio : {0.0F, 0.25F, 0.5F, 1.0F})
    {
        const Weights weights = internal::ComputeSkinTapWeights(kernel, ratio);
        float32 total = weights[0];
        for (usize tap = 1; tap <= internal::SKIN_KERNEL_SIDE_TAPS; ++tap)
        {
            CHECK(weights[tap] >= 0);
            CHECK(weights[tap] <= weights[tap - 1] + WEIGHT_TOLERANCE);
            total += 2 * weights[tap];
        }
        CHECK(math::Abs(total - 1) < 1e-5F);
    }
    CHECK(internal::ComputeSkinTapWeights(kernel, 0)[0] == 1);
    // Shorter distances concentrate the weight on the centre tap.
    CHECK(internal::ComputeSkinTapWeights(kernel, 0.25F)[0] > internal::ComputeSkinTapWeights(kernel, 0.5F)[0]);
    CHECK(internal::ComputeSkinTapWeights(kernel, 0.5F)[0] > dominant[0]);
}
