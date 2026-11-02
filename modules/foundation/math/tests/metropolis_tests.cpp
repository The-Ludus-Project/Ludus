#include <ludus/foundation/math/metropolis.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace ludus::foundation;
namespace math = ludus::foundation::math;

TEST_CASE("Jump Restore rates and exponential race times preserve their domains", "[math][metropolis]")
{
    math::JumpRestoreRates rates;
    REQUIRE(math::TryComputeJumpRestoreRates(2, 2, rates) == math::MathStatus::Success);
    CHECK(rates.RestoreProbability == 0.5F);
    CHECK(rates.MeanHoldingTime == 0.5F);
    REQUIRE(math::TryComputeJumpRestoreRates(3, 1, rates) == math::MathStatus::Success);
    CHECK(math::Abs(rates.RestoreProbability - 0.25F) < 0.000001F);
    CHECK(math::Abs(rates.MeanHoldingTime - 0.75F) < 0.000001F);
    REQUIRE(math::TryComputeJumpRestoreRates(1, 3, rates) == math::MathStatus::Success);
    CHECK(math::Abs(rates.RestoreProbability - 0.75F) < 0.000001F);
    CHECK(math::Abs(rates.MeanHoldingTime - 0.25F) < 0.000001F);
    const math::JumpRestoreRates previous = rates;
    CHECK(math::TryComputeJumpRestoreRates(0, 1, rates) == math::MathStatus::InvalidArgument);
    CHECK(rates.MeanHoldingTime == previous.MeanHoldingTime);
    CHECK(math::TryComputeJumpRestoreRates(1, std::numeric_limits<float32>::infinity(), rates) ==
          math::MathStatus::NonFiniteInput);
    const float32 maximum = std::numeric_limits<float32>::max();
    CHECK(math::TryComputeJumpRestoreRates(std::numeric_limits<float32>::min(), maximum, rates) ==
          math::MathStatus::OutOfRange);
    CHECK(rates.MeanHoldingTime == previous.MeanHoldingTime);
    REQUIRE(math::TryComputeJumpRestoreRates(maximum, maximum, rates) == math::MathStatus::Success);
    CHECK(rates.MeanHoldingTime == 0.5F);
    float32 time = 1;
    REQUIRE(math::TrySampleJumpRestoreHoldingTime(0, 0.5F, time) == math::MathStatus::Success);
    CHECK(time == 0);
    REQUIRE(math::TrySampleJumpRestoreHoldingTime(0.5F, 0.5F, time) == math::MathStatus::Success);
    CHECK(math::Abs(time - 0.34657359F) < 0.000001F);
    const float32 previousTime = time;
    CHECK(math::TrySampleJumpRestoreHoldingTime(1, 0.5F, time) == math::MathStatus::InvalidArgument);
    CHECK(time == previousTime);
    constexpr uint32 TICKET_COUNT = 4096;
    float64 mean = 0;
    for (uint32 index = 0; index < TICKET_COUNT; ++index)
    {
        const float32 ticket = (static_cast<float32>(index) + 0.5F) / TICKET_COUNT;
        REQUIRE(math::TrySampleJumpRestoreHoldingTime(ticket, 0.5F, time) == math::MathStatus::Success);
        mean += static_cast<float64>(time) / TICKET_COUNT;
    }
    CHECK(math::Abs(mean - 0.5) < 0.001);
}

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
