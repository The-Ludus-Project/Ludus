// Acceptance tests for the embedded LTC table (data/ltc_table.bin, laid out by shaders/ltc_table_layout.h): the
// unshadowed specular shading of a polygon that the table predicts (Heitz et al. 2016, Equations 8-10, with the albedo
// of Peters 2021, section 4.2) is compared with a Monte Carlo estimate that samples and evaluates the exact reference
// BSDF. The fit is an approximation, so the comparison is statistical; docs/development/ltc-table.md records the
// measured accuracy. Whatever the fit, combining LTC sampling with projected solid angle sampling must stay unbiased
// (Peters 2021, section 4.2).

#include "../src/internal/ltc_table.h"
#include "../src/internal/ltc_table_data.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace internal = ludus::graphics::renderer::internal;

constexpr uint64 RANDOM_SEED = 0x17C7AB1EULL;
constexpr uint32 MONTE_CARLO_SAMPLE_COUNT = 1u << 16;
// One Monte Carlo run covers three F0 values at once: metallic BSDFs use the base color as F0. Each channel is
// predicted from its own F0 slice, as for a gray material.
const math::Vector3 F0_CHANNELS{0.04F, 0.5F, 1.0F};
constexpr float32 ROUGHNESS_VALUES[] = {0.045F, 0.1F, 0.25F, 0.5F, 1.0F};
constexpr float32 VIEW_DEGREES[] = {0.0F, 30.0F, 60.0F, 80.0F};
// Half sizes of squares facing the shading point at unit distance, centered on the mirror direction.
constexpr float32 LOBE_SQUARE_HALF_SIZES[] = {0.05F, 0.2F, 0.6F, 2.0F};
constexpr float32 OFFSET_DEGREES = 20.0F;
constexpr float32 OFFSET_SQUARE_HALF_SIZE = 0.3F;
// A square this large at unit height covers nearly the whole upper hemisphere.
constexpr float32 HEMISPHERE_HALF_SIZE = 1000.0F;
// The norms are Monte Carlo moments of the fitter (256 x 256 strata over VNDF and uniform samples), so a lobe whose
// albedo is within noise of one may store a norm slightly above it. Measured largest: 1.00001.
constexpr float32 NORM_ESTIMATE_TOLERANCE = 1.0e-4F;
// Estimates below this are dominated by Monte Carlo noise and excluded from relative statistics.
constexpr float32 MINIMUM_COMPARED_ESTIMATE = 1.0e-3F;

struct Square final
{
    math::ProjectedPolygonInput Polygon{};
    math::Vector3 Normal{};
};

/// A square of the given half size facing the origin, centered at the unit direction center.
Square MakeSquare(math::Vector3 center, float32 halfSize) noexcept
{
    const math::Vector3 helper = math::Abs(center.Z) < 0.9F ? math::Vector3{0, 0, 1} : math::Vector3{1, 0, 0};
    math::Vector3 tangent;
    static_cast<void>(math::TryNormalize(math::Cross(helper, center), tangent));
    const math::Vector3 bitangent = math::Cross(center, tangent);
    Square result;
    result.Polygon.VertexCount = 4;
    result.Polygon.PositionsTangent[0] = center + (tangent + bitangent) * halfSize;
    result.Polygon.PositionsTangent[1] = center + (bitangent - tangent) * halfSize;
    result.Polygon.PositionsTangent[2] = center - (tangent + bitangent) * halfSize;
    result.Polygon.PositionsTangent[3] = center + (tangent - bitangent) * halfSize;
    result.Normal = center;
    return result;
}

/// Whether the ray from the origin along direction crosses the convex polygon.
bool IsInside(const math::ProjectedPolygonInput& polygon, math::Vector3 direction) noexcept
{
    const math::Vector3* positions = polygon.PositionsTangent;
    const float32 orientation = math::Dot(positions[0], math::Cross(positions[1], positions[2])) > 0.0F ? 1.0F : -1.0F;
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        if (orientation * math::Dot(direction, math::Cross(positions[i], positions[(i + 1) % polygon.VertexCount])) <
            0.0F)
        {
            return false;
        }
    }
    return true;
}

