#include "../src/internal/ggx_albedo_table.h"
#include "microfacet_random_walk.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// CPU-oracle evidence for ReferenceMaterialProfile::MultipleScatteringGgx: the tabulated albedo against an
// independent quadrature, the lossless furnace over a roughness and view sweep, matched sampling densities, and
// absorbing-metal energy against the independent higher-order Smith random walk (microfacet_random_walk.h). The
// paired CPU/GPU capture comparisons live in reference_compute_tests.cpp. Measured errors are recorded in
// docs/architecture/renderer-systems.md#multiple-scattering-ggx-profile.
namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::scene_format;
namespace internal = ludus::graphics::renderer::internal;
using ludus::renderer_tests::Double3;
using ludus::renderer_tests::MicrofacetRandomWalk;
using ludus::renderer_tests::OracleRandom;

constexpr rr::ReferenceMaterialProfile SINGLE = rr::ReferenceMaterialProfile::SingleScattering;
constexpr rr::ReferenceMaterialProfile MULTIPLE = rr::ReferenceMaterialProfile::MultipleScatteringGgx;
const math::Vector3 NORMAL_WORLD{0, 0, 1};
/// Gold-like RGB optical constants whose Fresnel reflectance differs strongly by channel.
constexpr StaticArray<float32, 3> GOLD_ETA{{0.143F, 0.374F, 1.442F}};
constexpr StaticArray<float32, 3> GOLD_K{{3.983F, 2.385F, 1.603F}};

float64 Widen(float32 value)
{
    return static_cast<float64>(value);
}
Double3 Widen(const StaticArray<float32, 3>& value)
{
    return {Widen(value[0]), Widen(value[1]), Widen(value[2])};
}
Double3 Widen(math::Vector3 value)
{
    return {Widen(value.X), Widen(value.Y), Widen(value.Z)};
}
math::Vector3 MakeView(float64 degrees)
{
    const float64 radians = degrees * math::kPi / 180;
    return {static_cast<float32>(std::sin(radians)), 0, static_cast<float32>(std::cos(radians))};
}
rr::ReferenceBsdf MakeWhiteMetal(float32 roughness, rr::ReferenceMaterialProfile profile)
{
    rr::ReferenceBsdf bsdf;
    bsdf.BaseColor = {1, 1, 1};
    bsdf.Metallic = 1;
    bsdf.Roughness = roughness;
    bsdf.Profile = profile;
    return bsdf;
}
rr::ReferenceBsdf MakeConductor(float32 alpha,
                                const StaticArray<float32, 3>& eta,
                                const StaticArray<float32, 3>& extinction,
                                rr::ReferenceMaterialProfile profile)
{
    rr::ReferenceBsdf bsdf;
    bsdf.Scattering.Model = format::MaterialModel::Conductor;
    bsdf.Scattering.AlphaU = alpha;
    bsdf.Scattering.AlphaV = alpha;
    for (usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
    {
        bsdf.Scattering.Eta[channel] = eta[channel];
        bsdf.Scattering.K[channel] = extinction[channel];
    }
    bsdf.Profile = profile;
    return bsdf;
}
/// Unit-Fresnel directional albedo of RT1's single-scattering lobe by classical GGX normal sampling (Bruce Walter et
/// al., "Microfacet Models for Refraction through Rough Surfaces", EGSR 2007), on a float64 midpoint grid.
/// Independent of the table generator's visible-normal integration.
float64 IntegrateSingleScatteringAlbedo(float64 alpha, float64 cosine, uint32 strata)
{
    const float64 sine = std::sqrt(std::fmax(0.0, 1 - cosine * cosine));
    const float64 alphaSquared = alpha * alpha;
    const float64 lambdaView = std::sqrt(cosine * cosine * (1 - alphaSquared) + alphaSquared);
    float64 sum = 0;
    for (uint32 i = 0; i < strata; ++i)
    {
        const float64 u = (i + 0.5) / strata;
        const float64 halfCosine = 1 / std::sqrt(1 + alphaSquared * u / (1 - u));
        const float64 halfSine = std::sqrt(std::fmax(0.0, 1 - halfCosine * halfCosine));
        for (uint32 j = 0; j < strata; ++j)
        {
            const float64 angle = math::kTau * (j + 0.5) / strata;
            const float64 halfX = halfSine * std::cos(angle);
            const float64 viewHalf = sine * halfX + cosine * halfCosine;
            const float64 lightCosine = 2 * viewHalf * halfCosine - cosine;
            if (viewHalf <= 0 || lightCosine <= 0)
            {
                continue;
            }
            const float64 lambdaLight = std::sqrt(lightCosine * lightCosine * (1 - alphaSquared) + alphaSquared);
            const float64 visibility = 0.5 / std::fmax(lightCosine * lambdaView + cosine * lambdaLight, 1e-6);
            // f cos / pdf with pdf = D cos(theta_h) / (4 v.h).
            sum += 4 * visibility * lightCosine * viewHalf / halfCosine;
        }
    }
    return sum / (float64(strata) * strata);
}
/// Radical inverse of index in base: the Halton sequence's coordinate for that base.
float32 ComputeRadicalInverse(uint32 index, uint32 base)
{
    float64 result = 0;
    float64 scale = 1.0 / base;
    for (uint32 value = index; value > 0; value /= base)
    {
        result += scale * (value % base);
        scale /= base;
    }
    return static_cast<float32>(std::fmin(result, 0.99999994));
}
/// The index-th point of the three-dimensional Halton sequence in bases 2, 3 and 5: lobe selection and disk samples
/// stay jointly well distributed, which a product grid of lobe strata does not provide for mixture proposals.
math::Vector3 MakeHaltonSample(uint32 index)
{
    return {ComputeRadicalInverse(index + 1, 2),
            ComputeRadicalInverse(index + 1, 3),
            ComputeRadicalInverse(index + 1, 5)};
}
/// Mean RGB albedo of a BSDF from its own sampler over Halton points: an estimate of the integral of f cos over the
/// hemisphere that also exercises the sampler, its density and its weights.
math::Vector3 MeasureSampledAlbedo(const rr::ReferenceBsdf& bsdf, math::Vector3 viewWorld, uint32 count)
{
    Double3 sum{};
    for (uint32 index = 0; index < count; ++index)
    {
        rr::ReferenceBsdfSample sampled;
        REQUIRE(rr::SampleReferenceBsdf(bsdf, NORMAL_WORLD, viewWorld, MakeHaltonSample(index), sampled) ==
                rhi::RasterStatus::Ready);
        if (sampled.Accepted == true)
        {
            sum = sum + Widen(sampled.Weight);
        }
    }
    sum = sum * (1.0 / count);
    return {static_cast<float32>(sum.X), static_cast<float32>(sum.Y), static_cast<float32>(sum.Z)};
}
} // namespace

