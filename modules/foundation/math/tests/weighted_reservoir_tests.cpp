#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/weighted_reservoir.hpp>

#include <bit>
#include <limits>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation::math;
using ludus::foundation::core::float64;
using ludus::foundation::core::uint64;
using ludus::foundation::core::usize;

namespace
{
constexpr usize MAX_ENUMERATED_CANDIDATES = 5;

uint64 ComputeGreatestCommonDivisor(uint64 a, uint64 b) noexcept
{
    while (b != 0u)
    {
        const uint64 remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

// Ticket k of a grid of `count` midpoints, (k + 1/2) / count. When count is a
// multiple of every nonzero prefix sum S, ticket * S never equals an integer
// weight w (the product is an odd multiple of S / (2 * count)), and exactly
// count * w / S of the tickets select a candidate. The margin S / (2 * count)
// dwarfs FP32 rounding, so the counts are exact.
float32 MakeMidpointTicket(uint64 k, uint64 count) noexcept
{
    return static_cast<float32>((static_cast<float64>(k) + 0.5) / static_cast<float64>(count));
}

struct EnumeratedSelections
{
    uint64 TicketCount = 0;
    uint64 TupleCount = 0;
    uint64 SelectedCounts[MAX_ENUMERATED_CANDIDATES] = {};
    uint64 EmptyCount = 0;
};

// Streams every tuple of midpoint tickets through the reservoir and counts the
// final selections.
EnumeratedSelections EnumerateSelections(const uint32* weights, usize candidateCount)
{
    REQUIRE(candidateCount <= MAX_ENUMERATED_CANDIDATES);
    EnumeratedSelections result;
    result.TicketCount = 1;
    uint64 prefixSum = 0;
    for (usize i = 0; i < candidateCount; ++i)
    {
        prefixSum += weights[i];
        if (prefixSum != 0u)
        {
            result.TicketCount =
                result.TicketCount / ComputeGreatestCommonDivisor(result.TicketCount, prefixSum) * prefixSum;
        }
    }
    uint64 ticketIndices[MAX_ENUMERATED_CANDIDATES] = {};
    bool isDone = false;
    while (isDone == false)
    {
        WeightedReservoir reservoir;
        for (usize i = 0; i < candidateCount; ++i)
        {
            const ReservoirCandidate candidate
            {
                .Weight = static_cast<float32>(weights[i]),
                .Ticket = MakeMidpointTicket(ticketIndices[i], result.TicketCount),
            };
            bool isSelected = false;
            REQUIRE(TryAddReservoirCandidate(candidate, reservoir, isSelected) == MathStatus::Success);
            REQUIRE(isSelected == (reservoir.HasSelection() == true && reservoir.GetSelectedIndex() == i));
        }
        REQUIRE(reservoir.GetCandidateCount() == candidateCount);
        if (reservoir.HasSelection() == true)
        {
            ++result.SelectedCounts[reservoir.GetSelectedIndex()];
        }
        else
        {
            ++result.EmptyCount;
        }
        ++result.TupleCount;
        isDone = true;
        for (usize i = 0; i < candidateCount && isDone == true; ++i)
        {
            ++ticketIndices[i];
            if (ticketIndices[i] < result.TicketCount)
            {
                isDone = false;
            }
            else
            {
                ticketIndices[i] = 0;
            }
        }
    }
    return result;
}

void RequireExactFrequencies(const uint32* weights, usize candidateCount)
{
    const EnumeratedSelections selections = EnumerateSelections(weights, candidateCount);
    uint64 total = 0;
    for (usize i = 0; i < candidateCount; ++i)
    {
        total += weights[i];
    }
    for (usize i = 0; i < candidateCount; ++i)
    {
        // Selected fraction == weights[i] / total, compared without division.
        REQUIRE(selections.SelectedCounts[i] * total == weights[i] * selections.TupleCount);
    }
    REQUIRE(selections.EmptyCount == (total == 0u ? selections.TupleCount : 0u));
}

float32 MakeFloat(uint32 bits) noexcept
{
    return std::bit_cast<float32>(bits);
}
} // namespace

TEST_CASE("Weighted reservoir selection frequencies are exact over enumerated tickets", "[math][reservoir]")
{
    const uint32 mixed[] = {1, 2, 0, 3};
    RequireExactFrequencies(mixed, 4);
    const uint32 decreasing[] = {2, 1, 1};
    RequireExactFrequencies(decreasing, 3);
    const uint32 leadingZero[] = {0, 1, 1};
    RequireExactFrequencies(leadingZero, 3);
    const uint32 trailingZero[] = {3, 1, 0, 0};
    RequireExactFrequencies(trailingZero, 4);
    const uint32 five[] = {1, 1, 2, 4, 0};
    RequireExactFrequencies(five, 5);

    // The mixed case at a glance: 6 tickets per candidate, 1296 tuples, 1:2:0:3.
    const EnumeratedSelections selections = EnumerateSelections(mixed, 4);
    REQUIRE(selections.TicketCount == 6u);
    REQUIRE(selections.SelectedCounts[0] == 216u);
    REQUIRE(selections.SelectedCounts[1] == 432u);
    REQUIRE(selections.SelectedCounts[2] == 0u);
    REQUIRE(selections.SelectedCounts[3] == 648u);
}

TEST_CASE("Weighted reservoir zero totals are an explicit empty result", "[math][reservoir]")
{
    WeightedReservoir reservoir;
    REQUIRE(reservoir.HasSelection() == false);
    REQUIRE(reservoir.GetCandidateCount() == 0u);
    float32 contribution = -1.0f;
    REQUIRE(TryComputeReservoirContributionWeight(reservoir, 1.0f, contribution) == MathStatus::Success);
    REQUIRE(contribution == 0.0f);

    // Zero, negative zero and weights below the minimum, subnormals included,
    // are counted but never selected.
    REQUIRE(std::bit_cast<uint32>(WeightedReservoirMinimumWeight) == 0x0c800000u);
    const float32 zeroWeights[] = {0.0f,
                                   -0.0f,
                                   std::numeric_limits<float32>::denorm_min(),
                                   MakeFloat(0x007fffffu),
                                   std::numeric_limits<float32>::min(),
                                   MakeFloat(0x0c7fffffu)};
    for (const float32 weight : zeroWeights)
    {
        bool isSelected = true;
        REQUIRE(TryAddReservoirCandidate({ .Weight = weight, .Ticket = 0.0f }, reservoir, isSelected) ==
                MathStatus::Success);
        REQUIRE(isSelected == false);
    }
    REQUIRE(reservoir.HasSelection() == false);
    REQUIRE(reservoir.GetCandidateCount() == 6u);
    REQUIRE(reservoir.GetWeightSum() == 0.0f);
    REQUIRE(reservoir.GetSelectedIndex() == 0u);
    // An empty reservoir does not examine the target, even an invalid one.
    contribution = -1.0f;
    REQUIRE(TryComputeReservoirContributionWeight(reservoir, std::numeric_limits<float32>::quiet_NaN(), contribution) ==
            MathStatus::Success);
    REQUIRE(contribution == 0.0f);
}

TEST_CASE("Weighted reservoir selects a single positive candidate for every ticket", "[math][reservoir]")
{
    // The minimum weight, mantissas near one and two, and the largest float.
    const float32 weights[] = {WeightedReservoirMinimumWeight,
                               MakeFloat(0x0c800001u),
                               MakeFloat(0x0cffffffu),
                               1.0f,
                               3.0f,
                               0.1f,
                               MakeFloat(0x3fffffffu),
                               std::numeric_limits<float32>::max()};
    const float32 tickets[] = {0.0f, 0x1p-24f, 0.5f, 1.0f - 0x1p-24f, std::numeric_limits<float32>::denorm_min()};
    for (const float32 weight : weights)
    {
        for (const float32 ticket : tickets)
        {
            WeightedReservoir reservoir;
            bool isSelected = false;
            REQUIRE(TryAddReservoirCandidate({ .Weight = weight, .Ticket = ticket }, reservoir, isSelected) ==
                    MathStatus::Success);
            REQUIRE(isSelected == true);
            REQUIRE(reservoir.HasSelection() == true);
            REQUIRE(reservoir.GetSelectedIndex() == 0u);
            REQUIRE(reservoir.GetCandidateCount() == 1u);
            REQUIRE(reservoir.GetWeightSum() == weight);
        }
    }

    WeightedReservoir reservoir;
    bool isSelected = false;
    REQUIRE(TryAddReservoirCandidate({ .Weight = 3.0f, .Ticket = 1.0f - 0x1p-24f }, reservoir, isSelected) ==
            MathStatus::Success);
    float32 contribution = 0.0f;
    REQUIRE(TryComputeReservoirContributionWeight(reservoir, 1.5f, contribution) == MathStatus::Success);
    REQUIRE(contribution == 2.0f);
}

TEST_CASE("Weighted reservoir acceptance is strict at ticket * sum == weight", "[math][reservoir]")
{
    struct Case
    {
        float32 SecondTicket;
        uint32 ExpectedIndex;
    };
    const Case cases[] = {{0.5f, 0u}, {0.49999997f, 1u}, {0.0f, 1u}, {1.0f - 0x1p-24f, 0u}};
    for (const Case& test : cases)
    {
        WeightedReservoir reservoir;
        bool isSelected = false;
        REQUIRE(TryAddReservoirCandidate({ .Weight = 1.0f, .Ticket = 0.75f }, reservoir, isSelected) ==
                MathStatus::Success);
        REQUIRE(TryAddReservoirCandidate({ .Weight = 1.0f, .Ticket = test.SecondTicket }, reservoir, isSelected) ==
                MathStatus::Success);
        REQUIRE(isSelected == (test.ExpectedIndex == 1u));
        REQUIRE(reservoir.GetSelectedIndex() == test.ExpectedIndex);
        REQUIRE(reservoir.GetWeightSum() == 2.0f);
    }
}

TEST_CASE("Weighted reservoir rejects invalid candidates and preserves outputs", "[math][reservoir]")
{
    struct Case
    {
        ReservoirCandidate Candidate;
        MathStatus Expected;
    };
    const float32 infinity = std::numeric_limits<float32>::infinity();
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    const Case cases[] = {
        {{ .Weight = nan, .Ticket = 0.5f }, MathStatus::NonFiniteInput},
        {{ .Weight = infinity, .Ticket = 0.5f }, MathStatus::NonFiniteInput},
        {{ .Weight = -infinity, .Ticket = 0.5f }, MathStatus::NonFiniteInput},
        {{ .Weight = 1.0f, .Ticket = nan }, MathStatus::NonFiniteInput},
        {{ .Weight = 1.0f, .Ticket = infinity }, MathStatus::NonFiniteInput},
        {{ .Weight = -1.0f, .Ticket = 0.5f }, MathStatus::InvalidArgument},
        {{ .Weight = -std::numeric_limits<float32>::denorm_min(), .Ticket = 0.5f }, MathStatus::InvalidArgument},
        {{ .Weight = 1.0f, .Ticket = 1.0f }, MathStatus::InvalidArgument},
        {{ .Weight = 1.0f, .Ticket = 2.0f }, MathStatus::InvalidArgument},
        {{ .Weight = 1.0f, .Ticket = -0.25f }, MathStatus::InvalidArgument},
        {{ .Weight = 1.0f, .Ticket = -std::numeric_limits<float32>::denorm_min() }, MathStatus::InvalidArgument},
        {{ .Weight = std::numeric_limits<float32>::max(), .Ticket = 0.0f }, MathStatus::OutOfRange},
    };
    for (const Case& test : cases)
    {
        WeightedReservoir reservoir;
        bool isSelected = false;
        REQUIRE(TryAddReservoirCandidate({ .Weight = std::numeric_limits<float32>::max(), .Ticket = 0.25f },
                                         reservoir,
                                         isSelected) == MathStatus::Success);
        REQUIRE(TryAddReservoirCandidate({ .Weight = 0.0f, .Ticket = 0.25f }, reservoir, isSelected) ==
                MathStatus::Success);
        const WeightedReservoir before = reservoir;
        isSelected = true;
        REQUIRE(TryAddReservoirCandidate(test.Candidate, reservoir, isSelected) == test.Expected);
        REQUIRE(isSelected == true);
        REQUIRE(reservoir.GetWeightSum() == before.GetWeightSum());
        REQUIRE(reservoir.GetCandidateCount() == before.GetCandidateCount());
        REQUIRE(reservoir.GetSelectedIndex() == before.GetSelectedIndex());
    }

    // Negative zero is a valid zero ticket. A subnormal ticket counts as zero:
    // unflushed, 2^-140 * 2^120 = 2^-20 would reject the 2^-30 weight.
    WeightedReservoir reservoir;
    bool isSelected = false;
    REQUIRE(TryAddReservoirCandidate({ .Weight = 0x1p120f, .Ticket = -0.0f }, reservoir, isSelected) ==
            MathStatus::Success);
    REQUIRE(isSelected == true);
    REQUIRE(TryAddReservoirCandidate({ .Weight = 0x1p-30f, .Ticket = 0x1p-140f }, reservoir, isSelected) ==
            MathStatus::Success);
    REQUIRE(isSelected == true);
    REQUIRE(reservoir.GetSelectedIndex() == 1u);
}

TEST_CASE("Weighted reservoir contribution weight validates the target", "[math][reservoir]")
{
    WeightedReservoir reservoir;
    bool isSelected = false;
    REQUIRE(TryAddReservoirCandidate({ .Weight = 6.0f, .Ticket = 0.5f }, reservoir, isSelected) == MathStatus::Success);
    REQUIRE(TryAddReservoirCandidate({ .Weight = 2.0f, .Ticket = 0.5f }, reservoir, isSelected) == MathStatus::Success);
    REQUIRE(reservoir.GetSelectedIndex() == 0u);
    float32 contribution = 0.0f;
    REQUIRE(TryComputeReservoirContributionWeight(reservoir, 4.0f, contribution) == MathStatus::Success);
    REQUIRE(contribution == 1.0f);

    struct Case
    {
        float32 Target;
        MathStatus Expected;
    };
    const Case cases[] = {
        {std::numeric_limits<float32>::quiet_NaN(), MathStatus::NonFiniteInput},
        {std::numeric_limits<float32>::infinity(), MathStatus::NonFiniteInput},
        {0.0f, MathStatus::InvalidArgument},
        {-0.0f, MathStatus::InvalidArgument},
        {-1.0f, MathStatus::InvalidArgument},
        {std::numeric_limits<float32>::denorm_min(), MathStatus::InvalidArgument},
        {std::numeric_limits<float32>::max(), MathStatus::OutOfRange},
    };
    for (const Case& test : cases)
    {
        contribution = -1.0f;
        REQUIRE(TryComputeReservoirContributionWeight(reservoir, test.Target, contribution) == test.Expected);
        REQUIRE(contribution == -1.0f);
    }

    // A huge sum over a tiny target overflows the quotient.
    WeightedReservoir huge;
    REQUIRE(TryAddReservoirCandidate({ .Weight = 0x1p100f, .Ticket = 0.0f }, huge, isSelected) == MathStatus::Success);
    contribution = -1.0f;
    REQUIRE(TryComputeReservoirContributionWeight(huge, 0x1p-100f, contribution) == MathStatus::OutOfRange);
    REQUIRE(contribution == -1.0f);

    // A subnormal quotient is flushed to zero, as GPU arithmetic may do.
    WeightedReservoir tiny;
    REQUIRE(TryAddReservoirCandidate({ .Weight = WeightedReservoirMinimumWeight, .Ticket = 0.0f }, tiny, isSelected) ==
            MathStatus::Success);
    REQUIRE(TryComputeReservoirContributionWeight(tiny, 0x1p30f, contribution) == MathStatus::Success);
    REQUIRE(std::bit_cast<uint32>(contribution) == 0u);
}

TEST_CASE("Weighted reservoir contribution weight is unbiased over enumerated candidates", "[math][reservoir]")
{
    // Four lights with targets 1:2:0:3, sampled uniformly (p_src = 1/4), so a
    // candidate's weight is target / p_src. RIS with M = 2 must reproduce the
    // sum of any g that vanishes wherever the target does: E[W * g(Y)] = sum g.
    constexpr usize LIGHT_COUNT = 4;
    constexpr uint32 CANDIDATE_COUNT = 2;
    const float32 targets[LIGHT_COUNT] = {1.0f, 2.0f, 0.0f, 3.0f};
    const float64 integrand[LIGHT_COUNT] = {5.0, 1.0, 0.0, 2.0};
    const float32 sourceProbability = 0.25f;
    // A multiple of every possible prefix sum 4 * (1, 2, 3, 4, 5, 6) makes the
    // second candidate's midpoint tickets exact (see MakeMidpointTicket).
    constexpr uint64 TICKET_COUNT = 240;
    float64 estimate = 0.0;
    for (usize first = 0; first < LIGHT_COUNT; ++first)
    {
        for (usize second = 0; second < LIGHT_COUNT; ++second)
        {
            for (uint64 k = 0; k < TICKET_COUNT; ++k)
            {
                const usize lights[CANDIDATE_COUNT] = {first, second};
                WeightedReservoir reservoir;
                usize selectedLight = 0;
                for (uint32 i = 0; i < CANDIDATE_COUNT; ++i)
                {
                    const ReservoirCandidate candidate
                    {
                        .Weight = targets[lights[i]] / sourceProbability,
                        .Ticket = i == 0u ? 0.5f : MakeMidpointTicket(k, TICKET_COUNT),
                    };
                    bool isSelected = false;
                    REQUIRE(TryAddReservoirCandidate(candidate, reservoir, isSelected) == MathStatus::Success);
                    if (isSelected == true)
                    {
                        selectedLight = lights[i];
                    }
                }
                float32 contribution = 0.0f;
                REQUIRE(TryComputeReservoirContributionWeight(reservoir, targets[selectedLight], contribution) ==
                        MathStatus::Success);
                estimate += static_cast<float64>(contribution) * integrand[selectedLight];
            }
        }
    }
    estimate /= static_cast<float64>(LIGHT_COUNT * LIGHT_COUNT * TICKET_COUNT);
    const float64 expected = 8.0;
    // W is FP32; its relative rounding is far below this bound.
    REQUIRE(estimate > expected * (1.0 - 1e-6));
    REQUIRE(estimate < expected * (1.0 + 1e-6));
}

TEST_CASE("Weighted reservoir replays addressed tickets in a fixed order", "[math][reservoir]")
{
    RandomKey key;
    REQUIRE(TryMakeRandomKey(42, 7, key) == MathStatus::Success);
    REQUIRE(WeightedReservoirSelectionVersion == 1u);
    const float32 weights[] = {0.5f, 0.0f, 2.0f, 1.25f, 0.75f, 3.0f, 0.0f, 1.0f};
    WeightedReservoir first;
    WeightedReservoir second;
    for (uint32 pass = 0; pass < 2; ++pass)
    {
        WeightedReservoir& reservoir = pass == 0u ? first : second;
        for (uint32 i = 0; i < 8u; ++i)
        {
            // One ticket dimension per candidate, as in the RIS random layout.
            const RandomAddress address{ .Scope = 123, .Event = 5, .Dimension = static_cast<uint16>(13u + 2u * i) };
            bool isSelected = false;
            REQUIRE(TryAddReservoirCandidate({ .Weight = weights[i], .Ticket = SampleFloat01(key, address) },
                                             reservoir,
                                             isSelected) == MathStatus::Success);
        }
    }
    REQUIRE(first.GetCandidateCount() == 8u);
    REQUIRE(first.GetWeightSum() == 8.5f);
    REQUIRE(first.HasSelection() == true);
    REQUIRE(second.GetSelectedIndex() == first.GetSelectedIndex());
    REQUIRE(weights[first.GetSelectedIndex()] > 0.0f);
}
