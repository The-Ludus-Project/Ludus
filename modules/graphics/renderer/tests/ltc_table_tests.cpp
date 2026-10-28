// Acceptance of the fitted GGX LTC table (ltc_table.hpp) against Monte Carlo estimates of the
// exact reference BSDF. The LTC only guides sampling (Peters 2021, sections 4.1-4.2): combined
// with projected solid angle sampling it must be unbiased whatever the fit, and a good fit must
// reduce variance.

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/ltc_table.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;

constexpr math::Vector3 NORMAL{0, 0, 1};
// Stratified samples per dimension for the reference estimates and for each compared estimator.
constexpr usize REFERENCE_GRID = 256;
constexpr usize ESTIMATOR_GRID = 64;
// Largest admitted L1 distance between a fitted LTC and its normalized lobe away from grazing views; see the test.
// The checked-in table measures a median near 0.05 and a worst case of 0.34 on the tested nodes.
constexpr float64 MAXIMUM_LOBE_DISTANCE = 0.4;

[[nodiscard]] math::Vector3 MakeView(float32 cosine) noexcept
{
    return math::Vector3{math::Sqrt(1.0F - cosine * cosine), 0.0F, cosine};
}

[[nodiscard]] renderer::ReferenceBsdf MakeSpecular(float32 roughness, math::Vector3 f0) noexcept
{
    return renderer::ReferenceBsdf{ .BaseColor = f0, .Metallic = 1.0F, .Roughness = roughness };
}

[[nodiscard]] math::Vector2 MakeStratified(usize i, usize j, usize grid) noexcept
{
    return math::Vector2{(static_cast<float32>(i) + 0.5F) / static_cast<float32>(grid),
                         (static_cast<float32>(j) + 0.5F) / static_cast<float32>(grid)};
}

/// Monte Carlo albedo of the specular lobe with the BSDF's own visible-normal sampling.
[[nodiscard]] math::Vector3 EstimateAlbedo(float32 roughness, float32 viewCosine, math::Vector3 f0)
{
    math::Vector3d sum{};
    for (usize i = 0; i < REFERENCE_GRID; ++i)
    {
        for (usize j = 0; j < REFERENCE_GRID; ++j)
        {
            const math::Vector2 random = MakeStratified(i, j, REFERENCE_GRID);
            renderer::ReferenceBsdfSample sample;
            REQUIRE(renderer::SampleReferenceBsdf(MakeSpecular(roughness, f0),
                                                  NORMAL,
                                                  MakeView(viewCosine),
                                                  math::Vector3{0.0F, random.X, random.Y},
                                                  sample) == rhi::RasterStatus::Ready);
            if (sample.Accepted == true)
            {
                const float64 weight =
                    static_cast<float64>(sample.Direction.Z) / static_cast<float64>(sample.Evaluation.Pdf);
                sum = sum + math::Vector3d{static_cast<float64>(sample.Evaluation.Value.X) * weight,
                                           static_cast<float64>(sample.Evaluation.Value.Y) * weight,
                                           static_cast<float64>(sample.Evaluation.Value.Z) * weight};
            }
        }
    }
    const float64 count = static_cast<float64>(REFERENCE_GRID * REFERENCE_GRID);
    return math::Vector3{static_cast<float32>(sum.X / count),
                         static_cast<float32>(sum.Y / count),
                         static_cast<float32>(sum.Z / count)};
}

/// The specular lobe times the cosine for one channel.
[[nodiscard]] float64 EvaluateLobe(const renderer::ReferenceBsdf& bsdf, math::Vector3 view, math::Vector3 direction)
{
    renderer::ReferenceBsdfEvaluation evaluation;
    if (direction.Z <= 0.0F ||
        renderer::EvaluateReferenceBsdf(bsdf, NORMAL, view, direction, evaluation) != rhi::RasterStatus::Ready)
    {
        return 0.0;
    }
    return static_cast<float64>(evaluation.Value.X) * static_cast<float64>(direction.Z);
}

struct Estimate final
{
    float64 Mean = 0.0;
    float64 Variance = 0.0;
};

/// Estimate the integral of the lobe over the polygon with one sampling technique.
template <typename Sampler>
[[nodiscard]] Estimate EstimatePolygon(usize grid, const Sampler& sampler)
{
    float64 sum = 0.0;
    float64 sumSquares = 0.0;
    for (usize i = 0; i < grid; ++i)
    {
        for (usize j = 0; j < grid; ++j)
        {
            const float64 value = sampler(MakeStratified(i, j, grid));
            sum += value;
            sumSquares += value * value;
        }
    }
    const float64 count = static_cast<float64>(grid * grid);
    return Estimate
    {
        .Mean = sum / count,
        .Variance = math::Max(0.0, sumSquares / count - (sum / count) * (sum / count)),
    };
}