TEST_CASE("The GGX albedo table matches independent integration of the single-scattering lobe",
          "[renderer][rt1][materials][multiple-scattering]")
{
    // The fully rough normal-incidence baseline of the rough-metal furnace: alpha = F = 1 integrates to 1 - ln 2.
    CHECK(std::fabs(Widen(internal::LookupGgxDirectionalAlbedo(1, 1)) - (1 - std::log(2.0))) < 1e-5);
    // The unit-albedo mirror row has no deficit.
    CHECK(internal::LookupGgxDirectionalAlbedo(0, 0.3F) == 1);
    CHECK(internal::LookupGgxAverageDeficit(0) == 0);
    struct Point final
    {
        float64 Roughness = 0;
        float64 Degrees = 0;
        float64 Tolerance = 0;
        uint32 Strata = 2048;
    };
    // The Imageworks review's grazing case (roughness 0.06, 89.75 degrees, single-scatter albedo 0.8920) and points
    // between table rows and columns. Tolerances bound the bilinear interpolation error at each point.
    const Point points[]{
        {0.06, 89.75, 0.003, 8192},
        {0.045, 0.0, 0.0005},
        {0.045, 89.9, 0.004, 8192},
        {0.1, 80.0, 0.002},
        {0.15, 88.0, 0.003},
        {0.33, 60.0, 0.002},
        {0.5, 0.0, 0.002},
        {0.5, 85.0, 0.002},
        {0.71, 45.0, 0.002},
        {0.9, 89.0, 0.002},
        {1.0, 70.0, 0.002},
    };
    for (const Point& point : points)
    {
        const float64 alpha = point.Roughness * point.Roughness;
        const float64 cosine = std::cos(point.Degrees * math::kPi / 180);
        const float64 expected = IntegrateSingleScatteringAlbedo(alpha, cosine, point.Strata);
        const float64 tabulated = Widen(internal::LookupGgxDirectionalAlbedo(float32(alpha), float32(cosine)));
        CAPTURE(point.Roughness, point.Degrees, expected, tabulated);
        CHECK(std::fabs(tabulated - expected) <= point.Tolerance);
    }
}