/// Unshadowed specular shading of the polygon under unit radiance, from the exact BSDF by Monte Carlo.
math::Vector3 EstimateShading(const renderer::ReferenceBsdf& bsdf,
                              math::Vector3 view,
                              const math::ProjectedPolygonInput& polygon,
                              math::RandomStream& inoutRandom)
{
    const math::Vector3 normal{0, 0, 1};
    math::Vector3 sum{};
    for (uint32 i = 0; i < MONTE_CARLO_SAMPLE_COUNT; ++i)
    {
        const math::Vector3 samples{inoutRandom.NextFloat01(), inoutRandom.NextFloat01(), inoutRandom.NextFloat01()};
        renderer::ReferenceBsdfSample sample;
        REQUIRE(renderer::SampleReferenceBsdf(bsdf, normal, view, samples, sample) ==
                ludus::graphics::rhi::RasterStatus::Ready);
        if (sample.Accepted == true && IsInside(polygon, sample.Direction) == true)
        {
            sum = sum + sample.Evaluation.Value * (sample.Direction.Z / sample.Evaluation.Pdf);
        }
    }
    return sum * (1.0F / static_cast<float32>(MONTE_CARLO_SAMPLE_COUNT));
}

/// The table's prediction for a gray F0: albedo times the LTC integral over the polygon.
float32
PredictShading(float32 roughness, math::Vector3 view, float32 fresnel, const math::ProjectedPolygonInput& polygon)
{
    const math::Vector3 f0{fresnel, fresnel, fresnel};
    const internal::SpecularLtc ltc = internal::LookupSpecularLtc(roughness, view, f0);
    math::LinearlyTransformedCosine distribution;
    REQUIRE(math::TryMakeLinearlyTransformedCosine(ltc.InverseTransform, distribution) == math::MathStatus::Success);
    math::LinearlyTransformedCosinePolygon prepared;
    REQUIRE(math::TryPrepareLinearlyTransformedCosinePolygon(polygon, distribution, prepared) ==
            math::MathStatus::Success);
    return internal::ComputeSpecularLtcAlbedo(ltc, f0).X * math::GetLinearlyTransformedCosinePolygonIntegral(prepared);
}

struct Estimate final
{
    float64 Mean = 0.0;
    float64 Variance = 0.0;
};

[[nodiscard]] math::Vector2 MakeStratified(usize i, usize j, usize grid) noexcept
{
    return math::Vector2{(static_cast<float32>(i) + 0.5F) / static_cast<float32>(grid),
                         (static_cast<float32>(j) + 0.5F) / static_cast<float32>(grid)};
}

/// Estimate an integral with one sampling technique on a stratified grid x grid set of random numbers.
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

float32 GetPercentile(Array<float32>& inoutValues, float32 fraction) noexcept
{
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float32 a, float32 b) noexcept { return a < b; });
    return inoutValues[static_cast<usize>(fraction * static_cast<float32>(inoutValues.GetSize() - 1))];
}
} // namespace

