#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <limits>

using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;

TEST_CASE("Typed diffuse has Lambertian energy and conductor Fresnel preserves complex IOR",
          "[renderer][rt1][materials]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf diffuse;
    diffuse.BaseColor = {0.2F, 0.4F, 0.6F};
    diffuse.Scattering.Model = format::MaterialModel::Diffuse;
    rr::ReferenceBsdfEvaluation evaluated;
    REQUIRE(rr::EvaluateReferenceBsdf(diffuse, normalWorld, normalWorld, normalWorld, evaluated) ==
            rhi::RasterStatus::Ready);
    CHECK(evaluated.Value.X == Catch::Approx(diffuse.BaseColor.X / math::kPiF));
    CHECK(evaluated.Value.Y == Catch::Approx(diffuse.BaseColor.Y / math::kPiF));
    CHECK(evaluated.Pdf == Catch::Approx(1 / math::kPiF));
    for (const float32 lobeSample : {0.0F, 0.5F, 0.999F})
    {
        rr::ReferenceBsdfSample sampled;
        REQUIRE(rr::SampleReferenceBsdf(diffuse, normalWorld, normalWorld, {lobeSample, 0.3F, 0.7F}, sampled) ==
                rhi::RasterStatus::Ready);
        REQUIRE(sampled.Accepted == true);
        CHECK(sampled.Evaluation.Value.X * sampled.Direction.Z / sampled.Evaluation.Pdf ==
              Catch::Approx(diffuse.BaseColor.X));
    }
    rr::ReferenceBsdf conductor;
    conductor.Scattering.Model = format::MaterialModel::Conductor;
    conductor.Scattering.Eta[0] = 1.65746F;
    conductor.Scattering.Eta[1] = 0.880369F;
    conductor.Scattering.Eta[2] = 0.521229F;
    conductor.Scattering.K[0] = 9.223869F;
    conductor.Scattering.K[1] = 6.269523F;
    conductor.Scattering.K[2] = 4.837001F;
    conductor.Scattering.AlphaU = 0.1F;
    conductor.Scattering.AlphaV = 0.1F;
    for (const float32 cosine : {1.0F, 0.6F, 0.1F})
    {
        const float32 sine = math::Sqrt(1 - cosine * cosine);
        const math::Vector3 viewWorld{sine, 0, cosine};
        const math::Vector3 lightWorld{-sine, 0, cosine};
        REQUIRE(rr::EvaluateReferenceBsdf(conductor, normalWorld, viewWorld, lightWorld, evaluated) ==
                rhi::RasterStatus::Ready);
        const float64 alpha = static_cast<float64>(conductor.Scattering.AlphaU);
        const float64 distribution = 1 / (math::kPi * alpha * alpha);
        const float64 smithRoot = std::sqrt(float64(cosine * cosine) * (1 - alpha * alpha) + alpha * alpha);
        const float64 visibility = 1 / (4 * static_cast<float64>(cosine) * smithRoot);
        const StaticArray<float32, format::MATERIAL_RGB_CHANNELS> channels{
            {evaluated.Value.X, evaluated.Value.Y, evaluated.Value.Z}};
        for (usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
        {
            // Independent complex-amplitude Fresnel oracle, rather than the production real-valued algebra.
            const std::complex<float64> index(static_cast<float64>(conductor.Scattering.Eta[channel]),
                                              static_cast<float64>(conductor.Scattering.K[channel]));
            const std::complex<float64> square = index * index;
            const std::complex<float64> root = std::sqrt(square - float64(1 - cosine * cosine));
            const std::complex<float64> perpendicular = (float64(cosine) - root) / (float64(cosine) + root);
            const std::complex<float64> parallel =
                (square * float64(cosine) - root) / (square * float64(cosine) + root);
            const float64 fresnel = (std::norm(perpendicular) + std::norm(parallel)) * 0.5;
            CHECK(channels[channel] == Catch::Approx(fresnel * distribution * visibility).epsilon(0.00002));
        }
    }
    const rr::ReferenceBsdfEvaluation previous = evaluated;
    conductor.Scattering.AlphaV = 0.2F;
    CHECK(rr::EvaluateReferenceBsdf(conductor, normalWorld, normalWorld, normalWorld, evaluated) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(evaluated.Value == previous.Value);
    conductor.Scattering.AlphaV = conductor.Scattering.AlphaU;
    conductor.Scattering.Eta[0] = std::numeric_limits<float32>::infinity();
    CHECK(rr::EvaluateReferenceBsdf(conductor, normalWorld, normalWorld, normalWorld, evaluated) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(evaluated.Pdf == previous.Pdf);
}

TEST_CASE("Conductor visible-normal sampling preserves its density, null mass and energy", "[renderer][rt1][materials]")
{
    namespace format = ludus::content::scene_format;
    constexpr usize SIDE = 128;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf conductor;
    conductor.BaseColor = {}; // Conductor Fresnel comes from eta/k, not the legacy base colour.
    conductor.Scattering.Model = format::MaterialModel::Conductor;
    conductor.Scattering.AlphaU = 0.6F;
    conductor.Scattering.AlphaV = 0.6F;
    for (usize channel = 0; channel < format::MATERIAL_RGB_CHANNELS; ++channel)
    {
        conductor.Scattering.Eta[channel] = 1.6F;
        conductor.Scattering.K[channel] = 3.0F;
    }
    for (const float32 viewCosine : {1.0F, 0.2F})
    {
        const math::Vector3 viewWorld{math::Sqrt(1 - viewCosine * viewCosine), 0, viewCosine};
        float64 densityIntegral = 0;
        float64 energyIntegral = 0;
        float64 sampledEnergy = 0;
        usize accepted = 0;
        for (usize y = 0; y < SIDE; ++y)
        {
            const float32 cosine = (static_cast<float32>(y) + 0.5F) / static_cast<float32>(SIDE);
            const float32 sine = math::Sqrt(1 - cosine * cosine);
            for (usize x = 0; x < SIDE; ++x)
            {
                const float32 azimuthSample = (static_cast<float32>(x) + 0.5F) / static_cast<float32>(SIDE);
                const float32 angle = math::kTauF * azimuthSample;
                const math::Vector3 lightWorld{sine * math::Cos(angle), sine * math::Sin(angle), cosine};
                rr::ReferenceBsdfEvaluation evaluated;
                REQUIRE(rr::EvaluateReferenceBsdf(conductor, normalWorld, viewWorld, lightWorld, evaluated) ==
                        rhi::RasterStatus::Ready);
                densityIntegral += static_cast<float64>(evaluated.Pdf) * math::kTau;
                energyIntegral += static_cast<float64>(evaluated.Value.X * cosine) * math::kTau;
                rr::ReferenceBsdfSample sampled;
                REQUIRE(rr::SampleReferenceBsdf(conductor,
                                                normalWorld,
                                                viewWorld,
                                                {0.5F, cosine, azimuthSample},
                                                sampled) == rhi::RasterStatus::Ready);
                if (sampled.Accepted == true)
                {
                    ++accepted;
                    rr::ReferenceBsdfEvaluation matched;
                    REQUIRE(rr::EvaluateReferenceBsdf(conductor, normalWorld, viewWorld, sampled.Direction, matched) ==
                            rhi::RasterStatus::Ready);
                    CHECK(matched.Pdf == sampled.Evaluation.Pdf);
                    sampledEnergy += static_cast<float64>(matched.Value.X * sampled.Direction.Z / matched.Pdf);
                }
            }
        }
        const float64 sampleCount = static_cast<float64>(SIDE * SIDE);
        CHECK(math::Abs(densityIntegral / sampleCount - static_cast<float64>(accepted) / sampleCount) < 0.004);
        CHECK(math::Abs(energyIntegral / sampleCount - sampledEnergy / sampleCount) < 0.01);
        CHECK(sampledEnergy / sampleCount <= 1.001);
        CHECK(accepted < SIDE * SIDE);
    }
}

TEST_CASE("Non-scattering coatings preserve interface and absorption limits", "[renderer][rt1][coating]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf coated;
    coated.Scattering.Model = format::MaterialModel::CoatedDiffuse;
    coated.Scattering.AlphaU = 0.1F;
    coated.Scattering.AlphaV = coated.Scattering.AlphaU;
    coated.BaseColor = {};
    rr::ReferenceBsdfEvaluation evaluation;
    REQUIRE(rr::EvaluateReferenceBsdf(coated, normalWorld, normalWorld, normalWorld, evaluation) ==
            rhi::RasterStatus::Ready);
    const float64 eta = static_cast<float64>(coated.Scattering.CoatingEta);
    const float64 normalFresnel = (eta - 1) * (eta - 1) / ((eta + 1) * (eta + 1));
    const float64 alpha = static_cast<float64>(coated.Scattering.AlphaU);
    CHECK(evaluation.Value.X == Catch::Approx(normalFresnel / (4 * math::kPi * alpha * alpha)).epsilon(0.00002));
    CHECK(evaluation.Value.X == evaluation.Value.Y);
    CHECK(evaluation.Value.X == evaluation.Value.Z);
    coated.BaseColor = {0.2F, 0.4F, 0.6F};
    coated.Scattering.CoatingEta = 1;
    coated.Scattering.CoatingThickness = 0.2F;
    const math::Vector3 viewWorld{0.6F, 0, 0.8F};
    const math::Vector3 lightWorld{0, 0.6F, 0.8F};
    REQUIRE(rr::EvaluateReferenceBsdf(coated, normalWorld, viewWorld, lightWorld, evaluation) ==
            rhi::RasterStatus::Ready);
    const float64 attenuation = std::exp(-2 * static_cast<float64>(coated.Scattering.CoatingThickness) / 0.8);
    CHECK(evaluation.Value.X == Catch::Approx(0.2 * attenuation / math::kPi).epsilon(0.00001));
    CHECK(evaluation.Pdf == Catch::Approx(0.8 / math::kPi));
    rr::ReferenceBsdfSample sampled;
    REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, viewWorld, {0.5F, 0.36F, 0.25F}, sampled) ==
            rhi::RasterStatus::Ready);
    REQUIRE(sampled.Accepted == true);
    CHECK(sampled.Weight.X == Catch::Approx(0.2 * attenuation).epsilon(0.00001));
    CHECK(sampled.MisPdf == sampled.Evaluation.Pdf);
    CHECK(sampled.HasProportionalPdf == false);
    coated.Scattering.CoatingThickness = 0;
    REQUIRE(rr::EvaluateReferenceBsdf(coated, normalWorld, viewWorld, lightWorld, evaluation) ==
            rhi::RasterStatus::Ready);
    CHECK(evaluation.Value.Z == Catch::Approx(0.6 / math::kPi).epsilon(0.00001));
}

TEST_CASE("Coating sampling separates the internal path ratio from the external MIS density",
          "[renderer][rt1][coating]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf coated;
    coated.Scattering.Model = format::MaterialModel::CoatedDiffuse;
    coated.Scattering.AlphaU = 0.1F;
    coated.Scattering.AlphaV = coated.Scattering.AlphaU;
    rr::ReferenceBsdfRandomContext random{{871}, {2, 3, 4}};
    rr::ReferenceBsdfSample sampled;
    REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0, 0.25F, 0.5F}, sampled, random) ==
            rhi::RasterStatus::Ready);
    REQUIRE(sampled.Accepted == true);
    REQUIRE(sampled.HasProportionalPdf == true);
    CHECK(sampled.Weight.X == Catch::Approx(sampled.Evaluation.Value.X * sampled.Direction.Z / sampled.Evaluation.Pdf));
    CHECK(sampled.MisPdf != sampled.Evaluation.Pdf);
    const rr::ReferenceBsdfSample previous = sampled;
    rr::ReferenceBsdfSample replay;
    REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0, 0.25F, 0.5F}, replay, random) ==
            rhi::RasterStatus::Ready);
    CHECK(replay.Weight == sampled.Weight);
    CHECK(replay.MisPdf == sampled.MisPdf);
    usize internalSamples = 0;
    for (uint32 event = 0; event < 16; ++event)
    {
        random.Address.Event = event;
        REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0.9F, 0.25F, 0.5F}, sampled, random) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0.9F, 0.25F, 0.5F}, replay, random) ==
                rhi::RasterStatus::Ready);
        CHECK(replay.Accepted == sampled.Accepted);
        CHECK(replay.Weight == sampled.Weight);
        CHECK(replay.MisPdf == sampled.MisPdf);
        if (sampled.Accepted == true)
        {
            ++internalSamples;
            CHECK(sampled.HasProportionalPdf == true);
            CHECK(math::IsFinite(sampled.Weight) == true);
            CHECK(sampled.Direction.Z > 0);
        }
    }
    CHECK(internalSamples > 0);
    sampled = previous;
    random.Address.Dimension = 256;
    CHECK(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0.5F, 0.25F, 0.5F}, sampled, random) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(sampled.Weight == previous.Weight);
    coated.Scattering.AlphaV = 0.2F;
    CHECK(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0.5F, 0.25F, 0.5F}, sampled) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(sampled.MisPdf == previous.MisPdf);
    coated.Scattering.AlphaV = coated.Scattering.AlphaU;
    coated.Scattering.AlphaU = 0.0001F;
    coated.Scattering.AlphaV = coated.Scattering.AlphaU;
    rr::ReferenceBsdfEvaluation evaluation{{1, 2, 3}, 1};
    CHECK(rr::EvaluateReferenceBsdf(coated, normalWorld, normalWorld, normalWorld, evaluation) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(evaluation.Value.Z == 3);
}

