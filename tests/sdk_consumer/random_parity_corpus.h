#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/weighted_reservoir.hpp>

#include "random_parity_layout.h"

#include <cstring>

// Case corpus and comparison for FoundationMath's shader ports of addressed
// random v1 (modules/foundation/math/shaders/math.slang) and the weighted
// reservoir (shaders/weighted_reservoir.slang); see
// docs/architecture/randomness.md#gpu-addressed-sampling. Shared by the native
// renderer test (gpu_random_tests.cpp) and the WebGPU browser fixture
// (random_parity.cpp) so every backend checks the same cases. Every expected
// value comes from the CPU reference or a pinned known answer, never from a GPU.
namespace ludus::tests::random_parity
{
namespace math = ludus::foundation::math;
using foundation::Array;
using foundation::BorrowedView;
using foundation::float32;
using foundation::StaticArray;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;

/// Mismatches recorded in detail by CompareResults; the total count is unbounded.
inline constexpr usize MAX_REPORTED_MISMATCHES = 16;

/// One kernel input: (kind, key low, key high, bound) then a kind-specific operand.
struct ParityCase final
{
    math::RandomBlock Header;
    math::RandomBlock Operand;
};
static_assert(sizeof(ParityCase) == LUDUS_RANDOM_PARITY_WORDS_PER_CASE * sizeof(math::RandomBlock));

/// One kernel case with the result the GPU must reproduce; named fields keep the
/// operand and expected words from being exchanged.
struct ParityEntry final
{
    uint32 Kind = LUDUS_RANDOM_PARITY_CASE_PADDING;
    math::RandomKey Key;
    uint32 Bound = 0;
    math::RandomBlock Operand;
    math::RandomBlock Expected;
};

/// Kernel inputs and the expected result of each, in dispatch order.
class ParityCorpus final
{
public:
    void Add(const ParityEntry& entry) noexcept
    {
        const ParityCase item
        {
            .Header = {{entry.Kind,
                        static_cast<uint32>(entry.Key.Value),
                        static_cast<uint32>(entry.Key.Value >> 32u),
                        entry.Bound}},
            .Operand = entry.Operand,
        };
        if (mCases.TryAdd(item) == false || mExpected.TryAdd(entry.Expected) == false)
        {
            mHasFailedAllocation = true;
        }
    }
    void AddSample(math::RandomKey key, uint64 scope, uint32 event, uint32 dimension, uint32 bound) noexcept
    {
        // An out-of-range dimension must alias dimension % 65536, as CPU uint16
        // narrowing does, and never a retry attempt; bounded sampling rejects it.
        const bool isInRange = dimension <= math::RandomMaxDimension;
        const math::RandomAddress address{scope, event, static_cast<uint16>(dimension)};
        const uint32 raw = math::SampleUInt32(key, address);
        const float32 sample = math::SampleFloat01(key, address);
        uint32 sampleBits = 0;
        std::memcpy(&sampleBits, &sample, sizeof(sampleBits));
        uint32 bounded = 0;
        uint32 flags = isInRange == true ? LUDUS_RANDOM_PARITY_IS_IN_RANGE : 0u;
        if (isInRange == true && math::TrySampleBounded(key, address, bound, bounded) == math::MathStatus::Success)
        {
            flags |= LUDUS_RANDOM_PARITY_IS_SAMPLED;
        }
        else
        {
            bounded = 0;
        }
        Add(
        {
            .Kind = LUDUS_RANDOM_PARITY_CASE_SAMPLE,
            .Key = key,
            .Bound = bound,
            .Operand = {{static_cast<uint32>(scope), static_cast<uint32>(scope >> 32u), event, dimension}},
            .Expected = {{raw, sampleBits, bounded, flags}},
        });
    }
    void AddMultiply(uint32 a, uint32 b) noexcept
    {
        const uint64 product = static_cast<uint64>(a) * b;
        Add(
        {
            .Kind = LUDUS_RANDOM_PARITY_CASE_MULTIPLY,
            .Key = {},
            .Bound = 0,
            .Operand = {{a, b, 0, 0}},
            .Expected = {{static_cast<uint32>(product), static_cast<uint32>(product >> 32u), 0, 0}},
        });
    }
    /// Streams candidates through the CPU reservoir until one is rejected, then adds
    /// the reservoir case and one candidate case per entry for the kernel to replay.
    /// @param selectedTarget Target passed to the contribution weight. Callers keep
    ///        M * target a power of two, or make the quotient fail or underflow, so
    ///        every backend's FP32 division agrees with the CPU.
    /// @return False when more than LUDUS_RANDOM_PARITY_MAX_RESERVOIR_CANDIDATES
    ///         candidates are supplied; the corpus is then unchanged.
    [[nodiscard]] bool TryAddReservoir(BorrowedView<const math::ReservoirCandidate> candidates,
                                       float32 selectedTarget) noexcept
    {
        if (candidates.IsValid() == false || candidates.GetSize() > LUDUS_RANDOM_PARITY_MAX_RESERVOIR_CANDIDATES)
        {
            return false;
        }
        math::WeightedReservoir reservoir;
        uint32 flags = 0;
        for (usize i = 0; i < candidates.GetSize(); ++i)
        {
            bool isSelected = false;
            if (math::TryAddReservoirCandidate(candidates[i], reservoir, isSelected) != math::MathStatus::Success)
            {
                flags |= LUDUS_RANDOM_PARITY_RESERVOIR_IS_REJECTED;
                break;
            }
            if (isSelected == true)
            {
                flags |= 1u << (LUDUS_RANDOM_PARITY_RESERVOIR_SELECTED_SHIFT + i);
            }
        }
        flags |= reservoir.GetCandidateCount();
        if (reservoir.HasSelection() == true)
        {
            flags |= LUDUS_RANDOM_PARITY_RESERVOIR_HAS_SELECTION;
        }
        float32 contribution = 0.0f;
        if ((flags & LUDUS_RANDOM_PARITY_RESERVOIR_IS_REJECTED) == 0u &&
            math::TryComputeReservoirContributionWeight(reservoir, selectedTarget, contribution) ==
                math::MathStatus::Success)
        {
            flags |= LUDUS_RANDOM_PARITY_RESERVOIR_HAS_CONTRIBUTION;
        }
        Add(
        {
            .Kind = LUDUS_RANDOM_PARITY_CASE_RESERVOIR,
            .Key = {},
            .Bound = static_cast<uint32>(candidates.GetSize()),
            .Operand = {{GetFloatBits(selectedTarget), 0, 0, 0}},
            .Expected = {{reservoir.GetSelectedIndex(),
                          GetFloatBits(reservoir.GetWeightSum()),
                          GetFloatBits(contribution),
                          flags}},
        });
        for (usize i = 0; i < candidates.GetSize(); ++i)
        {
            Add(
            {
                .Kind = LUDUS_RANDOM_PARITY_CASE_RESERVOIR_CANDIDATE,
                .Key = {},
                .Bound = 0,
                .Operand = {{GetFloatBits(candidates[i].Weight), GetFloatBits(candidates[i].Ticket), 0, 0}},
                .Expected = {},
            });
        }
        return true;
    }
    void PadToGroup() noexcept
    {
        while (mCases.GetSize() % LUDUS_RANDOM_PARITY_GROUP_SIZE != 0 && mHasFailedAllocation == false)
        {
            Add({ .Kind = LUDUS_RANDOM_PARITY_CASE_PADDING, .Key = {}, .Bound = 0, .Operand = {}, .Expected = {} });
        }
    }
    [[nodiscard]] bool HasFailedAllocation() const noexcept
    {
        return mHasFailedAllocation;
    }
    [[nodiscard]] const Array<ParityCase>& GetCases() const noexcept
    {
        return mCases;
    }
    [[nodiscard]] const Array<math::RandomBlock>& GetExpected() const noexcept
    {
        return mExpected;
    }
    /// Kernel input size; the result buffer holds one RandomBlock per case.
    [[nodiscard]] usize GetCaseBytes() const noexcept
    {
        return mCases.GetSize() * sizeof(ParityCase);
    }
    [[nodiscard]] usize GetResultBytes() const noexcept
    {
        return mCases.GetSize() * sizeof(math::RandomBlock);
    }
    /// Compute workgroups that cover every case; PadToGroup makes the division exact.
    [[nodiscard]] uint32 GetGroupCount() const noexcept
    {
        return static_cast<uint32>(mCases.GetSize() / LUDUS_RANDOM_PARITY_GROUP_SIZE);
    }

private:
    [[nodiscard]] static uint32 GetFloatBits(float32 value) noexcept
    {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    Array<ParityCase> mCases;
    Array<math::RandomBlock> mExpected;
    bool mHasFailedAllocation = false;
};

[[nodiscard]] inline float32 MakeFloat(uint32 bits) noexcept
{
    float32 value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// Adds the weighted-reservoir cases: shader constants, every ticket tuple of an
/// exact-frequency grid, addressed RIS streams, and zero, boundary, flush,
/// rejection and contribution-weight edge cases. Every successful contribution
/// weight divides by a power of two M * target, which all backends divide
/// exactly, or underflows to zero.
/// @return False when the CPU reference cannot produce a key or a stream is too long.
[[nodiscard]] inline bool TryAddReservoirCases(ParityCorpus& outCorpus) noexcept
{
    using math::ReservoirCandidate;
    uint32 minimumWeightBits = 0;
    std::memcpy(&minimumWeightBits, &math::WeightedReservoirMinimumWeight, sizeof(minimumWeightBits));
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_RESERVOIR_CONSTANTS,
        .Key = {},
        .Bound = 0,
        .Operand = {},
        .Expected = {{math::WeightedReservoirSelectionVersion, minimumWeightBits, 0, 0}},
    });