TEST_CASE("The LTC table covers the renderer's roughness range with valid cells", "[renderer][ltc]")
{
    REQUIRE(LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS <= 0.045F);
    for (uint32 slice = 0; slice < LUDUS_LTC_TABLE_FRESNEL_COUNT; ++slice)
    {
        const float32 fresnel = static_cast<float32>(slice) / static_cast<float32>(LUDUS_LTC_TABLE_FRESNEL_COUNT - 1);
        const math::Vector3 f0{fresnel, fresnel, fresnel};
        REQUIRE(internal::GetLtcTableFresnelSlice(f0) == slice);
        for (uint32 row = 0; row < LUDUS_LTC_TABLE_ROUGHNESS_COUNT; ++row)
        {
            const float32 roughness = LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS +
                                      (1.0F - LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS) * static_cast<float32>(row) /
                                          static_cast<float32>(LUDUS_LTC_TABLE_ROUGHNESS_COUNT - 1);
            for (uint32 column = 0; column < LUDUS_LTC_TABLE_VIEW_COUNT; ++column)
            {
                const float32 theta = math::kHalfPiF * static_cast<float32>(column) /
                                      static_cast<float32>(LUDUS_LTC_TABLE_VIEW_COUNT - 1);
                const math::Vector3 view{math::Sin(theta), 0.0F, math::Cos(theta)};
                INFO("F0 slice " << slice << ", row " << row << ", column " << column);
                const internal::SpecularLtc ltc = internal::LookupSpecularLtc(roughness, view, f0);
                math::LinearlyTransformedCosine distribution;
                REQUIRE(math::TryMakeLinearlyTransformedCosine(ltc.InverseTransform, distribution) ==
                        math::MathStatus::Success);
                REQUIRE(ltc.Norm > 0.0F);
                REQUIRE(ltc.Norm <= 1.0F + NORM_ESTIMATE_TOLERANCE);
                REQUIRE(ltc.FresnelNorm >= 0.0F);
                REQUIRE(ltc.FresnelNorm <= ltc.Norm);
            }
        }
    }
    // Every embedded value is finite, and the unused moment lanes are zero.
    for (usize i = 0; i < internal::LTC_TABLE_ENTRY_COUNT; ++i)
    {
        const internal::LtcTableEntry& entry = internal::LTC_TABLE[i];
        REQUIRE(math::IsFinite(math::Vector4{entry.X, entry.Y, entry.Z, entry.W}) == true);
        if (i >= LUDUS_LTC_TABLE_MOMENT_OFFSET)
        {
            REQUIRE(entry.Z == 0.0F);
            REQUIRE(entry.W == 0.0F);
        }
    }
}

TEST_CASE("The F0 slice follows the luminance of F0", "[renderer][ltc]")
{
    REQUIRE(internal::GetLtcTableFresnelSlice(math::Vector3{-1.0F, -1.0F, -1.0F}) == 0);
    REQUIRE(internal::GetLtcTableFresnelSlice(math::Vector3{2.0F, 2.0F, 2.0F}) == LUDUS_LTC_TABLE_FRESNEL_COUNT - 1);
    // Rec.709 luminance: green dominates.
    REQUIRE(internal::GetLtcTableFresnelSlice(math::Vector3{0.0F, 1.0F, 0.0F}) >
            internal::GetLtcTableFresnelSlice(math::Vector3{1.0F, 0.0F, 1.0F}));
    // The albedo stays exact per channel whatever slice shapes the lobe.
    const math::Vector3 view{0.6F, 0.0F, 0.8F};
    const internal::SpecularLtc colored = internal::LookupSpecularLtc(0.4F, view, math::Vector3{0.9F, 0.6F, 0.2F});
    const math::Vector3 albedo = internal::ComputeSpecularLtcAlbedo(colored, math::Vector3{0.9F, 0.6F, 0.2F});
    REQUIRE(albedo.X == 0.9F * (colored.Norm - colored.FresnelNorm) + colored.FresnelNorm);
    REQUIRE(albedo.Z == 0.2F * (colored.Norm - colored.FresnelNorm) + colored.FresnelNorm);
}

