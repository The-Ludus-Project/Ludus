#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/scalar.hpp>

#include <cmath>

// Independent higher-order Smith reference for RT1's multiple-scattering GGX profile: a float64 random walk on the
// isotropic GGX microsurface with the uniform height distribution, which scatters every microfacet event with the
// unpolarized complex-IOR Fresnel reflectance of an RGB conductor. Its single-scattering term is GGX with the
// height-correlated Smith masking-shadowing function that RT1's single-scattering lobe uses.
//
// Thanks to Eric Heitz, Johannes Hanika, Eugene d'Eon and Carsten Dachsbacher, "Multiple-Scattering Microfacet BSDFs
// with the Smith Model", ACM Transactions on Graphics 35(4), Article 58, 2016 (SIGGRAPH 2016), sections 4-6,
// https://doi.org/10.1145/2897824.2925943: the microsurface random walk, its height sampling (section 5.2) and the
// stochastic evaluation that sums next-event estimates toward the outgoing direction (section 6). Visible normals are
// sampled with Jonathan Dupuy and Anis Benyoub, "Sampling Visible GGX Normals with Spherical Caps", High-Performance
// Graphics 2023 (Computer Graphics Forum 42(8)), which also admits directions below the horizon and is independent of
// the renderer's Heitz 2018 sampler. The Fresnel equations are evaluated with complex amplitudes,
// rather than the renderer's real-valued algebra. This is a test oracle reimplemented from the papers' text.
namespace ludus::renderer_tests
{
/// Unit direction or RGB triple in float64.
struct Double3 final
{
    foundation::float64 X = 0;
    foundation::float64 Y = 0;
    foundation::float64 Z = 0;
};
[[nodiscard]] inline Double3 operator+(Double3 a, Double3 b) noexcept
{
    return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
}
[[nodiscard]] inline Double3 operator*(Double3 a, foundation::float64 s) noexcept
{
    return {a.X * s, a.Y * s, a.Z * s};
}
[[nodiscard]] inline Double3 Multiply(Double3 a, Double3 b) noexcept
{
    return {a.X * b.X, a.Y * b.Y, a.Z * b.Z};
}
[[nodiscard]] inline foundation::float64 Dot(Double3 a, Double3 b) noexcept
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}
[[nodiscard]] inline Double3 Normalize(Double3 a) noexcept
{
    return a * (1 / std::sqrt(Dot(a, a)));
}

/// Unpolarized Fresnel reflectance of a conductor with complex index eta + i k, by complex amplitudes.
[[nodiscard]] inline foundation::float64
ComputeComplexFresnel(foundation::float64 cosine, foundation::float64 eta, foundation::float64 extinction) noexcept
{
    // n^2 - sin^2 with n = eta + i k, then the s and p amplitude reflection coefficients.
    const foundation::float64 realSquare = eta * eta - extinction * extinction - (1 - cosine * cosine);
    const foundation::float64 imaginarySquare = 2 * eta * extinction;
    const foundation::float64 modulus = std::sqrt(realSquare * realSquare + imaginarySquare * imaginarySquare);
    const foundation::float64 rootReal = std::sqrt(0.5 * (modulus + realSquare));
    const foundation::float64 rootImaginary = imaginarySquare == 0
                                                  ? std::sqrt(std::fmax(0.0, 0.5 * (modulus - realSquare)))
                                                  : imaginarySquare / (2 * rootReal);
    const auto ratio =
        [](foundation::float64 ar, foundation::float64 ai, foundation::float64 br, foundation::float64 bi) noexcept {
            return (ar * ar + ai * ai) / (br * br + bi * bi);
        };
    const foundation::float64 perpendicular =
        ratio(cosine - rootReal, -rootImaginary, cosine + rootReal, rootImaginary);
    // n^2 cos - root, with n^2 = realSquare + sin^2 + i imaginarySquare.
    const foundation::float64 squareReal = realSquare + (1 - cosine * cosine);
    const foundation::float64 parallel = ratio(squareReal * cosine - rootReal,
                                               imaginarySquare * cosine - rootImaginary,
                                               squareReal * cosine + rootReal,
                                               imaginarySquare * cosine + rootImaginary);
    return 0.5 * (perpendicular + parallel);
}

/// Small deterministic generator for the oracle's walks (PCG-XSH-RR, M. E. O'Neill, 2014).
class OracleRandom final
{
public:
    explicit OracleRandom(foundation::uint64 seed) noexcept : mState(seed * 2 + 1)
    {
        static_cast<void>(NextUnit());
    }
    /// Uniform value in [0, 1).
    foundation::float64 NextUnit() noexcept
    {
        const foundation::uint64 previous = mState;
        mState = previous * 6364136223846793005ULL + 1442695040888963407ULL;
        const foundation::uint32 shifted = static_cast<foundation::uint32>(((previous >> 18U) ^ previous) >> 27U);
        const foundation::uint32 rotation = static_cast<foundation::uint32>(previous >> 59U);
        const foundation::uint32 bits = (shifted >> rotation) | (shifted << ((32U - rotation) & 31U));
        return static_cast<foundation::float64>(bits) * (1.0 / 4294967296.0);
    }

private:
    foundation::uint64 mState;
};