TEST_CASE("Coating estimates agree with the independent PBRT layered oracle", "[renderer][rt1][coating][oracle]")
{
    namespace format = ludus::content::scene_format;
    // Measured offline against unmodified pbrt-v4 material kernels at
    // b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f: 131072 samples, IOR 1.5,
    // thickness .01, no scattering, depth 10, one internal evaluation sample.
    // Thanks to Pharr, Jakob and Humphreys, PBRT 4e section 14.3, for this independent oracle.
    struct Measurement
    {
        float32 Alpha;
        float32 Reflectance;
        float64 Value;
        float64 ValueError;
        float64 Weight;
        float64 WeightError;
        float64 Density;
        float64 DensityError;
        float64 Accepted;
    };
    constexpr usize CASE_COUNT = 6;
    constexpr StaticArray<Measurement, CASE_COUNT> ORACLE{{
        {0.1F,
         0.2F,
         0.112133193194,
         2.25474559871e-05,
         0.129240344993,
         0.000582959819025,
         1.93225527677,
         0.000133122234252,
         0.66153717041},
        {0.1F,
         0.6F,
         0.196730335636,
         0.000176696746338,
         0.375776747843,
         0.000867400659556,
         1.93225527677,
         0.000133122234252,
         0.688323974609},
        {0.1F,
         0.9F,
         0.301193521474,
         0.000355955495402,
         0.683766575343,
         0.00104433231264,
         1.93225527677,
         0.000133122234252,
         0.827980041504},
        {0.01F,
         0.2F,
         0.0312084743267,
         1.76426053184e-05,
         0.128703819385,
         0.000583538224266,
         0.332826469765,
         4.51051202792e-05,
         0.661376953125},
        {0.01F,
         0.6F,
         0.11769753608,
         0.000180522707737,
         0.384664723049,
         0.000876849778107,
         0.332826469765,
         4.51051202792e-05,
         0.693305969238},
        {0.01F,
         0.9F,
         0.228410981888,
         0.000358162583512,
         0.713778194388,
         0.000973605665431,
         0.332826469765,
         4.51051202792e-05,
         0.863464355469},
    }};
    constexpr uint32 SAMPLE_COUNT = 32768;
    constexpr float64 COUNT = SAMPLE_COUNT;
    constexpr float64 ORACLE_COUNT = 131072;
    constexpr float64 STANDARD_ERRORS = 6;
    constexpr float64 ROUNDING_ALLOWANCE = 0.00001;
    struct Moments
    {
        float64 Sum = 0;
        float64 Squared = 0;
    };
    const auto add = [](Moments& inoutMoments, float64 value) {
        inoutMoments.Sum += value;
        inoutMoments.Squared += value * value;
    };
    const auto error = [](const Moments& moments) {
        const float64 mean = moments.Sum / COUNT;
        return std::sqrt(math::Max(0.0, moments.Squared / COUNT - mean * mean) / COUNT);
    };
    const auto check = [&](const Moments& moments, float64 expected, float64 expectedError) {
        const float64 sampleError = error(moments);
        CHECK(math::Abs(moments.Sum / COUNT - expected) <=
              STANDARD_ERRORS * std::sqrt(sampleError * sampleError + expectedError * expectedError) +
                  ROUNDING_ALLOWANCE);
    };
    const math::Vector3 normalWorld{0, 0, 1};
    const math::Vector3 viewWorld{0.6F, 0, 0.8F};
    const math::Vector3 lightWorld{-0.4F, 0, math::Sqrt(0.84F)};
    for (const Measurement& oracle : ORACLE)
    {
        CAPTURE(oracle.Alpha, oracle.Reflectance);
        rr::ReferenceBsdf coated;
        coated.Scattering.Model = format::MaterialModel::CoatedDiffuse;
        coated.Scattering.AlphaU = oracle.Alpha;
        coated.Scattering.AlphaV = oracle.Alpha;
        coated.BaseColor = {oracle.Reflectance, oracle.Reflectance, oracle.Reflectance};
        Moments values;
        Moments reversed;
        Moments weights;
        Moments densities;
        Moments accepted;
        for (uint32 sample = 0; sample < SAMPLE_COUNT; ++sample)
        {
            rr::ReferenceBsdfRandomContext random{{871}, {2, sample, 0}};
            rr::ReferenceBsdfEvaluation evaluation;
            REQUIRE(rr::EvaluateReferenceBsdf(coated, normalWorld, viewWorld, lightWorld, evaluation, random) ==
                    rhi::RasterStatus::Ready);
            add(values, static_cast<float64>(evaluation.Value.X));
            add(densities, static_cast<float64>(evaluation.Pdf));
            random.Address.Scope = 3;
            REQUIRE(rr::EvaluateReferenceBsdf(coated, normalWorld, lightWorld, viewWorld, evaluation, random) ==
                    rhi::RasterStatus::Ready);
            add(reversed, static_cast<float64>(evaluation.Value.X));
            random.Address.Scope = 2;
            const math::Vector3 proposal{math::SampleFloat01(random.Key, {2, sample, 0}),
                                         math::SampleFloat01(random.Key, {2, sample, 1}),
                                         math::SampleFloat01(random.Key, {2, sample, 2})};
            rr::ReferenceBsdfSample continuation;
            REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, viewWorld, proposal, continuation, random) ==
                    rhi::RasterStatus::Ready);
            add(weights, continuation.Accepted == true ? static_cast<float64>(continuation.Weight.X) : 0);
            add(accepted, continuation.Accepted == true ? 1 : 0);
        }
        check(values, oracle.Value, oracle.ValueError);
        check(weights, oracle.Weight, oracle.WeightError);
        check(densities, oracle.Density, oracle.DensityError);
        check(accepted, oracle.Accepted, std::sqrt(oracle.Accepted * (1 - oracle.Accepted) / ORACLE_COUNT));
        check(reversed, values.Sum / COUNT, error(values));
        CHECK(weights.Sum / COUNT <= 1 + STANDARD_ERRORS * error(weights));
    }
}