/// A quad light above and in front of the shading point, in the LTC tangent frame.
[[nodiscard]] math::ProjectedPolygonInput MakeLight(float32 offsetX) noexcept
{
    math::ProjectedPolygonInput light;
    light.VertexCount = 4;
    light.PositionsTangent[0] = math::Vector3{offsetX - 0.4F, -0.5F, 1.0F};
    light.PositionsTangent[1] = math::Vector3{offsetX + 0.6F, -0.5F, 1.0F};
    light.PositionsTangent[2] = math::Vector3{offsetX + 0.6F, 0.5F, 1.0F};
    light.PositionsTangent[3] = math::Vector3{offsetX - 0.4F, 0.5F, 1.0F};
    return light;
}
} // namespace

TEST_CASE("The embedded LTC table is finite and well formed", "[renderer][ltc]")
{
    const BorrowedView<const renderer::LtcTableEntry> transforms = renderer::GetLtcTableTransforms();
    const BorrowedView<const renderer::LtcAlbedoEntry> albedo = renderer::GetLtcTableAlbedo();
    REQUIRE(transforms.GetSize() == renderer::LTC_TABLE_TRANSFORM_COUNT);
    REQUIRE(albedo.GetSize() == renderer::LTC_TABLE_ALBEDO_COUNT);
    for (usize i = 0; i < transforms.GetSize(); ++i)
    {
        const renderer::LtcTableEntry& entry = transforms[i];
        REQUIRE(math::IsFinite(entry.InverseM00) == true);
        REQUIRE(math::IsFinite(entry.InverseM02) == true);
        REQUIRE(math::IsFinite(entry.InverseM20) == true);
        // det(M^-1) = InverseM11 (InverseM00 - InverseM02 InverseM20) must be positive: orientation preserving.
        REQUIRE(entry.InverseM11 > 0.0F);
        REQUIRE(entry.InverseM00 - entry.InverseM02 * entry.InverseM20 > 0.0F);
    }
    for (usize i = 0; i < albedo.GetSize(); ++i)
    {
        REQUIRE(albedo[i].FresnelScale >= 0.0F);
        REQUIRE(albedo[i].FresnelBias >= 0.0F);
        REQUIRE(albedo[i].FresnelScale + albedo[i].FresnelBias <= 1.0F + 1.0e-3F);
    }
}

TEST_CASE("Fitted LTCs approximate the normalized specular lobe", "[renderer][ltc]")
{
    // Heitz et al. 2016 section 4 fit each LTC to the cosine-weighted BRDF divided by its albedo. The L1 distance
    // between the two densities over the sphere is 0 for a perfect fit and 2 for disjoint ones; a broken fit, such
    // as one collapsed to a narrow spike, scores above 1. Grid nodes avoid interpolation; grazing views are excluded
    // because LTCs fit them least well (Heitz et al. section 4).
    constexpr usize ROWS = 128;
    constexpr usize COLUMNS = 256;
    const usize roughnessIndices[] = {13, 25, 38, 50, 63};
    const usize inclinationIndices[] = {0, 20, 40};
    const float32 fresnels[] = {0.04F, 1.0F};
    float64 worst = 0.0;
    for (const usize roughnessIndex : roughnessIndices)
    {
        for (const usize inclinationIndex : inclinationIndices)
        {
            for (const float32 fresnel : fresnels)
            {
                const float32 roughness = static_cast<float32>(roughnessIndex) /
                                          static_cast<float32>(renderer::LTC_TABLE_ROUGHNESS_COUNT - 1);
                const float32 inclination = static_cast<float32>(inclinationIndex) /
                                            static_cast<float32>(renderer::LTC_TABLE_INCLINATION_COUNT - 1) *
                                            math::kHalfPiF;
                const float32 cosine = math::Cos(inclination);
                const math::Vector3 f0{fresnel, fresnel, fresnel};
                renderer::SpecularLtc ltc;
                REQUIRE(renderer::TryLookupSpecularLtc(roughness, cosine, f0, ltc) == rhi::RasterStatus::Ready);
                const math::Vector3 view = MakeView(cosine);
                const renderer::ReferenceBsdf bsdf = MakeSpecular(roughness, f0);
                float64 distance = 0.0;
                for (usize i = 0; i < ROWS; ++i)
                {
                    const float64 z = -1.0 + 2.0 * (static_cast<float64>(i) + 0.5) / ROWS;
                    const float64 radius = std::sqrt(1.0 - z * z);
                    for (usize j = 0; j < COLUMNS; ++j)
                    {
                        const float64 azimuth = (static_cast<float64>(j) + 0.5) / COLUMNS * math::kTau;
                        const math::Vector3 direction{static_cast<float32>(radius * std::cos(azimuth)),
                                                      static_cast<float32>(radius * std::sin(azimuth)),
                                                      static_cast<float32>(z)};
                        const float64 target = EvaluateLobe(bsdf, view, direction) / static_cast<float64>(ltc.Albedo.X);
                        distance += std::abs(
                            target - static_cast<float64>(math::EvaluateLtcDensity(ltc.InverseTransform, direction)));
                    }
                }
                distance *= 2.0 * math::kTau / (static_cast<float64>(ROWS) * COLUMNS);
                INFO("roughness " << roughness << ", inclination " << inclination << ", F0 " << fresnel);
                REQUIRE(distance <= MAXIMUM_LOBE_DISTANCE);
                worst = math::Max(worst, distance);
            }
        }
    }
    REQUIRE(worst > 0.0);
}