    // Weights 1:2:0:3 with midpoint tickets (k + 1/2) / 6: six is a multiple of
    // every prefix sum, so the 1296 streams select 216:432:0:648 exactly, as the
    // FoundationMath unit test proves on the CPU.
    constexpr usize GRID_CANDIDATE_COUNT = 4;
    constexpr uint32 GRID_TICKET_COUNT = 6;
    const StaticArray<float32, GRID_CANDIDATE_COUNT> gridWeights = {1.0f, 2.0f, 0.0f, 3.0f};
    for (uint32 tuple = 0; tuple < GRID_TICKET_COUNT * GRID_TICKET_COUNT * GRID_TICKET_COUNT * GRID_TICKET_COUNT;
         ++tuple)
    {
        StaticArray<ReservoirCandidate, GRID_CANDIDATE_COUNT> candidates{};
        uint32 digits = tuple;
        for (usize i = 0; i < GRID_CANDIDATE_COUNT; ++i)
        {
            const float32 ticket = (static_cast<float32>(digits % GRID_TICKET_COUNT) + 0.5f) / GRID_TICKET_COUNT;
            candidates[i] = { .Weight = gridWeights[i], .Ticket = ticket };
            digits /= GRID_TICKET_COUNT;
        }
        if (outCorpus.TryAddReservoir({candidates.GetData(), GRID_CANDIDATE_COUNT}, 0.5f) == false)
        {
            return false;
        }
    }

