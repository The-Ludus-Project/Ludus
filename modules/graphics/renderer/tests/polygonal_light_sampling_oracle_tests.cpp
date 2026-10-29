#include "polygonal_light_sampling_oracle.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/shading_normal.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <catch2/catch_test_macros.hpp>

// Unbiasedness of the polygonal-light-sampling template's estimators, through the CPU oracle that the GPU parity test
// ties to the template's shader: on small single-triangle scenes, each estimator's mean over many samples must agree
// with a converged area quadrature of the same integral within its Monte Carlo error. Thanks to Christoph Peters,
// "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021,
// https://doi.org/10.1145/3450626.3459672: section 4.2 shows that the MIS estimators are unbiased for any BRDF because
// their weights sum to one where a technique samples, and section 4.1 notes that the LTC may be zero where the BRDF is
// not, which is why the LTC estimator is compared with the integral over the LTC's support. Section 6.1 clips the
// shading normal to the view's hemisphere; the quadrature integrates with that clipped normal.
namespace
{
using namespace ludus::foundation;
namespace testing = ludus::graphics::renderer::testing;

// Midpoint quadrature over a barycentric grid of this many subdivisions per edge: 512^2 subtriangles, whose
// discretization error stays far below the Monte Carlo tolerance for the smooth integrands below.
constexpr usize QUADRATURE_SUBDIVISIONS = 512;
constexpr usize SAMPLE_COUNT = usize{1} << 16;
// Standard errors allowed between a mean and the reference, plus a relative floor for the single-precision sampler's
// backward error (Peters 2021, section 3.6) and the quadrature.
constexpr float64 STANDARD_ERROR_BOUND = 4.0;
constexpr float64 RELATIVE_FLOOR = 2.0e-3;
constexpr usize CHANNEL_COUNT = 3;

struct Reference final
{
    /// The integral over the polygon and over its part inside the LTC's support.
    StaticArray<float64, CHANNEL_COUNT> Full{};
    StaticArray<float64, CHANNEL_COUNT> LtcSupport{};
};

float64 GetChannel(math::Vector3 value, usize channel) noexcept
{
    return static_cast<float64>(channel == 0 ? value.X : (channel == 1 ? value.Y : value.Z));
}

math::Vector3d ToVector3d(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

/// Integrate f cos L over the triangle's area: each subtriangle contributes the integrand at its centroid times
/// cos(theta_light) / r^2 times its area.
Reference IntegrateReference(const testing::PolygonalLightOracle& oracle)
{
    Reference reference;
    const testing::PolygonalLightCase& lightCase = oracle.Case;
    const math::Vector3d position0 = ToVector3d(lightCase.PositionsWorld[0]);
    const math::Vector3d edge1 = ToVector3d(lightCase.PositionsWorld[1]) - position0;
    const math::Vector3d edge2 = ToVector3d(lightCase.PositionsWorld[2]) - position0;
    const math::Vector3d areaNormal = math::Cross(edge1, edge2);
    const float64 doubleArea = math::Length(areaNormal);
    const math::Vector3d unitNormal = areaNormal * (1.0 / doubleArea);
    const float64 subdivisions = static_cast<float64>(QUADRATURE_SUBDIVISIONS);
    const float64 subtriangleArea = 0.5 * doubleArea / (subdivisions * subdivisions);
    const math::Vector3d receiver = ToVector3d(lightCase.ReceiverPositionWorld);
    for (usize row = 0; row < QUADRATURE_SUBDIVISIONS; ++row)
    {
        // Row `row` holds 2 (QUADRATURE_SUBDIVISIONS - row) - 1 subtriangles: upright ones, then inverted ones.
        for (usize column = 0; column + row < QUADRATURE_SUBDIVISIONS; ++column)
        {
            const usize orientationCount = column + row + 1 < QUADRATURE_SUBDIVISIONS ? 2 : 1;
            for (usize orientation = 0; orientation < orientationCount; ++orientation)
            {
                const float64 offset = orientation == 0 ? 1.0 / 3.0 : 2.0 / 3.0;
                const float64 u = (static_cast<float64>(column) + offset) / subdivisions;
                const float64 v = (static_cast<float64>(row) + offset) / subdivisions;
                const math::Vector3d point = position0 + edge1 * u + edge2 * v;
                const math::Vector3d offsetWorld = point - receiver;
                const float64 distanceSquared = math::Dot(offsetWorld, offsetWorld);
                const math::Vector3d direction = offsetWorld * (1.0 / math::Sqrt(distanceSquared));
                const math::Vector3 directionWorld{static_cast<float32>(direction.X),
                                                   static_cast<float32>(direction.Y),
                                                   static_cast<float32>(direction.Z)};
                math::Vector3 integrand;
                REQUIRE(testing::TryEvaluatePolygonalLightIntegrand(oracle, directionWorld, integrand) == true);
                const float64 measure = math::Abs(math::Dot(unitNormal, direction)) / distanceSquared * subtriangleArea;
                const bool isInLtcSupport = testing::IsInLtcSupport(oracle, directionWorld);
                for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
                {
                    const float64 value = GetChannel(integrand, channel) * measure;
                    reference.Full[channel] += value;
                    reference.LtcSupport[channel] += isInLtcSupport == true ? value : 0.0;
                }
            }
        }
    }
    return reference;
}

/// Running mean and variance of one estimator, per channel.
struct Statistics final
{
    StaticArray<float64, CHANNEL_COUNT> Sum{};
    StaticArray<float64, CHANNEL_COUNT> SumOfSquares{};

    void Add(math::Vector3 value) noexcept
    {
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            const float64 x = GetChannel(value, channel);
            Sum[channel] += x;
            SumOfSquares[channel] += x * x;
        }
    }
    [[nodiscard]] float64 GetMean(usize channel) const noexcept
    {
        return Sum[channel] / static_cast<float64>(SAMPLE_COUNT);
    }
    [[nodiscard]] float64 GetVariance(usize channel) const noexcept
    {
        const float64 mean = GetMean(channel);
        return math::Max(SumOfSquares[channel] / static_cast<float64>(SAMPLE_COUNT) - mean * mean, 0.0);
    }
    /// Variance summed over channels, the comparison between estimators.
    [[nodiscard]] float64 GetTotalVariance() const noexcept
    {
        return GetVariance(0) + GetVariance(1) + GetVariance(2);
    }
};

/// Sample every estimator SAMPLE_COUNT times and check each mean against the reference.
StaticArray<Statistics, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT>
CheckUnbiased(const testing::PolygonalLightCase& lightCase)
{
    testing::PolygonalLightOracle oracle;
    REQUIRE(testing::TryPreparePolygonalLightOracle(lightCase, oracle) == true);
    REQUIRE(oracle.DiffuseIntegral > 0.0F);
    const Reference reference = IntegrateReference(oracle);
    StaticArray<Statistics, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> statistics{};
    math::RandomStream random;
    for (usize i = 0; i < SAMPLE_COUNT; ++i)
    {
        const math::Vector2 projectedSolidAngleSample{random.NextFloat01(), random.NextFloat01()};
        const math::Vector2 ltcSample{random.NextFloat01(), random.NextFloat01()};
        StaticArray<math::Vector3, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> radiance;
        REQUIRE(testing::TryEstimatePolygonalLight(oracle, projectedSolidAngleSample, ltcSample, radiance) == true);
        for (usize estimator = 0; estimator < testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
        {
            statistics[estimator].Add(radiance[estimator]);
        }
    }
    for (usize estimator = 0; estimator < testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
    {
        const bool isLtc = estimator == static_cast<usize>(testing::PolygonalLightEstimator::Ltc);
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            const float64 expected = isLtc == true ? reference.LtcSupport[channel] : reference.Full[channel];
            const float64 mean = statistics[estimator].GetMean(channel);
            const float64 standardError =
                math::Sqrt(statistics[estimator].GetVariance(channel) / static_cast<float64>(SAMPLE_COUNT));
            INFO("estimator " << estimator << ", channel " << channel << ": mean=" << mean << ", reference=" << expected
                              << ", standard error=" << standardError);
            REQUIRE(expected > 0.0);
            CHECK(math::Abs(mean - expected) <= STANDARD_ERROR_BOUND * standardError + RELATIVE_FLOOR * expected);
        }
    }
    return statistics;
}

/// A triangle 1.2 to 1.4 above a receiver at the origin with a +z normal, facing it, shaded with bsdf.
testing::PolygonalLightCase MakeCase(const ludus::graphics::renderer::ReferenceBsdf& bsdf)
{
    testing::PolygonalLightCase lightCase;
    lightCase.NormalWorld = {0, 0, 1};
    // 30 degrees from the normal.
    lightCase.ViewDirectionWorld = {0.5F, 0.0F, 0.8660254F};
    lightCase.BaseColor = bsdf.BaseColor;
    lightCase.Metallic = bsdf.Metallic;
    lightCase.Roughness = bsdf.Roughness;
    lightCase.PositionsWorld = {{{-0.5F, -0.4F, 1.2F}, {0.0F, 0.7F, 1.4F}, {0.6F, -0.3F, 1.0F}}};
    lightCase.Emission = {4.0F, 3.0F, 2.0F};
    return lightCase;
}
} // namespace

TEST_CASE("Polygonal-light-sampling estimators are unbiased against a converged quadrature",
          "[renderer][polygonal-light-sampling]")
{
    SECTION("rough dielectric")
    {
        const testing::PolygonalLightCase lightCase =
            MakeCase({ .BaseColor = {0.7F, 0.5F, 0.3F}, .Metallic = 0.0F, .Roughness = 0.6F });
        // The triangle faces the receiver, so the one-sided emitter lights it.
        testing::PolygonalLightOracle oracle;
        REQUIRE(testing::TryPreparePolygonalLightOracle(lightCase, oracle) == true);
        REQUIRE(oracle.DiffuseCoefficient.X > 0.0F);
        static_cast<void>(CheckUnbiased(lightCase));
    }
    SECTION("glossy metal with the triangle in the specular highlight")
    {
        testing::PolygonalLightCase lightCase =
            MakeCase({ .BaseColor = {0.9F, 0.6F, 0.4F}, .Metallic = 1.0F, .Roughness = 0.3F });
        // Mirror the view about the normal so the highlight falls inside the triangle.
        lightCase.ViewDirectionWorld = {-0.5F, 0.0F, 0.8660254F};
        lightCase.PositionsWorld = {{{0.3F, -0.4F, 1.2F}, {0.6F, 0.4F, 1.4F}, {1.1F, -0.3F, 1.0F}}};
        const StaticArray<Statistics, testing::POLYGONAL_LIGHT_ESTIMATOR_COUNT> statistics = CheckUnbiased(lightCase);
        const float64 clamped =
            statistics[static_cast<usize>(testing::PolygonalLightEstimator::ClampedOptimalMis)].GetTotalVariance();
        const float64 balance =
            statistics[static_cast<usize>(testing::PolygonalLightEstimator::BalanceHeuristic)].GetTotalVariance();
        const float64 projectedSolidAngle =
            statistics[static_cast<usize>(testing::PolygonalLightEstimator::ProjectedSolidAngle)].GetTotalVariance();
        INFO("variance: clamped optimal MIS=" << clamped << ", balance heuristic=" << balance
                                              << ", projected solid angle=" << projectedSolidAngle);
        // Peters 2021, Fig. 10: on an unoccluded glossy surface the clamped optimal MIS has less variance than the
        // balance heuristic, and projected solid angle sampling alone misses the highlight's shape.
        CHECK(clamped < balance);
        CHECK(clamped < projectedSolidAngle);
    }
    SECTION("a two-sided triangle straddling the horizon, with the weighted balance heuristic")
    {
        testing::PolygonalLightCase lightCase =
            MakeCase({ .BaseColor = {0.4F, 0.6F, 0.8F}, .Metallic = 0.5F, .Roughness = 0.45F });
        lightCase.PositionsWorld = {{{-0.8F, -0.2F, 0.6F}, {0.4F, 0.5F, -0.3F}, {0.3F, -0.6F, 0.5F}}};
        lightCase.IsEmissionTwoSided = true;
        // v = 1: section 4.2's weighted balance heuristic, still unbiased.
        lightCase.Visibility = 1.0F;
        testing::PolygonalLightOracle oracle;
        REQUIRE(testing::TryPreparePolygonalLightOracle(lightCase, oracle) == true);
        // Clipping a triangle with one vertex below the horizon leaves a quadrilateral.
        REQUIRE(oracle.Diffuse.VertexCount == 4);
        static_cast<void>(CheckUnbiased(lightCase));
    }
    SECTION("a normal-mapped receiver whose shading normal faces away from the view (section 6.1)")
    {
        testing::PolygonalLightCase lightCase =
            MakeCase({ .BaseColor = {0.6F, 0.7F, 0.5F}, .Metallic = 0.3F, .Roughness = 0.4F });
        // cos = -0.28 against the view: without the clip the BSDF, and so every estimate, would be zero.
        lightCase.NormalWorld = {-0.8F, 0.0F, 0.6F};
        lightCase.ViewDirectionWorld = {0.8F, 0.0F, 0.6F};
        testing::PolygonalLightOracle oracle;
        REQUIRE(testing::TryPreparePolygonalLightOracle(lightCase, oracle) == true);
        REQUIRE(math::Dot(oracle.NormalWorld, lightCase.ViewDirectionWorld) > 0.0F);
        static_cast<void>(CheckUnbiased(lightCase));
    }
}

TEST_CASE("The polygonal-light-sampling oracle clips shading normals to the view's hemisphere",
          "[renderer][polygonal-light-sampling]")
{
    // Peters 2021, section 6.1, with Ludus's margin: the clipped normal is unit, at least 0.01 / sqrt(1 + 0.01^2)
    // inside the view's hemisphere, unchanged once its cosine reaches 0.01, and the view itself when it faces away.
    constexpr float32 MINIMUM_OUTGOING_COSINE = math::SHADING_NORMAL_MINIMUM_OUTGOING_COSINE;
    constexpr float32 UNIT_TOLERANCE = 1.0e-5F;
    const math::Vector3 view{0.0F, 0.6F, 0.8F};
    const auto clip = [view](math::Vector3 normal) -> math::Vector3 {
        testing::PolygonalLightCase lightCase =
            MakeCase({ .BaseColor = {0.5F, 0.5F, 0.5F}, .Metallic = 0.0F, .Roughness = 0.5F });
        lightCase.NormalWorld = normal;
        lightCase.ViewDirectionWorld = view;
        testing::PolygonalLightOracle oracle;
        REQUIRE(testing::TryPreparePolygonalLightOracle(lightCase, oracle) == true);
        return oracle.NormalWorld;
    };
    const math::Vector3 facing{0.0F, 0.0F, 1.0F};
    const math::Vector3 unchanged = clip(facing);
    CHECK(unchanged.X == facing.X);
    CHECK(unchanged.Y == facing.Y);
    CHECK(unchanged.Z == facing.Z);
    const math::Vector3 away = clip(view * -1.0F);
    CHECK(math::Abs(math::Dot(away, view) - 1.0F) <= UNIT_TOLERANCE);
    const math::Vector3 grazing = clip({0.0F, 0.8F, -0.6F});
    CHECK(math::Abs(math::Length(grazing) - 1.0F) <= UNIT_TOLERANCE);
    CHECK(math::Dot(grazing, view) >=
          MINIMUM_OUTGOING_COSINE / math::Sqrt(1.0F + MINIMUM_OUTGOING_COSINE * MINIMUM_OUTGOING_COSINE) -
              UNIT_TOLERANCE);
    CHECK(math::Dot(grazing, view) <= MINIMUM_OUTGOING_COSINE + UNIT_TOLERANCE);
}
