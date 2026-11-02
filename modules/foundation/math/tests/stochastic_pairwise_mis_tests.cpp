#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/stochastic_pairwise_mis.hpp>

using namespace ludus::foundation;
namespace math = ludus::foundation::math;

TEST_CASE("Pairwise MIS partitions a common measure with non-unit Jacobians", "[math][pairwise-mis]")
{
    constexpr usize COUNT = 3;
    const StaticArray<float32, COUNT> jacobians{{0.125F, 1, 8}};
    for (const float32 jacobian : jacobians)
    {
        for (const float32 sourceTarget : StaticArray<float32, COUNT>{{0, 1, 7}})
        {
            float32 proxy = 0;
            REQUIRE(math::TryComputePairwiseShiftedTarget(sourceTarget, jacobian, proxy) == math::MathStatus::Success);
            float32 neighbor = 0;
            float32 beta = 0;
            float32 base = 0;
            REQUIRE(math::TryComputePairwiseNeighborMis(3, 3, 2, proxy, 5, neighbor) == math::MathStatus::Success);
            REQUIRE(math::TryComputePairwiseCanonicalTerm(3, 3, 2, proxy, 5, beta) == math::MathStatus::Success);
            REQUIRE(math::TryComputePairwiseCanonicalBase(3, 2, base) == math::MathStatus::Success);
            CHECK(math::Abs(neighbor + beta + base - 1) < 1e-6F);
            const float64 sourceDensity = static_cast<float64>(sourceTarget) / static_cast<float64>(jacobian);
            const float64 expected = (3.0 / 5.0) * (3 * sourceDensity / (3 * sourceDensity + 2 * 5));
            CHECK(math::Abs(static_cast<float64>(neighbor) - expected) < 1e-6);
        }
    }
    // Confidence normalization must avoid infinity for finite, very large counts.
    const float32 largest = std::numeric_limits<float32>::max();
    float32 neighbor = 0;
    REQUIRE(math::TryComputePairwiseNeighborMis(largest, largest, largest, largest, largest, neighbor) ==
            math::MathStatus::Success);
    CHECK(math::Abs(neighbor - .25F) < 1e-6F);
}

TEST_CASE("Pairwise stochastic correction and UCW never add an RIS M division", "[math][pairwise-mis]")
{
    float32 scale = 0;
    REQUIRE(math::TryComputePairwiseConfidenceScale(3, 63, scale) == math::MathStatus::Success);
    CHECK(math::Abs(scale - 3.0F / 63.0F) < 1e-6F);
    REQUIRE(math::TryComputePairwiseConfidenceScale(3, 0, scale) == math::MathStatus::Success);
    CHECK(scale == 0);
    REQUIRE(math::TryComputePairwiseConfidenceScale(8, 1, scale) == math::MathStatus::Success);
    CHECK(scale == 1);
    float32 corrected = 0;
    float32 weight = 0;
    float32 contribution = 0;
    REQUIRE(math::TryComputeStochasticMisWeight(.25F, .125F, 2, corrected) == math::MathStatus::Success);
    CHECK(corrected == 1);
    REQUIRE(math::TryComputePairwiseResamplingWeight(corrected, 4, 3, 2, weight) == math::MathStatus::Success);
    CHECK(weight == 24);
    REQUIRE(math::TryComputePairwiseContributionWeight(weight, 4, contribution) == math::MathStatus::Success);
    CHECK(contribution == 6);
    REQUIRE(math::TryComputePairwiseContributionWeight(0, std::numeric_limits<float32>::quiet_NaN(), contribution) ==
            math::MathStatus::Success);
    CHECK(contribution == 0);
}

TEST_CASE("Checked pairwise APIs preserve outputs on domain errors and overflow", "[math][pairwise-mis]")
{
    float32 value = 17;
    const float32 notANumber = std::numeric_limits<float32>::quiet_NaN();
    const float32 infinity = std::numeric_limits<float32>::infinity();
    constexpr usize INVALID_COUNT = 3;
    for (const float32 invalid : StaticArray<float32, INVALID_COUNT>{{-1, notANumber, infinity}})
    {
        CHECK(math::TryComputePairwiseNeighborMis(invalid, 3, 2, 1, 1, value) != math::MathStatus::Success);
        CHECK(value == 17);
        CHECK(math::TryComputePairwiseCanonicalTerm(1, 3, 2, invalid, 1, value) != math::MathStatus::Success);
        CHECK(value == 17);
        CHECK(math::TryComputePairwiseResamplingWeight(1, 1, invalid, 1, value) != math::MathStatus::Success);
        CHECK(value == 17);
    }
    CHECK(math::TryComputePairwiseNeighborMis(4, 3, 2, 1, 1, value) == math::MathStatus::InvalidArgument);
    CHECK(value == 17);
    CHECK(math::TryComputePairwiseCanonicalBase(0, 0, value) == math::MathStatus::InvalidArgument);
    CHECK(value == 17);
    CHECK(math::TryComputeStochasticMisWeight(1, 0, 1, value) == math::MathStatus::InvalidArgument);
    CHECK(value == 17);
    CHECK(math::TryComputeStochasticMisWeight(1, 1, 0, value) == math::MathStatus::InvalidArgument);
    CHECK(value == 17);
    CHECK(math::TryComputePairwiseShiftedTarget(std::numeric_limits<float32>::max(), .5F, value) ==
          math::MathStatus::OutOfRange);
    CHECK(value == 17);
    CHECK(math::TryComputePairwiseContributionWeight(1, 0, value) == math::MathStatus::InvalidArgument);
    CHECK(value == 17);
}