TEST_CASE("Smooth dielectrics split by Fresnel reflectance and refract by Snell's law", "[renderer][rt1][dielectric]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf glass;
    glass.Scattering.Model = format::MaterialModel::Dielectric;
    glass.Scattering.Eta = {{1.5F, 1.5F, 1.5F}};
    glass.Scattering.AlphaU = format::MIN_MATERIAL_ALPHA;
    glass.Scattering.AlphaV = format::MIN_MATERIAL_ALPHA;
    const float32 incidentSine = 0.6F;
    const math::Vector3 viewWorld{incidentSine, 0, 0.8F};
    // Delta lobes have no density for any finite pair of directions.
    rr::ReferenceBsdfEvaluation evaluated;
    REQUIRE(rr::EvaluateReferenceBsdf(glass, normalWorld, viewWorld, {-incidentSine, 0, 0.8F}, evaluated) ==
            rhi::RasterStatus::Ready);
    CHECK(evaluated.Value == math::Vector3{});
    CHECK(evaluated.Pdf == 0);
    // Independent Fresnel oracle in float64: s and p amplitudes at a 0.8 cosine.
    for (const bool isViewerInside : {false, true})
    {
        CAPTURE(isViewerInside);
        glass.IsViewerInside = isViewerInside;
        const float64 eta = isViewerInside == true ? 1 / 1.5 : 1.5;
        const float64 incidentCosine = 0.8;
        const float64 transmittedSine = static_cast<float64>(incidentSine) / eta;
        const float64 transmittedCosine = math::Sqrt(1 - transmittedSine * transmittedSine);
        const float64 perpendicular =
            (incidentCosine - eta * transmittedCosine) / (incidentCosine + eta * transmittedCosine);
        const float64 parallel =
            (eta * incidentCosine - transmittedCosine) / (eta * incidentCosine + transmittedCosine);
        const float64 reflectance = (perpendicular * perpendicular + parallel * parallel) * 0.5;
        rr::ReferenceBsdfSample reflected;
        REQUIRE(rr::SampleReferenceBsdf(glass,
                                        normalWorld,
                                        viewWorld,
                                        {static_cast<float32>(reflectance) * 0.99F, 0.5F, 0.5F},
                                        reflected) == rhi::RasterStatus::Ready);
        REQUIRE(reflected.Accepted == true);
        CHECK(reflected.IsSpecular == true);
        CHECK(reflected.MisPdf == 0);
        CHECK(reflected.Direction.X == Catch::Approx(-incidentSine));
        CHECK(reflected.Direction.Z == Catch::Approx(0.8F));
        CHECK(reflected.Weight == math::Vector3{1, 1, 1});
        rr::ReferenceBsdfSample transmitted;
        REQUIRE(rr::SampleReferenceBsdf(glass,
                                        normalWorld,
                                        viewWorld,
                                        {static_cast<float32>(reflectance) * 1.01F, 0.5F, 0.5F},
                                        transmitted) == rhi::RasterStatus::Ready);
        REQUIRE(transmitted.Accepted == true);
        CHECK(transmitted.IsSpecular == true);
        // Snell: the transmitted direction lies below the surface with sine sin(theta_i) / eta.
        CHECK(transmitted.Direction.X == Catch::Approx(-transmittedSine).margin(1e-6));
        CHECK(transmitted.Direction.Z == Catch::Approx(-transmittedCosine).margin(1e-6));
        // Radiance transport: the selected lobe's probability cancels its Fresnel factor and 1 / eta^2 remains.
        CHECK(transmitted.Weight.Y == Catch::Approx(1 / (eta * eta)));
    }
    // Total internal reflection: from inside at a sine beyond 1 / 1.5, every sample reflects.
    glass.IsViewerInside = true;
    const math::Vector3 grazingWorld{0.8F, 0, 0.6F};
    for (const float32 lobeSample : {0.0F, 0.5F, 0.999F})
    {
        rr::ReferenceBsdfSample sampled;
        REQUIRE(rr::SampleReferenceBsdf(glass, normalWorld, grazingWorld, {lobeSample, 0.5F, 0.5F}, sampled) ==
                rhi::RasterStatus::Ready);
        CHECK(sampled.Direction.Z == Catch::Approx(0.6F));
        CHECK(sampled.Weight == math::Vector3{1, 1, 1});
    }
}