TEST_CASE("Lookups are invariant to the view azimuth", "[renderer][ltc]")
{
    // Rotating the view about the normal rotates the distribution with it.
    const float32 theta = 1.0F;
    const math::Vector3 canonicalView{math::Sin(theta), 0.0F, math::Cos(theta)};
    const math::Vector3 f0{0.04F, 0.04F, 0.04F};
    const internal::SpecularLtc canonical = internal::LookupSpecularLtc(0.3F, canonicalView, f0);
    math::LinearlyTransformedCosine canonicalDistribution;
    REQUIRE(math::TryMakeLinearlyTransformedCosine(canonical.InverseTransform, canonicalDistribution) ==
            math::MathStatus::Success);
    for (const float32 azimuth : {0.5F, 2.0F, 4.0F})
    {
        const float32 c = math::Cos(azimuth);
        const float32 s = math::Sin(azimuth);
        const math::Vector3 view{canonicalView.X * c, canonicalView.X * s, canonicalView.Z};
        const internal::SpecularLtc rotated = internal::LookupSpecularLtc(0.3F, view, f0);
        math::LinearlyTransformedCosine rotatedDistribution;
        REQUIRE(math::TryMakeLinearlyTransformedCosine(rotated.InverseTransform, rotatedDistribution) ==
                math::MathStatus::Success);
        REQUIRE(rotated.Norm == canonical.Norm);
        const math::Vector3 direction{-0.5F, 0.2F, 0.84F};
        const math::Vector3 rotatedDirection{direction.X * c - direction.Y * s,
                                             direction.X * s + direction.Y * c,
                                             direction.Z};
        const float32 expected = math::EvaluateLinearlyTransformedCosine(canonicalDistribution, direction);
        REQUIRE(math::Abs(math::EvaluateLinearlyTransformedCosine(rotatedDistribution, rotatedDirection) - expected) <=
                1.0e-4F * expected);
    }
}

TEST_CASE("LTC shading matches Monte Carlo integration of the exact specular BSDF", "[renderer][ltc]")
{
    math::RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 1) == true);
    Array<float32> errors;
    Array<float32> minimumRoughnessErrors;
    Array<float32> hemisphereErrors;
    for (const float32 roughness : ROUGHNESS_VALUES)
    {
        renderer::ReferenceBsdf bsdf;
        bsdf.BaseColor = F0_CHANNELS;
        bsdf.Metallic = 1.0F;
        bsdf.Roughness = roughness;
        for (const float32 degrees : VIEW_DEGREES)
        {
            const float32 theta = math::DegreesToRadians(degrees);
            const math::Vector3 view{math::Sin(theta), 0.0F, math::Cos(theta)};
            const math::Vector3 mirror{-view.X, 0.0F, view.Z};
            const float32 offsetTheta = theta - math::DegreesToRadians(OFFSET_DEGREES);
            const math::Vector3 offset{-math::Sin(offsetTheta), 0.0F, math::Cos(offsetTheta)};
            Square squares[sizeof(LOBE_SQUARE_HALF_SIZES) / sizeof(LOBE_SQUARE_HALF_SIZES[0]) + 2];
            uint32 squareCount = 0;
            for (const float32 halfSize : LOBE_SQUARE_HALF_SIZES)
            {
                squares[squareCount] = MakeSquare(mirror, halfSize);
                ++squareCount;
            }
            squares[squareCount] = MakeSquare(offset, OFFSET_SQUARE_HALF_SIZE);
            ++squareCount;
            squares[squareCount] = MakeSquare(math::Vector3{0, 0, 1}, HEMISPHERE_HALF_SIZE);
            ++squareCount;
            for (uint32 i = 0; i < squareCount; ++i)
            {
                const math::Vector3 estimate = EstimateShading(bsdf, view, squares[i].Polygon, random);
                const math::Vector3 prediction{PredictShading(roughness, view, F0_CHANNELS.X, squares[i].Polygon),
                                               PredictShading(roughness, view, F0_CHANNELS.Y, squares[i].Polygon),
                                               PredictShading(roughness, view, F0_CHANNELS.Z, squares[i].Polygon)};
                INFO("roughness " << roughness << ", view " << degrees << " degrees, square " << i << ", estimate "
                                  << estimate.Z << ", prediction " << prediction.Z);
                const float32 channels[][2] = {
                    {estimate.X, prediction.X},
                    {estimate.Y, prediction.Y},
                    {estimate.Z, prediction.Z},
                };
                for (const auto& channel : channels) // ludus-style: inferred-type: dependent array-of-arrays row
                {
                    if (channel[0] < MINIMUM_COMPARED_ESTIMATE)
                    {
                        continue;
                    }
                    const float32 error = math::Abs(channel[1] - channel[0]) / channel[0];
                    REQUIRE(errors.TryAdd(error) == true);
                    if (roughness == ROUGHNESS_VALUES[0])
                    {
                        REQUIRE(minimumRoughnessErrors.TryAdd(error) == true);
                    }
                    if (i + 1 == squareCount)
                    {
                        REQUIRE(hemisphereErrors.TryAdd(error) == true);
                    }
                }
            }
        }
    }
    // Measured on Apple M5 with the checked-in table (n = 335): median 0.37%, p90 5.0%, largest 22% (a 0.007
    // estimate at roughness 1, 80 degrees and F0 = 0.04); at roughness 0.045 median 0.007%, p90 0.05% and largest
    // 16%; whole hemisphere median 0.07% and largest 3.3% (roughness 0.25, 80 degrees, F0 = 0.04, where the stored
    // norm matches quadrature and the fitted shape is responsible). docs/development/ltc-table.md discusses them.
    CAPTURE(errors.GetSize(), minimumRoughnessErrors.GetSize(), hemisphereErrors.GetSize());
    REQUIRE(errors.GetSize() > 300);
    REQUIRE(GetPercentile(errors, 0.5F) <= 1.0e-2F);
    REQUIRE(GetPercentile(errors, 0.9F) <= 1.5e-1F);
    REQUIRE(GetPercentile(minimumRoughnessErrors, 0.5F) <= 5.0e-3F);
    REQUIRE(GetPercentile(minimumRoughnessErrors, 0.9F) <= 1.0e-2F);
    REQUIRE(GetPercentile(hemisphereErrors, 1.0F) <= 4.0e-2F);
}