/// The isotropic GGX microsurface with uniform heights in [-1, 1] and an RGB conductor at every facet.
class MicrofacetRandomWalk final
{
public:
    /// Scattering orders beyond this are truncated; their energy is negligible for the tested alphas.
    static constexpr foundation::uint32 MAX_ORDER = 64;
    /// @param alpha GGX alpha in (0, 1].
    /// @param eta Real index per channel.
    /// @param extinction Extinction per channel; a large value with eta = 0 gives unit Fresnel.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the real and imaginary parts of one complex index.
    MicrofacetRandomWalk(foundation::float64 alpha, Double3 eta, Double3 extinction, bool isLossless) noexcept
        : mAlpha(alpha), mEta(eta), mExtinction(extinction), mIsLossless(isLossless)
    {
    }
    /// Stochastic estimate of f(in, out) * out.z, the BSDF times the outgoing cosine, from `walks` random walks
    /// entering from `in` (pointing away from the surface), including scattering orders 1..maximumOrder.
    [[nodiscard]] Double3 Evaluate(Double3 in,
                                   Double3 out,
                                   foundation::uint32 walks,
                                   OracleRandom& inoutRandom,
                                   foundation::uint32 maximumOrder = MAX_ORDER) const
    {
        Double3 sum{};
        for (foundation::uint32 walk = 0; walk < walks; ++walk)
        {
            sum = sum + evaluateWalk(in, out, maximumOrder, inoutRandom);
        }
        return sum * (1.0 / walks);
    }
    /// Mean RGB energy that leaves the microsurface after entering from `in`.
    [[nodiscard]] Double3 MeasureAlbedo(Double3 in, foundation::uint32 walks, OracleRandom& inoutRandom) const
    {
        Double3 sum{};
        for (foundation::uint32 walk = 0; walk < walks; ++walk)
        {
            Double3 energy{1, 1, 1};
            Double3 ray = in * -1;
            foundation::float64 height = 1 + INITIAL_HEIGHT_MARGIN;
            bool hasEscaped = false;
            for (foundation::uint32 order = 0; order <= MAX_ORDER; ++order)
            {
                height = sampleHeight(ray, height, inoutRandom.NextUnit());
                if (height == ESCAPED)
                {
                    hasEscaped = order > 0;
                    break;
                }
                if (order == MAX_ORDER)
                {
                    break;
                }
                ray = scatter(ray, energy, inoutRandom);
            }
            if (hasEscaped == true)
            {
                sum = sum + energy;
            }
        }
        return sum * (1.0 / walks);
    }

private:
    static constexpr foundation::float64 ESCAPED = 1e300;
    static constexpr foundation::float64 INITIAL_HEIGHT_MARGIN = 1;
    static constexpr foundation::float64 POLE_COSINE = 0.999999;
    static constexpr foundation::float64 HORIZON_COSINE = 1e-6;