TEST_CASE("Thin dielectrics sum their interreflections and pass light undeviated", "[renderer][rt1][dielectric]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf pane;
    pane.Scattering.Model = format::MaterialModel::ThinDielectric;
    pane.Scattering.Eta = {{1.5F, 1.5F, 1.5F}};
    pane.Scattering.AlphaU = format::MIN_MATERIAL_ALPHA;
    pane.Scattering.AlphaV = format::MIN_MATERIAL_ALPHA;
    // At normal incidence R = 0.04 and the slab reflects R + T^2 R / (1 - R^2) = 2R / (1 + R).
    const float32 slabReflectance = 2 * 0.04F / 1.04F;
    for (const bool isViewerInside : {false, true})
    {
        pane.IsViewerInside = isViewerInside; // A thin slab is symmetric.
        rr::ReferenceBsdfSample reflected;
        REQUIRE(rr::SampleReferenceBsdf(pane, normalWorld, normalWorld, {slabReflectance * 0.99F, 0, 0}, reflected) ==
                rhi::RasterStatus::Ready);
        CHECK(reflected.Direction.Z == Catch::Approx(1));
        rr::ReferenceBsdfSample transmitted;
        REQUIRE(rr::SampleReferenceBsdf(pane, normalWorld, normalWorld, {slabReflectance * 1.01F, 0, 0}, transmitted) ==
                rhi::RasterStatus::Ready);
        CHECK(transmitted.Direction.Z == Catch::Approx(-1));
        CHECK(transmitted.Weight == math::Vector3{1, 1, 1});
    }
    const math::Vector3 viewWorld{0.6F, 0, 0.8F};
    rr::ReferenceBsdfSample straight;
    REQUIRE(rr::SampleReferenceBsdf(pane, normalWorld, viewWorld, {0.999F, 0, 0}, straight) ==
            rhi::RasterStatus::Ready);
    CHECK(straight.Direction == -viewWorld);
}