    // Addressed RIS streams as the many-area-light layout uses them: candidate i
    // draws its weight and ticket from dimensions 12 + 2i and 13 + 2i. Weights are
    // general FP32 values, with about one in eight zero; M * target = 1.
    math::RandomKey key;
    if (math::TryMakeRandomKey(42, 7, key) != math::MathStatus::Success)
    {
        return false;
    }
    constexpr usize STREAM_CANDIDATE_COUNT = LUDUS_RANDOM_PARITY_MAX_RESERVOIR_CANDIDATES;
    constexpr uint64 STREAM_COUNT = 256;
    for (uint64 scope = 0; scope < STREAM_COUNT; ++scope)
    {
        StaticArray<ReservoirCandidate, STREAM_CANDIDATE_COUNT> candidates{};
        for (usize i = 0; i < STREAM_CANDIDATE_COUNT; ++i)
        {
            const uint16 dimension = static_cast<uint16>(12u + 2u * i);
            const float32 weightSample = math::SampleFloat01(key, {scope, 0, dimension});
            candidates[i] =
            {
                .Weight = weightSample < 0.125f ? 0.0f : weightSample * 8.0f,
                .Ticket = math::SampleFloat01(key, {scope, 0, static_cast<uint16>(dimension + 1u)}),
            };
        }
        if (outCorpus.TryAddReservoir({candidates.GetData(), STREAM_CANDIDATE_COUNT}, 0.125f) == false)
        {
            return false;
        }
    }

