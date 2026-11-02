#include <ludus/foundation/math/metropolis.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace ludus::foundation;
namespace math = ludus::foundation::math;

TEST_CASE("Symmetric MH acceptance and toroidal mutations preserve their domains", "[math][metropolis]")
{
    float32 acceptance = 0;
    REQUIRE(math::TryComputeSymmetricMetropolisAcceptance(2, 1, acceptance) == math::MathStatus::Success);
    CHECK(acceptance == 0.5F);
    REQUIRE(math::TryComputeSymmetricMetropolisAcceptance(1, 2, acceptance) == math::MathStatus::Success);
    CHECK(acceptance == 1);
    CHECK(math::TryComputeSymmetricMetropolisAcceptance(0, 2, acceptance) == math::MathStatus::InvalidArgument);
    CHECK(acceptance == 1);
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    CHECK(math::TryComputeSymmetricMetropolisAcceptance(nan, 2, acceptance) == math::MathStatus::NonFiniteInput);
    float32 sample = 0;
    REQUIRE(math::TryMutateMetropolisPrimary(0, 0, 0.5F, false, sample) == math::MathStatus::Success);
    CHECK(sample == 0.5F);
    REQUIRE(math::TryMutateMetropolisPrimary(0.75F, 0.75F, 0.5F, false, sample) == math::MathStatus::Success);
    CHECK(sample == 0);
    REQUIRE(math::TryMutateMetropolisPrimary(0.75F, 0.25F, 0.5F, true, sample) == math::MathStatus::Success);
    CHECK(sample == 0.25F);
    CHECK(math::TryMutateMetropolisPrimary(0, 1, 0.5F, false, sample) == math::MathStatus::InvalidArgument);
    CHECK(sample == 0.25F);
}

TEST_CASE("RB holding weights include the accepting proposal and survive dispatch boundaries", "[math][metropolis]")
{
    math::MetropolisTour tour;
    REQUIRE(math::TryAdvanceMetropolisTour(0.5F, tour) == math::MathStatus::Success);
    CHECK(tour.RejectionProduct == 0.5F);
    CHECK(tour.HoldingWeight == 1.5F);
    REQUIRE(math::TryAdvanceMetropolisTour(0.5F, tour) == math::MathStatus::Success);
    CHECK(tour.RejectionProduct == 0.25F);
    CHECK(tour.HoldingWeight == 1.75F);
    REQUIRE(math::TryAdvanceMetropolisTour(1, tour) == math::MathStatus::Success);
    CHECK(tour.RejectionProduct == 0);
    CHECK(tour.HoldingWeight == 1.75F);
    CHECK(math::TryAdvanceMetropolisTour(-1, tour) == math::MathStatus::InvalidArgument);
    CHECK(tour.HoldingWeight == 1.75F);
}

TEST_CASE("Weighted radiance resolves sums rather than averages of intermediate ratios", "[math][metropolis]")
{
    math::WeightedRadiance accumulation;
    math::Vector3 result{1, 1, 1};
    REQUIRE(math::TryResolveWeightedRadiance(accumulation, result) == math::MathStatus::Success);
    CHECK(result.X == 0);
    REQUIRE(math::TryAddWeightedRadiance({2, 0, 1}, 1, accumulation) == math::MathStatus::Success);
    REQUIRE(math::TryAddWeightedRadiance({0, 2, 1}, 3, accumulation) == math::MathStatus::Success);
    REQUIRE(math::TryResolveWeightedRadiance(accumulation, result) == math::MathStatus::Success);
    CHECK(result.X == 0.5F);
    CHECK(result.Y == 1.5F);
    CHECK(result.Z == 1);
    const math::Vector4 before = accumulation.Sum;
    CHECK(math::TryAddWeightedRadiance({-1, 0, 0}, 1, accumulation) == math::MathStatus::InvalidArgument);
    CHECK(accumulation.Sum.X == before.X);
    CHECK(accumulation.Sum.W == before.W);
    const float32 maximum = std::numeric_limits<float32>::max();
    CHECK(math::TryAddWeightedRadiance({maximum, 0, 0}, maximum, accumulation) == math::MathStatus::OutOfRange);
    CHECK(accumulation.Sum.X == before.X);
    CHECK(accumulation.Sum.W == before.W);
    REQUIRE(math::TryAddWeightedRadiance({1, 1, 1}, 0, accumulation) == math::MathStatus::Success);
    CHECK(accumulation.Sum.W == before.W);
    math::WeightedRadiance invalid;
    invalid.Sum.X = 1;
    result = {1, 1, 1};
    CHECK(math::TryResolveWeightedRadiance(invalid, result) == math::MathStatus::InvalidArgument);
    CHECK(result.X == 1);
}
