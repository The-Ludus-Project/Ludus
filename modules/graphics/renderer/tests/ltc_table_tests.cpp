// Acceptance tests for the generated LTC table (shaders/ltc_table_data.inc): the unshadowed specular shading of a
// polygon that the table predicts (Heitz et al. 2016, Equations 8-10, with the albedo of Peters 2021, section 4.2)
// is compared with a Monte Carlo estimate that samples and evaluates the exact reference BSDF. The fit is an
// approximation, so the comparison is statistical; docs/development/ltc-table.md records the measured accuracy.

#include "../src/internal/ltc_table.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../shaders/ltc_table_data.inc"

namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace internal = ludus::graphics::renderer::internal;

constexpr uint64 RANDOM_SEED = 0x17C7AB1EULL;
constexpr uint32 MONTE_CARLO_SAMPLE_COUNT = 1u << 16;
// One Monte Carlo run covers three F0 values at once: metallic BSDFs use the base color as F0.
const math::Vector3 F0_CHANNELS{0.04F, 0.5F, 1.0F};
constexpr float32 ROUGHNESS_VALUES[] = {0.045F, 0.1F, 0.25F, 0.5F, 1.0F};
constexpr float32 VIEW_DEGREES[] = {0.0F, 30.0F, 60.0F, 80.0F};
// Half sizes of squares facing the shading point at unit distance, centered on the mirror direction.
constexpr float32 LOBE_SQUARE_HALF_SIZES[] = {0.05F, 0.2F, 0.6F, 2.0F};
constexpr float32 OFFSET_DEGREES = 20.0F;
constexpr float32 OFFSET_SQUARE_HALF_SIZE = 0.3F;
// A square this large at unit height covers nearly the whole upper hemisphere.
constexpr float32 HEMISPHERE_HALF_SIZE = 1000.0F;
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

/// The table's prediction: albedo times the LTC integral over the polygon.
math::Vector3 PredictShading(float32 roughness, math::Vector3 view, const math::ProjectedPolygonInput& polygon)
{
    const internal::SpecularLtc ltc = internal::LookupSpecularLtc(roughness, view);
    math::LinearlyTransformedCosine distribution;
    REQUIRE(math::TryMakeLinearlyTransformedCosine(ltc.InverseTransform, distribution) == math::MathStatus::Success);
    math::LinearlyTransformedCosinePolygon prepared;
    REQUIRE(math::TryPrepareLinearlyTransformedCosinePolygon(polygon, distribution, prepared) ==
            math::MathStatus::Success);
    return internal::ComputeSpecularLtcAlbedo(ltc, F0_CHANNELS) *
           math::GetLinearlyTransformedCosinePolygonIntegral(prepared);
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
    for (uint32 row = 0; row < LUDUS_LTC_TABLE_ROUGHNESS_COUNT; ++row)
    {
        const float32 roughness =
            LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS + (1.0F - LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS) * static_cast<float32>(row) /
                                                    static_cast<float32>(LUDUS_LTC_TABLE_ROUGHNESS_COUNT - 1);
        for (uint32 column = 0; column < LUDUS_LTC_TABLE_VIEW_COUNT; ++column)
        {
            const float32 theta =
                math::kHalfPiF * static_cast<float32>(column) / static_cast<float32>(LUDUS_LTC_TABLE_VIEW_COUNT - 1);
            const math::Vector3 view{math::Sin(theta), 0.0F, math::Cos(theta)};
            INFO("row " << row << ", column " << column);
            const internal::SpecularLtc ltc = internal::LookupSpecularLtc(roughness, view);
            math::LinearlyTransformedCosine distribution;
            REQUIRE(math::TryMakeLinearlyTransformedCosine(ltc.InverseTransform, distribution) ==
                    math::MathStatus::Success);
            REQUIRE(ltc.Norm > 0.0F);
            REQUIRE(ltc.Norm <= 1.0F);
            REQUIRE(ltc.FresnelNorm >= 0.0F);
            REQUIRE(ltc.FresnelNorm <= ltc.Norm);
        }
    }
}

TEST_CASE("Lookups are invariant to the view azimuth", "[renderer][ltc]")
{
    // Rotating the view about the normal rotates the distribution with it.
    const float32 theta = 1.0F;
    const math::Vector3 canonicalView{math::Sin(theta), 0.0F, math::Cos(theta)};
    const internal::SpecularLtc canonical = internal::LookupSpecularLtc(0.3F, canonicalView);
    math::LinearlyTransformedCosine canonicalDistribution;
    REQUIRE(math::TryMakeLinearlyTransformedCosine(canonical.InverseTransform, canonicalDistribution) ==
            math::MathStatus::Success);
    for (const float32 azimuth : {0.5F, 2.0F, 4.0F})
    {
        const float32 c = math::Cos(azimuth);
        const float32 s = math::Sin(azimuth);
        const math::Vector3 view{canonicalView.X * c, canonicalView.X * s, canonicalView.Z};
        const internal::SpecularLtc rotated = internal::LookupSpecularLtc(0.3F, view);
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
                const math::Vector3 prediction = PredictShading(roughness, view, squares[i].Polygon);
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
    // Measured on Apple M5 with the checked-in table (n = 335): median 0.32%, p90 7.7%, largest 85% (a 0.004
    // estimate at roughness 1, 80 degrees and F0 = 0.04); at roughness 0.045 median 0.006% and p90 0.05%; whole
    // hemisphere median 0.08% and largest 1.2%. docs/development/ltc-table.md discusses the outliers.
    CAPTURE(errors.GetSize(), minimumRoughnessErrors.GetSize(), hemisphereErrors.GetSize());
    REQUIRE(errors.GetSize() > 300);
    REQUIRE(GetPercentile(errors, 0.5F) <= 1.0e-2F);
    REQUIRE(GetPercentile(errors, 0.9F) <= 1.5e-1F);
    REQUIRE(GetPercentile(minimumRoughnessErrors, 0.5F) <= 5.0e-3F);
    REQUIRE(GetPercentile(minimumRoughnessErrors, 0.9F) <= 1.0e-2F);
    REQUIRE(GetPercentile(hemisphereErrors, 1.0F) <= 3.0e-2F);
}