    // Edge streams. Target bits that an empty reservoir never examines are NaN.
    const float32 maxTicket = 1.0f - 0x1p-24f;
    const float32 largest = MakeFloat(0x7f7fffffu);
    const float32 notANumber = MakeFloat(0x7fc00000u);
    const ReservoirCandidate zeroTotal[] = {
        { .Weight = 0.0f, .Ticket = 0.0f },
        { .Weight = MakeFloat(0x80000000u), .Ticket = 0.5f },
        { .Weight = MakeFloat(0x00000001u), .Ticket = 0.0f },
        { .Weight = MakeFloat(0x00800000u), .Ticket = 0.0f },
        { .Weight = MakeFloat(0x0c7fffffu), .Ticket = 0.0f },
    };
    const ReservoirCandidate single[] = {{ .Weight = 3.0f, .Ticket = maxTicket }};
    const ReservoirCandidate strictBoundary[] = {
        { .Weight = 1.0f, .Ticket = 0.75f },
        { .Weight = 1.0f, .Ticket = 0.5f },
    };
    const ReservoirCandidate belowBoundary[] = {
        { .Weight = 1.0f, .Ticket = 0.75f },
        { .Weight = 1.0f, .Ticket = 0.49999997f },
    };
    // Unflushed, the subnormal ticket would give 2^-140 * 2^120 = 2^-20 and reject 2^-30.
    const ReservoirCandidate subnormalTicket[] = {
        { .Weight = 0x1p120f, .Ticket = MakeFloat(0x80000000u) },
        { .Weight = 0x1p-30f, .Ticket = 0x1p-140f },
    };
    if (outCorpus.TryAddReservoir({}, notANumber) == false ||
        outCorpus.TryAddReservoir(zeroTotal, notANumber) == false || outCorpus.TryAddReservoir(single, 4.0f) == false ||
        outCorpus.TryAddReservoir(strictBoundary, 1.0f) == false ||
        outCorpus.TryAddReservoir(belowBoundary, 1.0f) == false ||
        outCorpus.TryAddReservoir(subnormalTicket, 0x1p100f) == false)
    {
        return false;
    }
    // The first positive weight is selected even at the largest ticket.
    const float32 firstWeights[] =
        {MakeFloat(0x0c800000u), MakeFloat(0x0c800001u), MakeFloat(0x0cffffffu), 0.1f, MakeFloat(0x3fffffffu), largest};
    for (const float32 weight : firstWeights)
    {
        const ReservoirCandidate first[] = {
            { .Weight = 0.0f, .Ticket = 0.0f },
            { .Weight = weight, .Ticket = maxTicket },
        };
        if (outCorpus.TryAddReservoir(first, 0.5f) == false)
        {
            return false;
        }
    }
    // Rejected candidates stop the stream; the trailing candidate is never streamed.
    const ReservoirCandidate rejected[] = {
        { .Weight = notANumber, .Ticket = 0.5f },
        { .Weight = MakeFloat(0x7f800000u), .Ticket = 0.5f },
        { .Weight = MakeFloat(0xff800000u), .Ticket = 0.5f },
        { .Weight = -1.0f, .Ticket = 0.5f },
        { .Weight = MakeFloat(0x80000001u), .Ticket = 0.5f },
        { .Weight = 1.0f, .Ticket = 1.0f },
        { .Weight = 1.0f, .Ticket = 2.0f },
        { .Weight = 1.0f, .Ticket = -0.25f },
        { .Weight = 1.0f, .Ticket = notANumber },
        { .Weight = 1.0f, .Ticket = MakeFloat(0x7f800000u) },
        { .Weight = 1.0f, .Ticket = MakeFloat(0x80000001u) },
        { .Weight = largest, .Ticket = 0.0f },
    };
    for (const ReservoirCandidate& candidate : rejected)
    {
        const ReservoirCandidate stream[] = {
            { .Weight = largest, .Ticket = 0.25f },
            { .Weight = 0.0f, .Ticket = 0.5f },
            candidate,
            { .Weight = 1.0f, .Ticket = 0.0f },
        };
        if (outCorpus.TryAddReservoir(stream, 1.0f) == false)
        {
            return false;
        }
    }
    // Contribution weights: invalid targets, overflow of M * target and of the
    // quotient, and subnormal quotients flushed to zero.
    const ReservoirCandidate two[] = {{ .Weight = 2.0f, .Ticket = 0.5f }};
    const float32 targets[] = {0.0f,
                               MakeFloat(0x80000000u),
                               -1.0f,
                               notANumber,
                               MakeFloat(0x7f800000u),
                               MakeFloat(0x00000001u),
                               MakeFloat(0x00800000u),
                               largest};
    for (const float32 target : targets)
    {
        if (outCorpus.TryAddReservoir(two, target) == false)
        {
            return false;
        }
    }
    const ReservoirCandidate twoCandidates[] = {
        { .Weight = 2.0f, .Ticket = 0.5f },
        { .Weight = 0.0f, .Ticket = 0.5f },
    };
    const ReservoirCandidate huge[] = {{ .Weight = 0x1p100f, .Ticket = 0.0f }};
    const ReservoirCandidate minimum[] = {{ .Weight = 0x1p-102f, .Ticket = 0.0f }};
    return outCorpus.TryAddReservoir(twoCandidates, largest) == true &&
           outCorpus.TryAddReservoir(huge, 0x1p-100f) == true && outCorpus.TryAddReservoir(minimum, 0x1p30f) == true;
}

/// Builds the full corpus: Random123 and Ludus v1 known answers, layout
/// constants, forced retry-loop boundaries, endpoint floats, live CPU parity for
/// raw/float/bounded samples, MultiplyWide limb sweeps and the weighted-reservoir
/// cases, padded to whole workgroups.
/// @return False when allocation fails or the CPU reference disagrees with a
///         pinned key derivation; the corpus is then unusable.
[[nodiscard]] inline bool TryBuildCorpus(ParityCorpus& outCorpus) noexcept
{
    // Random123 tests/kat_vectors, revision 9545ff6413f258be2f04c1d319d99aaef7521150, the same
    // pinned zero, all-ones and mixed vectors as FoundationMath's addressed_random_tests.cpp.
    // https://github.com/DEShawResearch/random123/blob/9545ff6413f258be2f04c1d319d99aaef7521150/tests/kat_vectors
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_PHILOX,
        .Key = {0},
        .Bound = 0,
        .Operand = {{0, 0, 0, 0}},
        .Expected = {{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u}},
    });
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_PHILOX,
        .Key = {0xffffffffffffffffULL},
        .Bound = 0,
        .Operand = {{0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}},
        .Expected = {{0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu}},
    });
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_PHILOX,
        .Key = {0x299f31d0a4093822ULL},
        .Bound = 0,
        .Operand = {{0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u}},
        .Expected = {{0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u}},
    });

    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_CONSTANTS,
        .Key = {},
        .Bound = 0,
        .Operand = {},
        .Expected = {{math::RandomAddressLayoutVersion,
                      math::RandomBoundedMappingVersion,
                      math::RandomMaxDimension,
                      math::RandomBoundedAttemptCount}},
    });

    // Retry loop boundaries through the production loop with injected draws.
    // Bound 0x80000001 has threshold 0x7fffffff, so a zero draw always rejects.
    constexpr uint32 HALF_REJECTING_BOUND = 0x80000001u;
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_FORCED_EXHAUSTION,
        .Key = {},
        .Bound = HALF_REJECTING_BOUND,
        .Operand = {},
        .Expected = {{0, 0, 0, 0}},
    });
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_FORCED_EXHAUSTION,
        .Key = {},
        .Bound = 0,
        .Operand = {},
        .Expected = {{0, 0, 0, 0}},
    });
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_FORCED_LAST_ATTEMPT,
        .Key = {},
        .Bound = HALF_REJECTING_BOUND,
        .Operand = {},
        .Expected = {{0, LUDUS_RANDOM_PARITY_IS_SAMPLED, 0, 0}},
    });
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_FORCED_CONSTANT,
        .Key = {},
        .Bound = 1,
        .Operand = {{0xffffffffu, 0, 0, 0}},
        .Expected = {{0, LUDUS_RANDOM_PARITY_IS_SAMPLED, 0, 0}},
    });

    // Endpoint floats pinned independently of the CPU mapping code.
    constexpr usize ENDPOINT_FLOAT_COUNT = 5;
    const StaticArray<uint32, ENDPOINT_FLOAT_COUNT> floatBits = {0u, 255u, 256u, 0x80000000u, 0xffffffffu};
    const StaticArray<float32, ENDPOINT_FLOAT_COUNT> floatValues = {0.0f, 0.0f, 0x1p-24f, 0.5f, 1.0f - 0x1p-24f};
    for (usize i = 0; i < ENDPOINT_FLOAT_COUNT; ++i)
    {
        uint32 expectedBits = 0;
        std::memcpy(&expectedBits, &floatValues[i], sizeof(expectedBits));
        outCorpus.Add(
        {
            .Kind = LUDUS_RANDOM_PARITY_CASE_FLOAT,
            .Key = {},
            .Bound = 0,
            .Operand = {{floatBits[i], 0, 0, 0}},
            .Expected = {{expectedBits, 0, 0, 0}},
        });
    }

    // Ludus v1 golden answers from addressed_random_tests.cpp, pinned so the GPU
    // is checked against frozen values as well as the live CPU reference.
    math::RandomKey key;
    if (math::TryMakeRandomKey(42, 7, key) != math::MathStatus::Success || key.Value != 0xccf635ee9e9e2fa4ULL)
    {
        return false;
    }
    const uint32 sampled = LUDUS_RANDOM_PARITY_IS_SAMPLED | LUDUS_RANDOM_PARITY_IS_IN_RANGE;
    const float32 golden = 0.5788143277168274f;
    uint32 goldenBits = 0;
    std::memcpy(&goldenBits, &golden, sizeof(goldenBits));
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_SAMPLE,
        .Key = key,
        .Bound = 1000,
        .Operand = {{0x89abcdefu, 0x01234567u, 99, 1234}},
        .Expected = {{0x942d2d40u, goldenBits, 578, sampled}},
    });
    // Attempts 0 and 1 reject at this address; attempt 2 is accepted.
    math::RandomBlock forcedRejection = {{0xa3a6ca03u, 0, 0x3c0cb600u, sampled}};
    const float32 forcedSample = math::SampleFloat01(key, {});
    std::memcpy(&forcedRejection.Values[1], &forcedSample, sizeof(uint32));
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_SAMPLE,
        .Key = key,
        .Bound = HALF_REJECTING_BOUND,
        .Operand = {},
        .Expected = forcedRejection,
    });
    // The aligned block holding dimensions 1232-1235 (TrySampleBlock's golden answer).
    outCorpus.Add(
    {
        .Kind = LUDUS_RANDOM_PARITY_CASE_PHILOX,
        .Key = key,
        .Bound = 0,
        .Operand = {{0x89abcdefu, 0x01234567u, 99, 1232u >> 2u}},
        .Expected = {{0x991146b3u, 0x1aa17390u, 0x942d2d40u, 0x487e565du}},
    });

    // Live CPU parity across key words, both scope words, event extremes,
    // lane boundaries 0-3/4-7, the top of the dimension range, out-of-range
    // dimensions and bounds that force rejection (0x80000001), exceed 16 bits
    // or are invalid (0).
    math::RandomKey derived;
    if (math::TryMakeRandomKey(0xffffffffffffffffULL, 0xffffffffu, derived) != math::MathStatus::Success)
    {
        return false;
    }
    const math::RandomKey keys[] = {{0}, {0xffffffffffffffffULL}, key, derived, {0x0123456789abcdefULL}};
    const uint64 scopes[] = {0, 1, 0xffffffffULL, 0x100000000ULL, 0x0123456789abcdefULL, 0xffffffffffffffffULL};
    const uint32 events[] = {0, 1, 99, 0x80000000u, 0xffffffffu};
    const uint32 dimensions[] =
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 1232, 1235, 65531, 65532, 65535, 0x10000, 0x10000 + 1234, 0x20003, 0xffffffffu};
    const uint32 bounds[] =
        {1, 2, 3, 1000, 65535, 65536, 65537, 0x7fffffffu, 0x80000000u, HALF_REJECTING_BOUND, 0xffffffffu, 0};
    for (const math::RandomKey& sampleKey : keys)
    {
        for (const uint64 scope : scopes)
        {
            for (const uint32 event : events)
            {
                for (const uint32 dimension : dimensions)
                {
                    for (const uint32 bound : bounds)
                    {
                        outCorpus.AddSample(sampleKey, scope, event, dimension, bound);
                    }
                }
            }
        }
    }

    // MultiplyWide: every pair of 16-bit limb edge values, then exhaustive sweeps
    // of one limb against an all-ones operand where every carry is maximal.
    const uint32 edges[] = {0u,          1u,          2u,          0x7fffu,     0x8000u,     0xfffeu,
                            0xffffu,     0x10000u,    0x10001u,    0x1ffffu,    0x7fffffffu, 0x80000000u,
                            0x80000001u, 0xfffeffffu, 0xffff0000u, 0xffff0001u, 0xfffffffeu, 0xffffffffu,
                            0xd2511f53u, 0xcd9e8d57u, 0x9e3779b9u, 0xbb67ae85u};
    for (const uint32 a : edges)
    {
        for (const uint32 b : edges)
        {
            outCorpus.AddMultiply(a, b);
        }
    }
    constexpr uint32 LIMB_VALUES = 0x10000u;
    for (uint32 limb = 0; limb < LIMB_VALUES; ++limb)
    {
        outCorpus.AddMultiply(0xffffffffu, 0xffff0000u | limb);
        outCorpus.AddMultiply((limb << 16u) | 0xffffu, 0xffffffffu);
    }
    if (TryAddReservoirCases(outCorpus) == false)
    {
        return false;
    }
    outCorpus.PadToGroup();
    return outCorpus.HasFailedAllocation() == false;
}

