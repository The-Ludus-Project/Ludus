#include "material_program_fixtures.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>
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

TEST_CASE("A black coating substrate preserves interface reflection and absorbs transmitted proposals",
          "[renderer][rt1][coating]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    rr::ReferenceBsdf coated;
    coated.BaseColor = {};
    coated.Scattering.Model = format::MaterialModel::CoatedDiffuse;
    coated.Scattering.AlphaU = 0.1F;
    coated.Scattering.AlphaV = coated.Scattering.AlphaU;
    constexpr float64 REFLECTION_JACOBIAN = 4;
    constexpr float64 INTERFACE_PDF_WEIGHT = 0.9;
    const float64 alpha = static_cast<float64>(coated.Scattering.AlphaU);
    const float64 uniformDensity = 1 / (REFLECTION_JACOBIAN * math::kPi);
    // The MIS estimate blends the reflection-conditional GGX density with the uniform floor.
    const float64 expectedMisDensity =
        uniformDensity * (INTERFACE_PDF_WEIGHT / (alpha * alpha) + 1 - INTERFACE_PDF_WEIGHT);
    rr::ReferenceBsdfEvaluation evaluated;
    REQUIRE(rr::EvaluateReferenceBsdf(coated, normalWorld, normalWorld, normalWorld, evaluated) ==
            rhi::RasterStatus::Ready);
    CHECK(evaluated.Pdf == Catch::Approx(expectedMisDensity));

    rr::ReferenceBsdfSample sampled;
    REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0, 0, 0}, sampled) == rhi::RasterStatus::Ready);
    REQUIRE(sampled.Accepted == true);
    CHECK(sampled.Direction == normalWorld);
    CHECK(sampled.Weight.X == Catch::Approx(1));
    CHECK(sampled.HasProportionalPdf == true);
    CHECK(sampled.MisPdf == evaluated.Pdf);

    REQUIRE(rr::SampleReferenceBsdf(coated, normalWorld, normalWorld, {0.5F, 0, 0}, sampled) ==
            rhi::RasterStatus::Ready);
    CHECK(sampled.Accepted == false);
    CHECK(sampled.Weight == math::Vector3{});
    CHECK(sampled.MisPdf == 0);
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
    // Channel-wise indices are not a declared dispersion; K[1..2] would be absorption, and K[0] is an Abbe number.
    rr::ReferenceBsdf unequalChannels = glass;
    unequalChannels.Scattering.Eta[2] = 1.52F;
    rr::ReferenceBsdf absorbing = glass;
    absorbing.Scattering.K[1] = 0.1F;
    rr::ReferenceBsdf belowAbbeRange = glass;
    belowAbbeRange.Scattering.K[0] = format::MIN_DIELECTRIC_ABBE_NUMBER * 0.5F;
    rr::ReferenceBsdf rough = glass;
    rough.Scattering.AlphaU = format::LUDUS_SMOOTH_INTERFACE_ALPHA;
    rough.Scattering.AlphaV = format::LUDUS_SMOOTH_INTERFACE_ALPHA;
    for (const rr::ReferenceBsdf& rejected : {unequalChannels, absorbing, belowAbbeRange, rough})
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