TEST_CASE("The tabulated albedo matches Monte Carlo estimates of the exact BSDF", "[renderer][ltc]")
{
    // With alpha = 1, D = 1 / pi and the height-correlated visibility is 0.5 / (cos + 1) at normal incidence, so the
    // albedo for F0 = 1 is the integral of mu / (1 + mu), 1 - ln 2.
    renderer::SpecularLtc ltc;
    REQUIRE(renderer::TryLookupSpecularLtc(1.0F, 1.0F, math::Vector3{1, 1, 1}, ltc) == rhi::RasterStatus::Ready);
    REQUIRE(std::abs(static_cast<float64>(ltc.Albedo.X) - (1.0 - std::log(2.0))) <= 1.0e-3);

    // Off-grid lookups with a colored F0: bilinear interpolation adds a little error to the exact Schlick split.
    const float32 roughnesses[] = {0.08F, 0.3F, 0.55F, 0.9F};
    const float32 cosines[] = {0.97F, 0.6F, 0.25F};
    const math::Vector3 f0{0.95F, 0.64F, 0.04F};
    for (const float32 roughness : roughnesses)
    {
        for (const float32 cosine : cosines)
        {
            REQUIRE(renderer::TryLookupSpecularLtc(roughness, cosine, f0, ltc) == rhi::RasterStatus::Ready);
            const math::Vector3 expected = EstimateAlbedo(roughness, cosine, f0);
            INFO("roughness " << roughness << ", cosine " << cosine);
            REQUIRE(math::Abs(ltc.Albedo.X - expected.X) <= 1.0e-2F + 2.0e-2F * expected.X);
            REQUIRE(math::Abs(ltc.Albedo.Y - expected.Y) <= 1.0e-2F + 2.0e-2F * expected.Y);
            REQUIRE(math::Abs(ltc.Albedo.Z - expected.Z) <= 1.0e-2F + 2.0e-2F * expected.Z);
        }
    }
}

