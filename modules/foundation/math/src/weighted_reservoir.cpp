#include <ludus/foundation/math/weighted_reservoir.hpp>

#include <cstring>

namespace ludus::foundation::math
{
namespace
{
// IEEE binary32 fields. Classifying on bits keeps the CPU and the GPU port in
// agreement where GPU arithmetic may flush subnormals or assume finite values.
constexpr uint32 FLOAT32_SIGN_BIT = 0x80000000u;
constexpr uint32 FLOAT32_MAGNITUDE_BITS = 0x7fffffffu;
constexpr uint32 FLOAT32_EXPONENT_BITS = 0x7f800000u;
constexpr uint32 FLOAT32_SMALLEST_NORMAL_BITS = 0x00800000u;
constexpr uint32 FLOAT32_ONE_BITS = 0x3f800000u;
// WeightedReservoirMinimumWeight, 2^-102: biased exponent 25.
constexpr uint32 MINIMUM_WEIGHT_BITS = 0x0c800000u;
constexpr uint32 MAX_CANDIDATE_COUNT = 0xffffffffu;

uint32 GetBits(float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool IsFiniteBits(uint32 bits) noexcept
{
    return (bits & FLOAT32_EXPONENT_BITS) != FLOAT32_EXPONENT_BITS;
}

// True for a strictly negative value, excluding -0 and NaN (checked first).
bool IsNegativeBits(uint32 bits) noexcept
{
    return (bits & FLOAT32_SIGN_BIT) != 0u && (bits & FLOAT32_MAGNITUDE_BITS) != 0u;
}

// True for +-0 and every subnormal: both count as zero.
bool IsZeroOrSubnormalBits(uint32 bits) noexcept
{
    return (bits & FLOAT32_MAGNITUDE_BITS) < FLOAT32_SMALLEST_NORMAL_BITS;
}

// Nonnegative weights below WeightedReservoirMinimumWeight count as zero.
bool IsBelowMinimumWeightBits(uint32 bits) noexcept
{
    return (bits & FLOAT32_MAGNITUDE_BITS) < MINIMUM_WEIGHT_BITS;
}
} // namespace

MathStatus
TryAddReservoirCandidate(ReservoirCandidate candidate, WeightedReservoir& inoutReservoir, bool& outIsSelected) noexcept
{
    const uint32 weightBits = GetBits(candidate.Weight);
    const uint32 ticketBits = GetBits(candidate.Ticket);
    if (IsFiniteBits(weightBits) == false || IsFiniteBits(ticketBits) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (IsNegativeBits(weightBits) == true || IsNegativeBits(ticketBits) == true ||
        (ticketBits & FLOAT32_MAGNITUDE_BITS) >= FLOAT32_ONE_BITS)
    {
        return MathStatus::InvalidArgument;
    }
    if (inoutReservoir.mCandidateCount == MAX_CANDIDATE_COUNT)
    {
        return MathStatus::OutOfRange;
    }
    const float32 weight = IsBelowMinimumWeightBits(weightBits) == true ? 0.0f : candidate.Weight;
    const float32 ticket = IsZeroOrSubnormalBits(ticketBits) == true ? 0.0f : candidate.Ticket;
    const float32 weightSum = inoutReservoir.mWeightSum + weight;
    if (IsFiniteBits(GetBits(weightSum)) == false)
    {
        return MathStatus::OutOfRange;
    }
    // A zero weight never satisfies the strict comparison. For the first
    // positive weight w >= 2^-102, the exact ticket * w <= w - w * 2^-24 is
    // normal and far enough below w that it rounds below w: that candidate is
    // always selected. A product that is subnormal or flushed is
    // below 2^-126 and therefore below any accepted weight on every backend.
    const bool isSelected = ticket * weightSum < weight;
    if (isSelected == true)
    {
        inoutReservoir.mSelectedIndex = inoutReservoir.mCandidateCount;
    }
    inoutReservoir.mWeightSum = weightSum;
    ++inoutReservoir.mCandidateCount;
    outIsSelected = isSelected;
    return MathStatus::Success;
}

MathStatus TryComputeReservoirContributionWeight(const WeightedReservoir& reservoir,
                                                 float32 selectedTarget,
                                                 float32& outWeight) noexcept
{
    if (reservoir.HasSelection() == false)
    {
        outWeight = 0.0f;
        return MathStatus::Success;
    }
    const uint32 targetBits = GetBits(selectedTarget);
    if (IsFiniteBits(targetBits) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (IsNegativeBits(targetBits) == true || IsZeroOrSubnormalBits(targetBits) == true)
    {
        return MathStatus::InvalidArgument;
    }
    const float32 denominator = static_cast<float32>(reservoir.GetCandidateCount()) * selectedTarget;
    if (IsFiniteBits(GetBits(denominator)) == false)
    {
        return MathStatus::OutOfRange;
    }
    const float32 weight = reservoir.GetWeightSum() / denominator;
    const uint32 weightBits = GetBits(weight);
    if (IsFiniteBits(weightBits) == false)
    {
        return MathStatus::OutOfRange;
    }
    outWeight = IsZeroOrSubnormalBits(weightBits) == true ? 0.0f : weight;
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