TEST_CASE("Reference dielectrics admit one real index and a smooth interface only", "[renderer][rt1][dielectric]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf glass;
    glass.Scattering.Model = format::MaterialModel::Dielectric;
    glass.Scattering.Eta = {{1.5F, 1.5F, 1.5F}};
    glass.Scattering.AlphaU = format::MIN_MATERIAL_ALPHA;
    glass.Scattering.AlphaV = format::MIN_MATERIAL_ALPHA;
    rr::ReferenceBsdfSample sampled;
    REQUIRE(rr::SampleReferenceBsdf(glass, normalWorld, normalWorld, {0.5F, 0, 0}, sampled) ==
            rhi::RasterStatus::Ready);
    const rr::ReferenceBsdfSample previous = sampled;
    rr::ReferenceBsdf dispersive = glass;
    dispersive.Scattering.Eta[2] = 1.52F;
    rr::ReferenceBsdf absorbing = glass;
    absorbing.Scattering.K[0] = 0.1F;
    rr::ReferenceBsdf rough = glass;
    rough.Scattering.AlphaU = format::LUDUS_SMOOTH_INTERFACE_ALPHA;
    rough.Scattering.AlphaV = format::LUDUS_SMOOTH_INTERFACE_ALPHA;
    for (const rr::ReferenceBsdf& rejected : {dispersive, absorbing, rough})
    {
        CHECK(rr::SampleReferenceBsdf(rejected, normalWorld, normalWorld, {0.9F, 0, 0}, sampled) ==
              rhi::RasterStatus::InvalidDescription);
        CHECK(sampled.Direction == previous.Direction);
    }
    // An index-matched interface never reflects.
    glass.Scattering.Eta = {{1, 1, 1}};
    REQUIRE(rr::SampleReferenceBsdf(glass, normalWorld, {0.6F, 0, 0.8F}, {0, 0, 0}, sampled) ==
            rhi::RasterStatus::Ready);
    CHECK(sampled.Direction.X == Catch::Approx(-0.6F));
    CHECK(sampled.Direction.Z == Catch::Approx(-0.8F));
}