TEST_CASE("Combining projected solid angle and LTC sampling estimates polygon lighting without bias", "[renderer][ltc]")
{
    // Peters section 4.2: only projected solid angle sampling covers the whole polygon above the horizon, so the LTC
    // technique is combined with it by multiple importance sampling. The reference uses many projected solid angle
    // samples alone, which are unbiased for any BRDF. One sample per technique with the balance heuristic must agree
    // with it, and for glossy lobes must have less variance than projected solid angle sampling alone (Peters Fig. 14).
    constexpr usize REFERENCE_GRID = 256;
    constexpr usize ESTIMATOR_GRID = 64;
    // Four independent-sample standard errors; stratification makes the true error far smaller.
    constexpr float64 STANDARD_ERROR_BOUND = 4.0;
    constexpr float64 RELATIVE_ERROR_FLOOR = 2.0e-3;
    constexpr float32 GLOSSY_ROUGHNESS = 0.35F;
    constexpr float64 GLOSSY_VARIANCE_RATIO = 0.5;
    const math::Vector3 normal{0, 0, 1};
    for (const float32 roughness : {0.15F, 0.35F, 0.7F})
    {
        for (const float32 cosine : {0.9F, 0.5F})
        {
            for (const float32 fresnel : {0.04F, 1.0F})
            {
                const math::Vector3 view{math::Sqrt(1.0F - cosine * cosine), 0.0F, cosine};
                const math::Vector3 f0{fresnel, fresnel, fresnel};
                renderer::ReferenceBsdf bsdf;
                bsdf.BaseColor = f0;
                bsdf.Metallic = 1.0F;
                bsdf.Roughness = roughness;
                // A unit-height quad in the highlight: the view's mirror direction points away from it.
                const float32 offset = -view.X / view.Z;
                math::ProjectedPolygonInput light;
                light.VertexCount = 4;
                light.PositionsTangent[0] = math::Vector3{offset - 0.4F, -0.5F, 1.0F};
                light.PositionsTangent[1] = math::Vector3{offset + 0.6F, -0.5F, 1.0F};
                light.PositionsTangent[2] = math::Vector3{offset + 0.6F, 0.5F, 1.0F};
                light.PositionsTangent[3] = math::Vector3{offset - 0.4F, 0.5F, 1.0F};
                const internal::SpecularLtc ltc = internal::LookupSpecularLtc(roughness, view, f0);
                math::LinearlyTransformedCosine distribution;
                math::ProjectedSolidAnglePolygon cosinePolygon;
                math::LinearlyTransformedCosinePolygon ltcPolygon;
                REQUIRE(math::TryMakeLinearlyTransformedCosine(ltc.InverseTransform, distribution) ==
                        math::MathStatus::Success);
                REQUIRE(math::TryPrepareProjectedSolidAnglePolygon(light, cosinePolygon) == math::MathStatus::Success);
                REQUIRE(math::TryPrepareLinearlyTransformedCosinePolygon(light, distribution, ltcPolygon) ==
                        math::MathStatus::Success);
                REQUIRE(math::GetLinearlyTransformedCosinePolygonIntegral(ltcPolygon) > 0.0F);
                // The specular lobe times the cosine for one channel.
                const auto evaluateLobe = [&](math::Vector3 direction) {
                    renderer::ReferenceBsdfEvaluation evaluation;
                    if (direction.Z <= 0.0F ||
                        renderer::EvaluateReferenceBsdf(bsdf, normal, view, direction, evaluation) !=
                            ludus::graphics::rhi::RasterStatus::Ready)
                    {
                        return 0.0;
                    }
                    return static_cast<float64>(evaluation.Value.X) * static_cast<float64>(direction.Z);
                };
                const auto evaluateDensities = [&](math::Vector3 direction) {
                    return static_cast<float64>(
                               math::EvaluateProjectedSolidAnglePolygonDensity(cosinePolygon, direction)) +
                           static_cast<float64>(
                               math::EvaluateLinearlyTransformedCosinePolygonDensity(ltcPolygon, direction));
                };
                const auto sampleCosine = [&](math::Vector2 randomNumbers) {
                    math::Vector3 direction;
                    REQUIRE(math::TrySampleProjectedSolidAnglePolygon(cosinePolygon, randomNumbers, direction) ==
                            math::MathStatus::Success);
                    return evaluateLobe(direction) /
                           static_cast<float64>(
                               math::EvaluateProjectedSolidAnglePolygonDensity(cosinePolygon, direction));
                };
                const auto sampleBoth = [&](math::Vector2 randomNumbers) {
                    math::Vector3 fromCosine;
                    math::Vector3 fromLtc;
                    REQUIRE(math::TrySampleProjectedSolidAnglePolygon(cosinePolygon, randomNumbers, fromCosine) ==
                            math::MathStatus::Success);
                    REQUIRE(math::TrySampleLinearlyTransformedCosinePolygon(ltcPolygon, randomNumbers, fromLtc) ==
                            math::MathStatus::Success);
                    // Balance heuristic with one sample per technique: f / (p0 + p1) for each sample.
                    const float64 ltcDensities = evaluateDensities(fromLtc);
                    return evaluateLobe(fromCosine) / evaluateDensities(fromCosine) +
                           (ltcDensities > 0.0 ? evaluateLobe(fromLtc) / ltcDensities : 0.0);
                };
                const Estimate reference = EstimatePolygon(REFERENCE_GRID, sampleCosine);
                const Estimate cosineEstimate = EstimatePolygon(ESTIMATOR_GRID, sampleCosine);
                const Estimate combined = EstimatePolygon(ESTIMATOR_GRID, sampleBoth);
                INFO("roughness " << roughness << ", cosine " << cosine << ", F0 " << fresnel << ", reference "
                                  << reference.Mean << ", combined " << combined.Mean << ", variance "
                                  << combined.Variance << " vs projected solid angle " << cosineEstimate.Variance);
                REQUIRE(reference.Mean > 0.0);
                const float64 standardError =
                    math::Sqrt(combined.Variance / static_cast<float64>(ESTIMATOR_GRID * ESTIMATOR_GRID));
                REQUIRE(math::Abs(combined.Mean - reference.Mean) <=
                        STANDARD_ERROR_BOUND * standardError + RELATIVE_ERROR_FLOOR * reference.Mean);
                if (roughness <= GLOSSY_ROUGHNESS)
                {
                    REQUIRE(combined.Variance < GLOSSY_VARIANCE_RATIO * cosineEstimate.Variance);
                }
            }
        }
    }
}