TEST_CASE("A dispersive dielectric refracts each wavelength by Snell's law at its own index",
          "[renderer][rt1][dielectric][dispersion]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    // pbrt-v4's glass-F11 as the importers declare it.
    constexpr format::DielectricDispersion FLINT{ .IndexD = 1.784743F, .AbbeNumber = 25.7F };
    rr::ReferenceBsdf glass;
    glass.Scattering = format::MakeDielectricScattering(format::MaterialModel::Dielectric, FLINT);
    const float32 incidentSine = 0.6F;
    const math::Vector3 viewWorld{incidentSine, 0, 0.8F};
    // The index of each wavelength in float64 from the Cauchy coefficients that n_d and V_d determine, solved here
    // from n_F - n_C = (n_d - 1) / V_d instead of evaluated through ComputeDielectricIndex. Zero is RGB transport.
    const float64 lineF = static_cast<float64>(format::LUDUS_DIELECTRIC_F_LINE_NANOMETERS);
    const float64 lineD = static_cast<float64>(format::LUDUS_DIELECTRIC_D_LINE_NANOMETERS);
    const float64 lineC = static_cast<float64>(format::LUDUS_DIELECTRIC_C_LINE_NANOMETERS);
    const float64 indexD = static_cast<float64>(FLINT.IndexD);
    const float64 cauchyB =
        (indexD - 1) / static_cast<float64>(FLINT.AbbeNumber) / (1 / (lineF * lineF) - 1 / (lineC * lineC));
    const float64 cauchyA = indexD - cauchyB / (lineD * lineD);
    float64 previousSine = 0;
    for (const float32 wavelength : {0.0F, 780.0F, 656.2725F, 587.5618F, 486.1327F, 380.0F})
    {
        CAPTURE(wavelength);
        glass.WavelengthNanometers = wavelength;
        const float64 lambda = static_cast<float64>(wavelength);
        const float64 eta = wavelength == 0 ? indexD : cauchyA + cauchyB / (lambda * lambda);
        rr::ReferenceBsdfSample transmitted;
        // 0.999 exceeds every Fresnel reflectance at this incidence, so each sample refracts.
        REQUIRE(rr::SampleReferenceBsdf(glass, normalWorld, viewWorld, {0.999F, 0.5F, 0.5F}, transmitted) ==
                rhi::RasterStatus::Ready);
        REQUIRE(transmitted.Accepted == true);
        const float64 transmittedSine = static_cast<float64>(incidentSine) / eta;
        CHECK(transmitted.Direction.X == Catch::Approx(-transmittedSine).margin(1e-6));
        CHECK(transmitted.Direction.Z ==
              Catch::Approx(-math::Sqrt(1 - transmittedSine * transmittedSine)).margin(1e-6));
        CHECK(transmitted.Weight.Y == Catch::Approx(1 / (eta * eta)).epsilon(1e-5));
        // Normal dispersion: shorter wavelengths bend more, toward the normal.
        if (wavelength > 0)
        {
            const float64 sine = static_cast<float64>(-transmitted.Direction.X);
            CHECK((previousSine == 0 || sine < previousSine) == true);
            previousSine = sine;
        }
    }
    // RGB transport at zero wavelength and the d line both refract at n_d.
    rr::ReferenceBsdfSample atRgb;
    rr::ReferenceBsdfSample atLineD;
    glass.WavelengthNanometers = 0;
    REQUIRE(rr::SampleReferenceBsdf(glass, normalWorld, viewWorld, {0.999F, 0.5F, 0.5F}, atRgb) ==
            rhi::RasterStatus::Ready);
    glass.WavelengthNanometers = format::LUDUS_DIELECTRIC_D_LINE_NANOMETERS;
    REQUIRE(rr::SampleReferenceBsdf(glass, normalWorld, viewWorld, {0.999F, 0.5F, 0.5F}, atLineD) ==
            rhi::RasterStatus::Ready);
    CHECK(atRgb.Direction.X == Catch::Approx(atLineD.Direction.X).margin(1e-6));
    // A nondispersive index ignores the wavelength.
    rr::ReferenceBsdf plain = glass;
    plain.Scattering.K[0] = 0;
    rr::ReferenceBsdfSample atBlue;
    plain.WavelengthNanometers = 400;
    REQUIRE(rr::SampleReferenceBsdf(plain, normalWorld, viewWorld, {0.999F, 0.5F, 0.5F}, atBlue) ==
            rhi::RasterStatus::Ready);
    plain.WavelengthNanometers = 0;
    rr::ReferenceBsdfSample atNone;
    REQUIRE(rr::SampleReferenceBsdf(plain, normalWorld, viewWorld, {0.999F, 0.5F, 0.5F}, atNone) ==
            rhi::RasterStatus::Ready);
    CHECK(atBlue.Direction == atNone.Direction);
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

TEST_CASE("Project BRDF samples the authored powers and full mixture density",
          "[renderer][rt1][materials][project-brdf]")
{
    namespace format = ludus::content::scene_format;
    const math::Vector3 normalWorld{0, 0, 1};
    const math::Vector3 viewWorld{0.6F, 0, 0.8F};
    rr::ReferenceBsdf material;
    material.Scattering.Model = format::MaterialModel::ProjectBrdf;
    material.Scattering.Eta = {{0.25F, 0, 0.75F}};
    material.Scattering.K = {{0, 12, 4}};
    material.Scattering.CoatingThickness = 1;
    material.BaseColor = {0.2F, 0.4F, 0.8F};
    material.Roughness = 0.5F;
    for (const float32 selector : {0.125F, 0.625F})
    {
        rr::ReferenceBsdfSample sample;
        REQUIRE(rr::SampleReferenceBsdf(material, normalWorld, viewWorld, {selector, 0.75F, 0.125F}, sample) ==
                rhi::RasterStatus::Ready);
        REQUIRE(sample.Accepted == true);
        const float32 cosine = selector < 0.25F ? 0.5F : math::Sqrt(0.5F);
        const float32 expectedPdf = (0.25F * cosine + 1.5F * cosine * cosine * cosine) / math::kPiF;
        const float32 expectedValue = (0.25F + 1.5F * cosine * cosine * viewWorld.Z * viewWorld.Z) / math::kPiF;
        CHECK(sample.Direction.Z == Catch::Approx(cosine));
        CHECK(sample.MisPdf == Catch::Approx(expectedPdf));
        CHECK(math::Distance(sample.Evaluation.Value, material.BaseColor * expectedValue) < 1e-6F);
        CHECK(math::Distance(sample.Weight, material.BaseColor * (expectedValue * cosine / expectedPdf)) < 1e-6F);
        rr::ReferenceBsdfEvaluation reversed;
        REQUIRE(rr::EvaluateReferenceBsdf(material, normalWorld, sample.Direction, viewWorld, reversed) ==
                rhi::RasterStatus::Ready);
        CHECK(math::Distance(reversed.Value, sample.Evaluation.Value) < 1e-6F);
    }
}

namespace
{
namespace program_format = ludus::content::scene_format;
using program_format::MaterialLobeOperation;
using program_format::MaterialProgram;
using program_format::MaterialProgramInput;
using program_format::MaterialProgramOpcode;

/// Append one instruction writing register destination.
void AddInstruction(MaterialProgram& inoutProgram,
                    MaterialProgramOpcode opcode,
                    uint8 destination,
                    uint8 operandA,
                    uint8 operandB = 0,
                    uint8 operandC = 0)
{
    inoutProgram.Instructions[inoutProgram.InstructionCount] =
    {
        .Opcode = opcode,
        .Destination = destination,
        .OperandA = operandA,
        .OperandB = operandB,
        .OperandC = operandC,
    };
    ++inoutProgram.InstructionCount;
}
/// Load an RGB constant into register destination.
void AddConstant(MaterialProgram& inoutProgram, uint8 destination, math::Vector3 value)
{
    inoutProgram.Constants[inoutProgram.ConstantCount] = {{value.X, value.Y, value.Z, 0}};
    AddInstruction(inoutProgram,
                   MaterialProgramOpcode::Constant,
                   destination,
                   static_cast<uint8>(inoutProgram.ConstantCount));
    ++inoutProgram.ConstantCount;
}
void AddLobe(MaterialProgram& inoutProgram, program_format::MaterialProgramLobe lobe)
{
    inoutProgram.Lobes[inoutProgram.LobeCount] = lobe;
    ++inoutProgram.LobeCount;
}
/// The built-in Mix as a program: diffuse weighted by (1 - metallic) * base colour, and a conductor
/// weighted by metallic with roughness 0.5, so alpha 0.25.
MaterialProgram MakeMixProgram(math::Vector3 eta, math::Vector3 k)
{
    MaterialProgram program;
    AddInstruction(program, MaterialProgramOpcode::SurfaceInput, 0, static_cast<uint8>(MaterialProgramInput::Metallic));
    AddInstruction(program, MaterialProgramOpcode::OneMinus, 1, 0);
    AddInstruction(program,
                   MaterialProgramOpcode::SurfaceInput,
                   2,
                   static_cast<uint8>(MaterialProgramInput::BaseColor));
    AddInstruction(program, MaterialProgramOpcode::Multiply, 3, 1, 2);
    AddConstant(program, 4, {0.5F, 0.5F, 0.5F});
    AddLobe(program,
            {
                .Operation = MaterialLobeOperation::Diffuse,
                .WeightRegister = 3,
                .RoughnessRegister = program_format::NO_MATERIAL_PROGRAM_REGISTER,
            });
    AddLobe(program,
            {
                .Operation = MaterialLobeOperation::Conductor,
                .WeightRegister = 0,
                .RoughnessRegister = 4,
                .Eta = {{eta.X, eta.Y, eta.Z}},
                .K = {{k.X, k.Y, k.Z}},
            });
    return program;
}
/// Directions over the upper hemisphere, including grazing ones.
math::Vector3 MakeHemisphereDirection(float32 cosine, float32 azimuth)
{
    const float32 sine = math::Sqrt(1 - cosine * cosine);
    return {sine * math::Cos(azimuth), sine * math::Sin(azimuth), cosine};
}
} // namespace

TEST_CASE("Closure programs reproduce the built-in Mix and project BRDF", "[renderer][rt1][materials][program]")
{
    const math::Vector3 normalWorld{0, 0, 1};
    const math::Vector3 eta{0.7F, 0.8F, 0.9F};
    const math::Vector3 k{3, 4, 5};
    const MaterialProgram mixProgram = MakeMixProgram(eta, k);
    REQUIRE(program_format::IsMaterialProgramValid(mixProgram) == true);
    rr::ReferenceBsdf mixed;
    mixed.Scattering.Model = program_format::MaterialModel::DiffuseConductorMix;
    mixed.Scattering.AlphaU = 0.25F;
    mixed.Scattering.AlphaV = 0.25F;
    mixed.Scattering.Eta = {{eta.X, eta.Y, eta.Z}};
    mixed.Scattering.K = {{k.X, k.Y, k.Z}};
    mixed.Metallic = 0.4F;
    rr::ReferenceBsdf lowered = mixed;
    lowered.Scattering = {};
    lowered.Scattering.Model = program_format::MaterialModel::Program;
    lowered.ProgramOrNull = &mixProgram;
    // Selection uses the mean weight, which equals the Mix's conductor probability for a white base,
    // so value and density both match; a tinted base keeps the value but samples differently.
    for (const math::Vector3 baseColor : {math::Vector3{1, 1, 1}, math::Vector3{0.2F, 0.4F, 0.8F}})
    {
        mixed.BaseColor = baseColor;
        lowered.BaseColor = baseColor;
        const bool isWhite = baseColor.X == 1;
        for (const float32 viewCosine : {0.95F, 0.4F, 0.05F})
        {
            const math::Vector3 viewWorld = MakeHemisphereDirection(viewCosine, 0.3F);
            for (const float32 lightCosine : {0.9F, 0.5F, 0.1F})
            {
                for (const float32 azimuth : {0.0F, 1.5F, 3.0F})
                {
                    const math::Vector3 lightWorld = MakeHemisphereDirection(lightCosine, azimuth);
                    rr::ReferenceBsdfEvaluation expected;
                    rr::ReferenceBsdfEvaluation actual;
                    REQUIRE(rr::EvaluateReferenceBsdf(mixed, normalWorld, viewWorld, lightWorld, expected) ==
                            rhi::RasterStatus::Ready);
                    REQUIRE(rr::EvaluateReferenceBsdf(lowered, normalWorld, viewWorld, lightWorld, actual) ==
                            rhi::RasterStatus::Ready);
                    CHECK(actual.Value.X == Catch::Approx(expected.Value.X).epsilon(1e-5));
                    CHECK(actual.Value.Y == Catch::Approx(expected.Value.Y).epsilon(1e-5));
                    CHECK(actual.Value.Z == Catch::Approx(expected.Value.Z).epsilon(1e-5));
                    if (isWhite == true)
                    {
                        CHECK(actual.Pdf == Catch::Approx(expected.Pdf).epsilon(1e-5));
                    }
                }
            }
        }
    }

    // A two-lobe project BRDF: weights 0.25 and 0.75 of the base colour, powers 12 and 4.
    MaterialProgram brdfProgram;
    AddInstruction(brdfProgram,
                   MaterialProgramOpcode::SurfaceInput,
                   0,
                   static_cast<uint8>(MaterialProgramInput::BaseColor));
    AddConstant(brdfProgram, 1, {0.25F, 0.25F, 0.25F});
    AddConstant(brdfProgram, 2, {0.75F, 0.75F, 0.75F});
    AddInstruction(brdfProgram, MaterialProgramOpcode::Multiply, 3, 0, 1);
    AddInstruction(brdfProgram, MaterialProgramOpcode::Multiply, 4, 0, 2);
    AddInstruction(brdfProgram,
                   MaterialProgramOpcode::SurfaceInput,
                   5,
                   static_cast<uint8>(MaterialProgramInput::Roughness));
    AddLobe(brdfProgram,
            {
                .Operation = MaterialLobeOperation::CosinePower,
                .WeightRegister = 3,
                .RoughnessRegister = 5,
                .Power = 12,
            });
    AddLobe(brdfProgram,
            {
                .Operation = MaterialLobeOperation::CosinePower,
                .WeightRegister = 4,
                .RoughnessRegister = 5,
                .Power = 4,
            });
    REQUIRE(program_format::IsMaterialProgramValid(brdfProgram) == true);
    rr::ReferenceBsdf brdf;
    brdf.Scattering.Model = program_format::MaterialModel::ProjectBrdf;
    brdf.Scattering.Eta = {{0.25F, 0.75F, 0}};
    brdf.Scattering.K = {{12, 4, 0}};
    brdf.Scattering.CoatingThickness = 1;
    brdf.BaseColor = {0.2F, 0.4F, 0.8F};
    brdf.Roughness = 0.3F;
    rr::ReferenceBsdf brdfLowered = brdf;
    brdfLowered.Scattering = {};
    brdfLowered.Scattering.Model = program_format::MaterialModel::Program;
    brdfLowered.ProgramOrNull = &brdfProgram;
    for (const float32 viewCosine : {0.95F, 0.4F})
    {
        const math::Vector3 viewWorld = MakeHemisphereDirection(viewCosine, 0.3F);
        for (const float32 lightCosine : {0.9F, 0.5F, 0.1F})
        {
            const math::Vector3 lightWorld = MakeHemisphereDirection(lightCosine, 2.0F);
            rr::ReferenceBsdfEvaluation expected;
            rr::ReferenceBsdfEvaluation actual;
            REQUIRE(rr::EvaluateReferenceBsdf(brdf, normalWorld, viewWorld, lightWorld, expected) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(rr::EvaluateReferenceBsdf(brdfLowered, normalWorld, viewWorld, lightWorld, actual) ==
                    rhi::RasterStatus::Ready);
            CHECK(math::Distance(actual.Value, expected.Value) < 1e-5F * (1 + math::Length(expected.Value)));
            CHECK(actual.Pdf == Catch::Approx(expected.Pdf).epsilon(1e-5));
        }
    }
}

TEST_CASE("Closure-program sampling matches its density, energy and reciprocity", "[renderer][rt1][materials][program]")
{
    // Diffuse, conductor and cosine-power lobes whose weights come from the surface inputs.
    MaterialProgram program = MakeMixProgram({0.2F, 0.9F, 1.1F}, {3.9F, 2.4F, 2.2F});
    AddInstruction(program, MaterialProgramOpcode::Clamp, 5, 1);
    AddConstant(program, 6, {0.3F, 0.2F, 0.1F});
    AddInstruction(program, MaterialProgramOpcode::Lerp, 7, 6, 5, 0);
    AddLobe(program,
            {
                .Operation = MaterialLobeOperation::CosinePower,
                .WeightRegister = 7,
                .RoughnessRegister = 4,
                .Power = 16,
            });
    REQUIRE(program_format::IsMaterialProgramValid(program) == true);
    rr::ReferenceBsdf material;
    material.Scattering.Model = program_format::MaterialModel::Program;
    material.ProgramOrNull = &program;
    material.BaseColor = {0.6F, 0.3F, 0.1F};
    material.Metallic = 0.35F;
    material.Roughness = 0.4F;
    constexpr usize SIDE = 96;
    const math::Vector3 normalWorld{0, 0, 1};
    for (const float32 viewCosine : {1.0F, 0.3F})
    {
        const math::Vector3 viewWorld = MakeHemisphereDirection(viewCosine, 0);
        float64 densityIntegral = 0;
        float64 energyIntegral = 0;
        float64 sampledEnergy = 0;
        usize accepted = 0;
        for (usize y = 0; y < SIDE; ++y)
        {
            const float32 cosine = (static_cast<float32>(y) + 0.5F) / static_cast<float32>(SIDE);
            for (usize x = 0; x < SIDE; ++x)
            {
                const float32 azimuthSample = (static_cast<float32>(x) + 0.5F) / static_cast<float32>(SIDE);
                const math::Vector3 lightWorld = MakeHemisphereDirection(cosine, math::kTauF * azimuthSample);
                rr::ReferenceBsdfEvaluation evaluated;
                REQUIRE(rr::EvaluateReferenceBsdf(material, normalWorld, viewWorld, lightWorld, evaluated) ==
                        rhi::RasterStatus::Ready);
                densityIntegral += static_cast<float64>(evaluated.Pdf) * math::kTau;
                energyIntegral += static_cast<float64>(evaluated.Value.X * cosine) * math::kTau;
                rr::ReferenceBsdfEvaluation reversed;
                REQUIRE(rr::EvaluateReferenceBsdf(material, normalWorld, lightWorld, viewWorld, reversed) ==
                        rhi::RasterStatus::Ready);
                CHECK(math::Distance(reversed.Value, evaluated.Value) <= 1e-5F * (1 + math::Length(evaluated.Value)));
                // A lattice offset decorrelates the lobe selector from both direction values.
                constexpr usize SELECTOR_X_STRIDE = 37;
                constexpr usize SELECTOR_Y_STRIDE = 11;
                const float32 selector =
                    (static_cast<float32>((x * SELECTOR_X_STRIDE + y * SELECTOR_Y_STRIDE) % SIDE) + 0.5F) /
                    static_cast<float32>(SIDE);
                rr::ReferenceBsdfSample sampled;
                REQUIRE(rr::SampleReferenceBsdf(material,
                                                normalWorld,
                                                viewWorld,
                                                {selector, cosine, azimuthSample},
                                                sampled) == rhi::RasterStatus::Ready);
                if (sampled.Accepted == true)
                {
                    ++accepted;
                    rr::ReferenceBsdfEvaluation matched;
                    REQUIRE(rr::EvaluateReferenceBsdf(material, normalWorld, viewWorld, sampled.Direction, matched) ==
                            rhi::RasterStatus::Ready);
                    CHECK(matched.Pdf == sampled.Evaluation.Pdf);
                    CHECK(sampled.MisPdf == matched.Pdf);
                    sampledEnergy += static_cast<float64>(sampled.Weight.X);
                }
            }
        }
        const float64 sampleCount = static_cast<float64>(SIDE * SIDE);
        // The density integrates to the accepted fraction; the conductor's null proposals keep their mass.
        CHECK(math::Abs(densityIntegral / sampleCount - static_cast<float64>(accepted) / sampleCount) < 0.01);
        CHECK(math::Abs(energyIntegral / sampleCount - sampledEnergy / sampleCount) < 0.02);
    }
}

TEST_CASE("A grain mask weights two distinct lobes by itself and its complement", "[renderer][rt1][materials][program]")
{
    namespace fixtures = ludus::renderer_tests;
    const MaterialProgram program = fixtures::MakeGrainMixProgram();
    REQUIRE(program_format::IsMaterialProgramValid(program) == true);
    rr::ReferenceBsdf grain;
    grain.Scattering.Model = program_format::MaterialModel::Program;
    grain.ProgramOrNull = &program;
    grain.Roughness = fixtures::GRAIN_MIX_ROUGHNESS;
    CHECK(program_format::SupportsFixedDimensionReflection(grain.Scattering, program) == true);
    // The two lobes evaluated independently as typed models: a white Lambertian and the glossy gold conductor.
    rr::ReferenceBsdf diffuse;
    diffuse.Scattering.Model = program_format::MaterialModel::Diffuse;
    rr::ReferenceBsdf conductor;
    conductor.Scattering.Model = program_format::MaterialModel::Conductor;
    conductor.Scattering.AlphaU = fixtures::GRAIN_MIX_ROUGHNESS * fixtures::GRAIN_MIX_ROUGHNESS;
    conductor.Scattering.AlphaV = conductor.Scattering.AlphaU;
    conductor.Scattering.Eta = fixtures::GOLD_ETA;
    conductor.Scattering.K = fixtures::GOLD_K;
    const math::Vector3 normalWorld{0, 0, 1};
    const math::Vector3 viewWorld = MakeHemisphereDirection(0.8F, 0);
    // The mirror direction, where the gold peaks, and two off-specular directions.
    constexpr usize LIGHT_COUNT = 3;
    const StaticArray<math::Vector3, LIGHT_COUNT> lightsWorld{{
        {-viewWorld.X, -viewWorld.Y, viewWorld.Z},
        MakeHemisphereDirection(0.5F, 2.0F),
        MakeHemisphereDirection(0.95F, 1.0F),
    }};
    constexpr float32 INTERIOR_MASK = 0.35F;
    constexpr float32 RELATIVE_TOLERANCE = 1e-5F;
    for (const float32 mask : {0.0F, 1.0F, INTERIOR_MASK})
    {
        CAPTURE(mask);
        grain.BaseColor = {mask, mask, mask};
        for (const math::Vector3& lightWorld : lightsWorld)
        {
            CAPTURE(lightWorld.X, lightWorld.Y, lightWorld.Z);
            rr::ReferenceBsdfEvaluation mixed;
            rr::ReferenceBsdfEvaluation matte;
            rr::ReferenceBsdfEvaluation gold;
            REQUIRE(rr::EvaluateReferenceBsdf(grain, normalWorld, viewWorld, lightWorld, mixed) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(rr::EvaluateReferenceBsdf(diffuse, normalWorld, viewWorld, lightWorld, matte) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(rr::EvaluateReferenceBsdf(conductor, normalWorld, viewWorld, lightWorld, gold) ==
                    rhi::RasterStatus::Ready);
            // The selection weights are the weights' means, 1 - m and m, so they already sum to one.
            const math::Vector3 value = matte.Value * (1 - mask) + gold.Value * mask;
            const float32 density = matte.Pdf * (1 - mask) + gold.Pdf * mask;
            CHECK(math::Distance(mixed.Value, value) <= RELATIVE_TOLERANCE * (1 + math::Length(value)));
            CHECK(mixed.Pdf == Catch::Approx(density).epsilon(RELATIVE_TOLERANCE));
        }
        // Continuation proposes from the full mixture: each sample's density and weight are the mixture's at the
        // sampled direction, and the sampled albedo is the weighted sum of the lobes' sampled albedos.
        constexpr usize SIDE = 64;
        float64 mixedAlbedo = 0;
        float64 matteAlbedo = 0;
        float64 goldAlbedo = 0;
        for (usize y = 0; y < SIDE; ++y)
        {
            for (usize x = 0; x < SIDE; ++x)
            {
                const float32 first = (static_cast<float32>(x) + 0.5F) / static_cast<float32>(SIDE);
                const float32 second = (static_cast<float32>(y) + 0.5F) / static_cast<float32>(SIDE);
                // A lattice offset decorrelates the lobe selector from both direction values.
                constexpr usize SELECTOR_X_STRIDE = 37;
                constexpr usize SELECTOR_Y_STRIDE = 11;
                const float32 selector =
                    (static_cast<float32>((x * SELECTOR_X_STRIDE + y * SELECTOR_Y_STRIDE) % SIDE) + 0.5F) /
                    static_cast<float32>(SIDE);
                rr::ReferenceBsdfSample sampled;
                REQUIRE(rr::SampleReferenceBsdf(grain, normalWorld, viewWorld, {selector, first, second}, sampled) ==
                        rhi::RasterStatus::Ready);
                if (sampled.Accepted == true)
                {
                    rr::ReferenceBsdfEvaluation matte;
                    rr::ReferenceBsdfEvaluation gold;
                    REQUIRE(rr::EvaluateReferenceBsdf(diffuse, normalWorld, viewWorld, sampled.Direction, matte) ==
                            rhi::RasterStatus::Ready);
                    REQUIRE(rr::EvaluateReferenceBsdf(conductor, normalWorld, viewWorld, sampled.Direction, gold) ==
                            rhi::RasterStatus::Ready);
                    const float32 density = matte.Pdf * (1 - mask) + gold.Pdf * mask;
                    const math::Vector3 value = matte.Value * (1 - mask) + gold.Value * mask;
                    CHECK(sampled.MisPdf == Catch::Approx(density).epsilon(RELATIVE_TOLERANCE));
                    CHECK(math::Distance(sampled.Weight, value * (sampled.Direction.Z / density)) <=
                          RELATIVE_TOLERANCE * (1 + math::Length(sampled.Weight)));
                    mixedAlbedo += static_cast<float64>(sampled.Weight.X);
                }
                for (const bool isGold : {false, true})
                {
                    rr::ReferenceBsdfSample lobe;
                    REQUIRE(rr::SampleReferenceBsdf(isGold == true ? conductor : diffuse,
                                                    normalWorld,
                                                    viewWorld,
                                                    {selector, first, second},
                                                    lobe) == rhi::RasterStatus::Ready);
                    (isGold == true ? goldAlbedo : matteAlbedo) += static_cast<float64>(lobe.Weight.X);
                }
            }
        }
        const float64 sampleCount = static_cast<float64>(SIDE * SIDE);
        const float64 weight = static_cast<float64>(mask);
        constexpr float64 ALBEDO_TOLERANCE = 0.01;
        CHECK(math::Abs(mixedAlbedo / sampleCount - ((1 - weight) * matteAlbedo + weight * goldAlbedo) / sampleCount) <
              ALBEDO_TOLERANCE);
    }
}

TEST_CASE("Closure programs reject a missing program and stay finite", "[renderer][rt1][materials][program]")
{
    const math::Vector3 normalWorld{0, 0, 1};
    const math::Vector3 viewWorld = MakeHemisphereDirection(0.8F, 0);
    const math::Vector3 lightWorld = MakeHemisphereDirection(0.6F, 2);
    rr::ReferenceBsdf material;
    material.Scattering.Model = program_format::MaterialModel::Program;
    rr::ReferenceBsdfEvaluation evaluation;
    CHECK(rr::EvaluateReferenceBsdf(material, normalWorld, viewWorld, lightWorld, evaluation) ==
          rhi::RasterStatus::InvalidDescription);
    MaterialProgram invalid = MakeMixProgram({1, 1, 1}, {1, 1, 1});
    invalid.Lobes[0].WeightRegister = program_format::MAX_MATERIAL_PROGRAM_REGISTERS;
    material.ProgramOrNull = &invalid;
    CHECK(rr::EvaluateReferenceBsdf(material, normalWorld, viewWorld, lightWorld, evaluation) ==
          rhi::RasterStatus::InvalidDescription);

    // Products of large constants clamp at the register limit instead of overflowing.
    MaterialProgram large;
    AddConstant(large, 0, {10000, 10000, 10000});
    AddInstruction(large, MaterialProgramOpcode::Multiply, 1, 0, 0);
    AddInstruction(large, MaterialProgramOpcode::Multiply, 1, 1, 1);
    AddLobe(large,
            {
                .Operation = MaterialLobeOperation::Diffuse,
                .WeightRegister = 1,
                .RoughnessRegister = program_format::NO_MATERIAL_PROGRAM_REGISTER,
            });
    REQUIRE(program_format::IsMaterialProgramValid(large) == true);
    material.ProgramOrNull = &large;
    REQUIRE(rr::EvaluateReferenceBsdf(material, normalWorld, viewWorld, lightWorld, evaluation) ==
            rhi::RasterStatus::Ready);
    CHECK(evaluation.Value.X == Catch::Approx(10000 / math::kPiF));

    // Zero weights make a black surface whose proposals are ordinary null samples.
    MaterialProgram black;
    AddConstant(black, 0, {-1, 0, -2});
    AddLobe(black,
            {
                .Operation = MaterialLobeOperation::Diffuse,
                .WeightRegister = 0,
                .RoughnessRegister = program_format::NO_MATERIAL_PROGRAM_REGISTER,
            });
    material.ProgramOrNull = &black;
    REQUIRE(rr::EvaluateReferenceBsdf(material, normalWorld, viewWorld, lightWorld, evaluation) ==
            rhi::RasterStatus::Ready);
    CHECK(evaluation.Value == math::Vector3{});
    CHECK(evaluation.Pdf == 0);
    rr::ReferenceBsdfSample sample;
    REQUIRE(rr::SampleReferenceBsdf(material, normalWorld, viewWorld, {0.5F, 0.5F, 0.5F}, sample) ==
            rhi::RasterStatus::Ready);
    CHECK(sample.Accepted == false);
}