/// One result word that differs from the CPU reference or a pinned answer.
struct ParityMismatch final
{
    usize Index = 0;
    uint32 Kind = 0;
    usize Lane = 0;
    uint32 Actual = 0;
    uint32 Expected = 0;
};

/// Outcome of CompareResults: every mismatching word is counted, the first
/// MAX_REPORTED_MISMATCHES are recorded.
struct ParityComparison final
{
    usize MismatchCount = 0;
    StaticArray<ParityMismatch, MAX_REPORTED_MISMATCHES> Reported{};
};

/// Compares GPU results with the corpus expectations word by word.
/// @param actual One RandomBlock per corpus case, in dispatch order; storage
///        stays owned by the caller.
/// @return False when actual is invalid or its extent differs from the corpus;
///         outComparison is then left unchanged.
[[nodiscard]] inline bool TryCompareResults(const ParityCorpus& corpus,
                                            BorrowedView<const math::RandomBlock> actual,
                                            ParityComparison& outComparison) noexcept
{
    const Array<ParityCase>& cases = corpus.GetCases();
    const Array<math::RandomBlock>& expected = corpus.GetExpected();
    if (actual.IsValid() == false || actual.GetSize() != cases.GetSize() || expected.GetSize() != cases.GetSize())
    {
        return false;
    }
    ParityComparison comparison;
    constexpr usize LANES_PER_RESULT = 4;
    for (usize index = 0; index < cases.GetSize(); ++index)
    {
        for (usize lane = 0; lane < LANES_PER_RESULT; ++lane)
        {
            if (actual[index].Values[lane] == expected[index].Values[lane])
            {
                continue;
            }
            if (comparison.MismatchCount < MAX_REPORTED_MISMATCHES)
            {
                comparison.Reported[comparison.MismatchCount] =
                {
                    .Index = index,
                    .Kind = cases[index].Header.Values[0],
                    .Lane = lane,
                    .Actual = actual[index].Values[lane],
                    .Expected = expected[index].Values[lane],
                };
            }
            ++comparison.MismatchCount;
        }
    }
    outComparison = comparison;
    return true;
}
} // namespace ludus::tests::random_parity