TEST_CASE("Diffuse conductor Mix samples the full weighted directional density", "[renderer][rt1][materials][mix]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf conductor;
    conductor.Scattering.Model = format::MaterialModel::Conductor;
    conductor.Scattering.AlphaU = 0.3F;
    conductor.Scattering.AlphaV = conductor.Scattering.AlphaU;
    conductor.Scattering.Eta = {{0.7F, 0.8F, 0.9F}};
    conductor.Scattering.K = {{3, 4, 5}};
    rr::ReferenceBsdf diffuse = conductor;
    diffuse.BaseColor = {0.2F, 0.4F, 0.8F};
    diffuse.Scattering.Model = format::MaterialModel::Diffuse;
    rr::ReferenceBsdf mixed = conductor;
    mixed.BaseColor = diffuse.BaseColor;
    mixed.Scattering.Model = format::MaterialModel::DiffuseConductorMix;
    for (const float32 weight : {0.0F, 0.25F, 1.0F})
    {
        mixed.Metallic = weight;
        for (const float32 selector : {0.125F, 0.625F})
        {
            const math::Vector3 random{selector, 0.2F, 0.7F};
            const rr::ReferenceBsdf& selected = selector < weight ? conductor : diffuse;
            rr::ReferenceBsdfSample lobe;
            rr::ReferenceBsdfSample sample;
            REQUIRE(rr::SampleReferenceBsdf(selected, normalWorld, normalWorld, random, lobe) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(rr::SampleReferenceBsdf(mixed, normalWorld, normalWorld, random, sample) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(lobe.Accepted == true);
            REQUIRE(sample.Accepted == true);
            CHECK(sample.Direction == lobe.Direction);
            rr::ReferenceBsdfEvaluation matte;
            rr::ReferenceBsdfEvaluation metal;
            REQUIRE(rr::EvaluateReferenceBsdf(diffuse, normalWorld, normalWorld, sample.Direction, matte) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(rr::EvaluateReferenceBsdf(conductor, normalWorld, normalWorld, sample.Direction, metal) ==
                    rhi::RasterStatus::Ready);
            const float32 density = (1 - weight) * matte.Pdf + weight * metal.Pdf;
            const math::Vector3 value = matte.Value * (1 - weight) + metal.Value * weight;
            CHECK(sample.MisPdf == Catch::Approx(density));
            CHECK(math::Distance(sample.Evaluation.Value, value) < 1e-6F);
            CHECK(math::Distance(sample.Weight, value * (sample.Direction.Z / density)) < 1e-6F);
        }
    }
}