TEST_CASE("Compensated lossless GGX reflects the white furnace over a roughness and view sweep",
          "[renderer][rt1][materials][multiple-scattering]")
{
    // Kulla and Conty's furnace criterion: with unit Fresnel the compensated lobe's albedo is one. The legacy
    // metallic/roughness model with white base colour and metallic one has unit Schlick Fresnel. The albedo splits into
    // the single-scattering lobe, integrated by its visible-normal sampler, and the added lobe, the difference of the
    // two profiles' values. The added lobe is isotropic, so a light azimuth perpendicular to the view, away from the
    // specular peak, gives it by a one-dimensional quadrature that resolves grazing light.
    constexpr float64 FURNACE_TOLERANCE = 0.01;
    constexpr uint32 QUADRATURE_STEPS = 8192;
    constexpr float64 SAMPLED_TOLERANCE = 0.01;
    float64 largestError = 0;
    float64 largestSampledError = 0;
    for (const float32 roughness : {0.045F, 0.06F, 0.1F, 0.25F, 0.5F, 0.75F, 1.0F})
    {
        const rr::ReferenceBsdf single = MakeWhiteMetal(roughness, SINGLE);
        const rr::ReferenceBsdf compensated = MakeWhiteMetal(roughness, MULTIPLE);
        for (const float64 degrees : {0.0, 20.0, 45.0, 60.0, 75.0, 85.0, 88.0, 89.0, 89.5, 89.75, 89.9, 89.99})
        {
            const math::Vector3 viewWorld = MakeView(degrees);
            const float64 singleAlbedo = Widen(MeasureSampledAlbedo(single, viewWorld, 1U << 14).X);
            float64 addedAlbedo = 0;
            for (uint32 step = 0; step < QUADRATURE_STEPS; ++step)
            {
                // Light cosine mu = s^3 concentrates steps at grazing light, where the added lobe peaks.
                const float64 s = (step + 0.5) / QUADRATURE_STEPS;
                const float64 cosine = s * s * s;
                const math::Vector3 lightWorld{0,
                                               static_cast<float32>(std::sqrt(1 - cosine * cosine)),
                                               static_cast<float32>(cosine)};
                rr::ReferenceBsdfEvaluation withLobe;
                rr::ReferenceBsdfEvaluation withoutLobe;
                REQUIRE(rr::EvaluateReferenceBsdf(compensated, NORMAL_WORLD, viewWorld, lightWorld, withLobe) ==
                        rhi::RasterStatus::Ready);
                REQUIRE(rr::EvaluateReferenceBsdf(single, NORMAL_WORLD, viewWorld, lightWorld, withoutLobe) ==
                        rhi::RasterStatus::Ready);
                CHECK(withLobe.Value.X == withLobe.Value.Y);
                CHECK(withLobe.Value.X == withLobe.Value.Z);
                addedAlbedo +=
                    math::kTau * Widen(withLobe.Value.X - withoutLobe.Value.X) * cosine * 3 * s * s / QUADRATURE_STEPS;
            }
            const float64 albedo = singleAlbedo + addedAlbedo;
            CAPTURE(roughness, degrees, singleAlbedo, addedAlbedo, albedo);
            const float64 error = std::fabs(albedo - 1);
            largestError = std::fmax(largestError, error);
            CHECK(error <= FURNACE_TOLERANCE);
            CHECK(addedAlbedo >= 0);
            // Where cosine sampling reaches the added lobe's grazing mass, the compensated sampler's own estimate
            // agrees. Smooth metals viewed near grazing put that mass at grazing light, which it rarely samples.
            if (roughness >= 0.25F && degrees <= 85)
            {
                const float64 sampled = Widen(MeasureSampledAlbedo(compensated, viewWorld, 1U << 14).X);
                CAPTURE(sampled);
                largestSampledError = std::fmax(largestSampledError, std::fabs(sampled - albedo));
                CHECK(std::fabs(sampled - albedo) <= SAMPLED_TOLERANCE);
            }
        }
    }
    WARN("Largest lossless furnace error: " << largestError << "; sampled estimate differs by up to "
                                            << largestSampledError);
}