    [[nodiscard]] static foundation::float64 getHeightCdf(foundation::float64 height) noexcept
    {
        return std::fmin(1.0, std::fmax(0.0, 0.5 * (height + 1)));
    }
    [[nodiscard]] static foundation::float64 invertHeightCdf(foundation::float64 value) noexcept
    {
        return std::fmax(-1.0, std::fmin(1.0, 2 * value - 1));
    }
    /// Smith Lambda of GGX, signed for downward directions: Lambda(-w) = -1 - Lambda(w).
    [[nodiscard]] foundation::float64 computeLambda(Double3 w) const noexcept
    {
        if (w.Z >= POLE_COSINE)
        {
            return 0;
        }
        if (w.Z <= -POLE_COSINE)
        {
            return -1;
        }
        const foundation::float64 tangentSquared = (1 - w.Z * w.Z) / (w.Z * w.Z);
        const foundation::float64 positive = 0.5 * (-1 + std::sqrt(1 + mAlpha * mAlpha * tangentSquared));
        return w.Z > 0 ? positive : -1 - positive;
    }
    [[nodiscard]] foundation::float64 computeShadowing(Double3 w, foundation::float64 height) const noexcept
    {
        if (w.Z <= 0)
        {
            return 0;
        }
        return std::pow(getHeightCdf(height), computeLambda(w));
    }
    /// Next intersection height along ray from height, or ESCAPED (Heitz et al., section 5.2).
    [[nodiscard]] foundation::float64
    sampleHeight(Double3 ray, foundation::float64 height, foundation::float64 uniform) const noexcept
    {
        if (ray.Z >= POLE_COSINE)
        {
            return ESCAPED;
        }
        if (ray.Z <= -POLE_COSINE)
        {
            return invertHeightCdf(uniform * getHeightCdf(height));
        }
        if (std::fabs(ray.Z) < HORIZON_COSINE)
        {
            return height;
        }
        if (uniform > 1 - computeShadowing(ray, height))
        {
            return ESCAPED;
        }
        return invertHeightCdf(getHeightCdf(height) / std::pow(1 - uniform, 1 / computeLambda(ray)));
    }
    [[nodiscard]] foundation::float64 computeDistribution(Double3 normal) const noexcept
    {
        if (normal.Z <= 0)
        {
            return 0;
        }
        const foundation::float64 alphaSquared = mAlpha * mAlpha;
        const foundation::float64 denominator =
            normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z * alphaSquared;
        return alphaSquared / (foundation::math::kPi * denominator * denominator);
    }
    /// Projected area of the microfacets visible from w, (1 + Lambda(w)) w.z, for either hemisphere.
    [[nodiscard]] foundation::float64 computeProjectedArea(Double3 w) const noexcept
    {
        return 0.5 * (w.Z + std::sqrt(w.Z * w.Z + (1 - w.Z * w.Z) * mAlpha * mAlpha));
    }
    [[nodiscard]] Double3 computeFresnel(foundation::float64 cosine) const noexcept
    {
        if (mIsLossless == true)
        {
            return {1, 1, 1};
        }
        return {ComputeComplexFresnel(cosine, mEta.X, mExtinction.X),
                ComputeComplexFresnel(cosine, mEta.Y, mExtinction.Y),
                ComputeComplexFresnel(cosine, mEta.Z, mExtinction.Z)};
    }
    /// Reflection phase function density from `in` (pointing away from the facet) to `out`, with its Fresnel.
    [[nodiscard]] Double3 evaluatePhase(Double3 in, Double3 out) const noexcept
    {
        const Double3 half = Normalize(in + out);
        const foundation::float64 inHalf = Dot(in, half);
        if (half.Z <= 0 || inHalf <= 0)
        {
            return {};
        }
        const foundation::float64 visible = inHalf * computeDistribution(half) / computeProjectedArea(in);
        return computeFresnel(inHalf) * (visible / (4 * inHalf));
    }
    /// Visible normal for `in` by Dupuy and Benyoub's spherical caps in the stretched configuration.
    [[nodiscard]] Double3
    sampleVisibleNormal(Double3 in, foundation::float64 first, foundation::float64 second) const noexcept
    {
        const Double3 stretched = Normalize({mAlpha * in.X, mAlpha * in.Y, in.Z});
        const foundation::float64 angle = 2 * foundation::math::kPi * first;
        const foundation::float64 z = (1 - second) * (1 + stretched.Z) - stretched.Z;
        const foundation::float64 sine = std::sqrt(std::fmax(0.0, 1 - z * z));
        const Double3 half = Double3{sine * std::cos(angle), sine * std::sin(angle), z} + stretched;
        return Normalize({mAlpha * half.X, mAlpha * half.Y, std::fmax(half.Z, 0.0)});
    }
    /// Reflect the ray about a sampled visible normal and attenuate the energy by its Fresnel reflectance.
    [[nodiscard]] Double3 scatter(Double3 ray, Double3& inoutEnergy, OracleRandom& inoutRandom) const noexcept
    {
        const Double3 in = ray * -1;
        const foundation::float64 first = inoutRandom.NextUnit();
        const foundation::float64 second = inoutRandom.NextUnit();
        const Double3 normal = sampleVisibleNormal(in, first, second);
        const foundation::float64 inNormal = Dot(in, normal);
        inoutEnergy = Multiply(inoutEnergy, computeFresnel(inNormal));
        return normal * (2 * inNormal) + ray;
    }
    // NOLINTBEGIN(bugprone-easily-swappable-parameters): incoming then outgoing direction, as in Evaluate.
    [[nodiscard]] Double3
    evaluateWalk(Double3 in, Double3 out, foundation::uint32 maximumOrder, OracleRandom& inoutRandom) const noexcept
    // NOLINTEND(bugprone-easily-swappable-parameters)
    {
        if (out.Z <= 0)
        {
            return {};
        }
        Double3 sum{};
        Double3 energy{1, 1, 1};
        Double3 ray = in * -1;
        foundation::float64 height = 1 + INITIAL_HEIGHT_MARGIN;
        for (foundation::uint32 order = 0; order < maximumOrder; ++order)
        {
            height = sampleHeight(ray, height, inoutRandom.NextUnit());
            if (height == ESCAPED)
            {
                break;
            }
            // Next-event estimate: scatter toward out at this vertex, then leave unshadowed from its height.
            const Double3 phase = evaluatePhase(ray * -1, out);
            sum = sum + Multiply(energy, phase) * computeShadowing(out, height);
            ray = scatter(ray, energy, inoutRandom);
        }
        return sum;
    }

    foundation::float64 mAlpha;
    Double3 mEta;
    Double3 mExtinction;
    bool mIsLossless;
};
} // namespace ludus::renderer_tests
