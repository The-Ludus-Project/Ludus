#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
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