TEST_CASE("Compensated GGX samples report their evaluated density, MIS density and reciprocal value",
          "[renderer][rt1][materials][multiple-scattering]")
{
    StaticArray<rr::ReferenceBsdf, 4> materials{};
    materials[0] = MakeWhiteMetal(0.5F, MULTIPLE);
    materials[1] = MakeWhiteMetal(0.6F, MULTIPLE);
    materials[1].BaseColor = {0.8F, 0.5F, 0.2F};
    materials[1].Metallic = 0.5F;
    materials[2] = MakeConductor(0.3F, GOLD_ETA, GOLD_K, MULTIPLE);
    materials[3] = MakeConductor(0.5F, GOLD_ETA, GOLD_K, MULTIPLE);
    materials[3].Scattering.Model = format::MaterialModel::DiffuseConductorMix;
    materials[3].Metallic = 0.4F;
    materials[3].BaseColor = {0.3F, 0.6F, 0.9F};
    constexpr usize SIDE = 128;
    for (usize index = 0; index < materials.GetSize(); ++index)
    {
        const rr::ReferenceBsdf& bsdf = materials[index];
        for (const float64 degrees : {10.0, 60.0, 85.0})
        {
            CAPTURE(index, degrees);
            const math::Vector3 viewWorld = MakeView(degrees);
            // The density over the hemisphere equals the accepted fraction of proposals; below-horizon
            // visible-normal proposals are null events that keep their mass.
            float64 densityIntegral = 0;
            for (usize y = 0; y < SIDE; ++y)
            {
                const float32 cosine = (float32(y) + 0.5F) / float32(SIDE);
                const float32 sine = math::Sqrt(1 - cosine * cosine);
                for (usize x = 0; x < SIDE; ++x)
                {
                    const float32 azimuthSample = (float32(x) + 0.5F) / float32(SIDE);
                    const float32 angle = math::kTauF * azimuthSample;
                    const math::Vector3 lightWorld{sine * math::Cos(angle), sine * math::Sin(angle), cosine};
                    rr::ReferenceBsdfEvaluation forward;
                    rr::ReferenceBsdfEvaluation reverse;
                    REQUIRE(rr::EvaluateReferenceBsdf(bsdf, NORMAL_WORLD, viewWorld, lightWorld, forward) ==
                            rhi::RasterStatus::Ready);
                    REQUIRE(rr::EvaluateReferenceBsdf(bsdf, NORMAL_WORLD, lightWorld, viewWorld, reverse) ==
                            rhi::RasterStatus::Ready);
                    // The compensation lobe is symmetric in its two directions, like the single-scattering lobe.
                    CHECK(std::fabs(forward.Value.X - reverse.Value.X) <= 1e-5F * (1 + forward.Value.X));
                    CHECK(std::fabs(forward.Value.Z - reverse.Value.Z) <= 1e-5F * (1 + forward.Value.Z));
                    densityIntegral += static_cast<float64>(forward.Pdf) * math::kTau;
                    for (const float32 lobeSample : {0.1F, 0.6F, 0.95F})
                    {
                        rr::ReferenceBsdfSample sampled;
                        REQUIRE(rr::SampleReferenceBsdf(bsdf,
                                                        NORMAL_WORLD,
                                                        viewWorld,
                                                        {lobeSample, cosine, azimuthSample},
                                                        sampled) == rhi::RasterStatus::Ready);
                        if (sampled.Accepted == false)
                        {
                            continue;
                        }
                        rr::ReferenceBsdfEvaluation matched;
                        REQUIRE(rr::EvaluateReferenceBsdf(bsdf, NORMAL_WORLD, viewWorld, sampled.Direction, matched) ==
                                rhi::RasterStatus::Ready);
                        CHECK(sampled.HasProportionalPdf == false);
                        CHECK(sampled.MisPdf == sampled.Evaluation.Pdf);
                        CHECK(matched.Pdf == sampled.Evaluation.Pdf);
                        CHECK(matched.Value == sampled.Evaluation.Value);
                        const math::Vector3 weight =
                            matched.Value * (math::Dot(NORMAL_WORLD, sampled.Direction) / matched.Pdf);
                        CHECK(math::Distance(weight, sampled.Weight) <= 1e-5F * (1 + math::Length(weight)));
                    }
                }
            }
            // The accepted fraction of proposals over Halton points, which keep the lobe choice and the disk samples
            // jointly well distributed.
            constexpr uint32 PROPOSALS = 1U << 14;
            usize stratifiedAccepted = 0;
            for (uint32 proposal = 0; proposal < PROPOSALS; ++proposal)
            {
                rr::ReferenceBsdfSample sampled;
                REQUIRE(rr::SampleReferenceBsdf(bsdf, NORMAL_WORLD, viewWorld, MakeHaltonSample(proposal), sampled) ==
                        rhi::RasterStatus::Ready);
                stratifiedAccepted += sampled.Accepted == true ? 1 : 0;
            }
            const float64 acceptedFraction = float64(stratifiedAccepted) / float64(PROPOSALS);
            CAPTURE(densityIntegral / float64(SIDE * SIDE), acceptedFraction);
            CHECK(std::fabs(densityIntegral / float64(SIDE * SIDE) - acceptedFraction) < 0.01);
            CHECK(densityIntegral / float64(SIDE * SIDE) <= 1.005);
        }
    }
}