TEST_CASE("Combining cosine and LTC sampling estimates polygon lighting without bias", "[renderer][ltc]")
{
    // Peters section 4.2: only the projected solid angle technique covers the whole polygon above the horizon, so the
    // LTC technique is combined with it by multiple importance sampling. The reference uses many projected solid
    // angle samples alone, which are unbiased for any BRDF. One sample per technique with the balance heuristic must
    // agree with it, and for glossy lobes must have less variance than two cosine samples (Peters Fig. 14).
    const float32 roughnesses[] = {0.15F, 0.35F, 0.7F};
    const float32 cosines[] = {0.9F, 0.5F};
    const float32 fresnels[] = {0.04F, 1.0F};
    for (const float32 roughness : roughnesses)
    {
        for (const float32 cosine : cosines)
        {
            for (const float32 fresnel : fresnels)
            {
                const math::Vector3 view = MakeView(cosine);
                const math::Vector3 f0{fresnel, fresnel, fresnel};
                const renderer::ReferenceBsdf bsdf = MakeSpecular(roughness, f0);
                // The mirror direction of the view points away from it; put the light in the highlight.
                const math::ProjectedPolygonInput light = MakeLight(-view.X / view.Z);
                renderer::SpecularLtc ltc;
                REQUIRE(renderer::TryLookupSpecularLtc(roughness, cosine, f0, ltc) == rhi::RasterStatus::Ready);
                math::ProjectedSolidAnglePolygon cosinePolygon;
                math::LtcPolygon ltcPolygon;
                REQUIRE(math::TryPrepareProjectedSolidAnglePolygon(light, cosinePolygon) == math::MathStatus::Success);
                REQUIRE(math::TryPrepareLtcPolygon(light, ltc.InverseTransform, ltcPolygon) ==
                        math::MathStatus::Success);
                REQUIRE(math::GetLtcPolygonIntegral(ltcPolygon) > 0.0F);
                const auto evaluateDensities = [&](math::Vector3 direction) {
                    return static_cast<float64>(
                               math::EvaluateProjectedSolidAnglePolygonDensity(cosinePolygon, direction)) +
                           static_cast<float64>(math::EvaluateLtcPolygonDensity(ltcPolygon, direction));
                };
                const auto sampleCosine = [&](math::Vector2 random) {
                    math::Vector3 direction;
                    REQUIRE(math::TrySampleProjectedSolidAnglePolygon(cosinePolygon, random, direction) ==
                            math::MathStatus::Success);
                    return EvaluateLobe(bsdf, view, direction) /
                           static_cast<float64>(
                               math::EvaluateProjectedSolidAnglePolygonDensity(cosinePolygon, direction));
                };
                const auto sampleBoth = [&](math::Vector2 random) {
                    math::Vector3 fromCosine;
                    math::Vector3 fromLtc;
                    REQUIRE(math::TrySampleProjectedSolidAnglePolygon(cosinePolygon, random, fromCosine) ==
                            math::MathStatus::Success);
                    REQUIRE(math::TrySampleLtcPolygon(ltcPolygon, random, fromLtc) == math::MathStatus::Success);
                    // Balance heuristic with one sample per technique: f / (p0 + p1) for each sample.
                    const float64 ltcDensities = evaluateDensities(fromLtc);
                    return EvaluateLobe(bsdf, view, fromCosine) / evaluateDensities(fromCosine) +
                           (ltcDensities > 0.0 ? EvaluateLobe(bsdf, view, fromLtc) / ltcDensities : 0.0);
                };
                const Estimate reference = EstimatePolygon(REFERENCE_GRID, sampleCosine);
                const Estimate cosineEstimate = EstimatePolygon(ESTIMATOR_GRID, sampleCosine);
                const Estimate combined = EstimatePolygon(ESTIMATOR_GRID, sampleBoth);
                INFO("roughness " << roughness << ", cosine " << cosine << ", F0 " << fresnel << ", reference "
                                  << reference.Mean << ", combined " << combined.Mean << ", LTC approximation "
                                  << static_cast<float64>(ltc.Albedo.X) *
                                         static_cast<float64>(math::GetLtcPolygonIntegral(ltcPolygon))
                                  << ", variance " << combined.Variance << " vs cosine " << cosineEstimate.Variance);
                REQUIRE(reference.Mean > 0.0);
                // Stratification makes the true error far smaller than this independent-sample bound.
                const float64 standardError =
                    std::sqrt(combined.Variance / static_cast<float64>(ESTIMATOR_GRID * ESTIMATOR_GRID));
                REQUIRE(std::abs(combined.Mean - reference.Mean) <= 4.0 * standardError + 2.0e-3 * reference.Mean);
                if (roughness <= 0.35F)
                {
                    REQUIRE(combined.Variance < 0.5 * cosineEstimate.Variance);
                }
            }
        }
    }
}

TEST_CASE("The LTC tangent frame and lookup validate their inputs", "[renderer][ltc]")
{
    math::Matrix3 frame;
    const math::Vector3 normal{0, 1, 0};
    math::Vector3 view;
    REQUIRE(math::TryNormalize(math::Vector3{0.3F, 0.8F, -0.5F}, view) == math::MathStatus::Success);
    REQUIRE(renderer::TryMakeLtcTangentFrame(normal, view, frame) == rhi::RasterStatus::Ready);
    const math::Vector3 normalTangent = frame * normal;
    const math::Vector3 viewTangent = frame * view;
    REQUIRE(math::Length(normalTangent - math::Vector3{0, 0, 1}) <= 1.0e-6F);
    REQUIRE(math::Abs(viewTangent.Y) <= 1.0e-6F);
    REQUIRE(viewTangent.X > 0.0F);
    REQUIRE(math::Abs(viewTangent.Z - math::Dot(normal, view)) <= 1.0e-6F);
    // A view along the normal still yields a rotation.
    REQUIRE(renderer::TryMakeLtcTangentFrame(normal, normal, frame) == rhi::RasterStatus::Ready);
    REQUIRE(math::Length(frame * normal - math::Vector3{0, 0, 1}) <= 1.0e-6F);

    renderer::SpecularLtc ltc;
    ltc.Albedo = math::Vector3{7, 7, 7};
    REQUIRE(renderer::TryLookupSpecularLtc(1.5F, 0.5F, math::Vector3{}, ltc) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(renderer::TryLookupSpecularLtc(0.5F, -0.1F, math::Vector3{}, ltc) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(renderer::TryLookupSpecularLtc(0.5F, 0.5F, math::Vector3{2, 0, 0}, ltc) ==
            rhi::RasterStatus::InvalidDescription);
    REQUIRE(renderer::TryMakeLtcTangentFrame(math::Vector3{0, 2, 0}, view, frame) ==
            rhi::RasterStatus::InvalidDescription);
    REQUIRE(ltc.Albedo == math::Vector3{7, 7, 7});
}