TEST_CASE("The material profile is part of the BSDF input and the default keeps single scattering",
          "[renderer][rt1][materials][multiple-scattering]")
{
    const math::Vector3 viewWorld = MakeView(40);
    const math::Vector3 lightWorld = MakeView(-25);
    rr::ReferenceBsdf legacy = MakeWhiteMetal(0.7F, SINGLE);
    rr::ReferenceBsdf defaulted;
    defaulted.BaseColor = legacy.BaseColor;
    defaulted.Metallic = legacy.Metallic;
    defaulted.Roughness = legacy.Roughness;
    rr::ReferenceBsdfEvaluation first;
    rr::ReferenceBsdfEvaluation second;
    REQUIRE(rr::EvaluateReferenceBsdf(legacy, NORMAL_WORLD, viewWorld, lightWorld, first) == rhi::RasterStatus::Ready);
    REQUIRE(rr::EvaluateReferenceBsdf(defaulted, NORMAL_WORLD, viewWorld, lightWorld, second) ==
            rhi::RasterStatus::Ready);
    CHECK(first.Value == second.Value);
    CHECK(first.Pdf == second.Pdf);
    rr::ReferenceBsdf compensated = MakeWhiteMetal(0.7F, MULTIPLE);
    REQUIRE(rr::EvaluateReferenceBsdf(compensated, NORMAL_WORLD, viewWorld, lightWorld, second) ==
            rhi::RasterStatus::Ready);
    CHECK(second.Value.X > first.Value.X);
    // Diffuse, dielectric and project BRDF models have no GGX lobe to compensate.
    rr::ReferenceBsdf diffuse;
    diffuse.Scattering.Model = format::MaterialModel::Diffuse;
    diffuse.Profile = MULTIPLE;
    REQUIRE(rr::EvaluateReferenceBsdf(diffuse, NORMAL_WORLD, viewWorld, lightWorld, second) ==
            rhi::RasterStatus::Ready);
    CHECK(second.Value.X == diffuse.BaseColor.X / math::kPiF);
    rr::ReferenceBsdf invalid = compensated;
    invalid.Profile = static_cast<rr::ReferenceMaterialProfile>(7);
    rr::ReferenceBsdfEvaluation preserved = first;
    CHECK(rr::EvaluateReferenceBsdf(invalid, NORMAL_WORLD, viewWorld, lightWorld, preserved) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(preserved.Value == first.Value);
    rr::ReferenceBsdfSample sampled;
    CHECK(rr::SampleReferenceBsdf(invalid, NORMAL_WORLD, viewWorld, {0.5F, 0.5F, 0.5F}, sampled) ==
          rhi::RasterStatus::InvalidDescription);
}

TEST_CASE("The higher-order Smith oracle reproduces RT1's single scattering and conserves lossless energy",
          "[renderer][rt1][materials][multiple-scattering][oracle]")
{
    // The first-order term of the random walk is GGX with height-correlated Smith masking-shadowing: RT1's
    // single-scattering lobe. Lossless walks keep their energy apart from the truncated orders.
    OracleRandom random(0x0dd5eed);
    for (const float32 alpha : {0.25F, 0.64F, 1.0F})
    {
        const MicrofacetRandomWalk lossless(Widen(alpha), {}, {}, true);
        const MicrofacetRandomWalk gold(Widen(alpha), Widen(GOLD_ETA), Widen(GOLD_K), false);
        for (const float64 degrees : {0.0, 60.0, 85.0})
        {
            const math::Vector3 viewWorld = MakeView(degrees);
            const Double3 albedo = lossless.MeasureAlbedo(Widen(viewWorld), 1U << 14, random);
            CAPTURE(alpha, degrees, albedo.X);
            CHECK(albedo.X >= 0.999);
            CHECK(albedo.X <= 1);
            for (const float64 lightDegrees : {-40.0, 10.0, 30.0, 70.0})
            {
                const math::Vector3 lightWorld = MakeView(lightDegrees);
                const Double3 reference = gold.Evaluate(Widen(viewWorld), Widen(lightWorld), 1U << 15, random, 1);
                rr::ReferenceBsdfEvaluation evaluated;
                REQUIRE(rr::EvaluateReferenceBsdf(MakeConductor(alpha, GOLD_ETA, GOLD_K, SINGLE),
                                                  NORMAL_WORLD,
                                                  viewWorld,
                                                  lightWorld,
                                                  evaluated) == rhi::RasterStatus::Ready);
                const Double3 expected = Widen(evaluated.Value) * Widen(lightWorld.Z);
                CAPTURE(lightDegrees, reference.X, reference.Y, reference.Z, expected.X, expected.Y, expected.Z);
                // Monte Carlo error of 32768 walks: the first event's height is the only random quantity.
                CHECK(std::fabs(reference.X - expected.X) <= 0.01 * expected.X + 1e-4);
                CHECK(std::fabs(reference.Z - expected.Z) <= 0.01 * expected.Z + 1e-4);
            }
        }
    }
}

TEST_CASE("Compensated absorbing metals approach the higher-order Smith albedo per channel",
          "[renderer][rt1][materials][multiple-scattering][oracle]")
{
    // Energy against Heitz et al.'s random walk. The revised Fresnel multiplier has Favg^2 in its numerator; with a
    // single Favg the compensation would exceed the reference for colored metals. Kulla and Conty's lobe approximates
    // the higher-order energy: the largest measured per-channel error is 0.043 (single scattering: 0.62), in the
    // weakly reflecting channels of the roughest metal, where it stays below the reference.
    constexpr StaticArray<float32, 3> ACHROMATIC_ETA{{0.374F, 0.374F, 0.374F}};
    constexpr StaticArray<float32, 3> ACHROMATIC_K{{2.385F, 2.385F, 2.385F}};
    constexpr float64 ENERGY_TOLERANCE = 0.05;
    float64 largestCompensatedError = 0;
    float64 largestSingleError = 0;
    OracleRandom random(0x5eed);
    for (const bool isGold : {true, false})
    {
        const StaticArray<float32, 3>& eta = isGold == true ? GOLD_ETA : ACHROMATIC_ETA;
        const StaticArray<float32, 3>& extinction = isGold == true ? GOLD_K : ACHROMATIC_K;
        for (const float32 alpha : {0.25F, 0.64F, 1.0F})
        {
            const MicrofacetRandomWalk walk(Widen(alpha), Widen(eta), Widen(extinction), false);
            for (const float64 degrees : {0.0, 45.0, 75.0})
            {
                const math::Vector3 viewWorld = MakeView(degrees);
                const Double3 reference = walk.MeasureAlbedo(Widen(viewWorld), 1U << 16, random);
                const math::Vector3 compensated =
                    MeasureSampledAlbedo(MakeConductor(alpha, eta, extinction, MULTIPLE), viewWorld, 1U << 14);
                const math::Vector3 single =
                    MeasureSampledAlbedo(MakeConductor(alpha, eta, extinction, SINGLE), viewWorld, 1U << 14);
                CAPTURE(isGold, alpha, degrees);
                CAPTURE(reference.X, reference.Y, reference.Z, compensated.X, compensated.Y, compensated.Z);
                CAPTURE(single.X, single.Y, single.Z);
                const StaticArray<float64, 3> referenceChannels{{reference.X, reference.Y, reference.Z}};
                const StaticArray<float32, 3> compensatedChannels{{compensated.X, compensated.Y, compensated.Z}};
                const StaticArray<float32, 3> singleChannels{{single.X, single.Y, single.Z}};
                for (usize channel = 0; channel < 3; ++channel)
                {
                    const float64 error = std::fabs(Widen(compensatedChannels[channel]) - referenceChannels[channel]);
                    const float64 singleError = std::fabs(Widen(singleChannels[channel]) - referenceChannels[channel]);
                    largestCompensatedError = std::fmax(largestCompensatedError, error);
                    largestSingleError = std::fmax(largestSingleError, singleError);
                    CHECK(error <= ENERGY_TOLERANCE);
                    CHECK(error <= singleError + 0.002);
                }
                if (isGold == false)
                {
                    CHECK(compensated.X == compensated.Y);
                    CHECK(compensated.X == compensated.Z);
                }
            }
        }
    }
    WARN("Largest absorbing-metal albedo error: compensated " << largestCompensatedError << ", single "
                                                              << largestSingleError);
}
