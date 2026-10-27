#pragma once

// Sequential weighted reservoir v1 for kernels: a port of FoundationMath's
// ludus/foundation/math/weighted_reservoir.hpp with the same selections, status
// classes and weight sums. docs/architecture/randomness.md#sequential-weighted-reservoir
// owns the contract; tests/sdk_consumer/random_parity_corpus.h compares this code
// with the CPU reference on every GPU backend.
// With thanks to Chris Wyman, "Weighted Reservoir Sampling: Randomly Sampling
// Streams", Ray Tracing Gems II, chapter 22, printed pages 345-349 (2021), for the
// single-sample, with-replacement streaming update; no chapter code is copied.
// Metal compiles with fast math and GPUs may flush subnormals, so weights and
// tickets are classified on their bits, never through isfinite or NaN compares.
static const uint WEIGHTED_RESERVOIR_SELECTION_VERSION = 1;
// Bits of WeightedReservoirMinimumWeight, 2^-102: smaller weights count as zero,
// so ticket * sum never loses precision near an accepted weight.
static const uint WEIGHTED_RESERVOIR_MINIMUM_WEIGHT_BITS = 0x0c800000;
// IEEE binary32 fields, as named in the CPU implementation.
static const uint RESERVOIR_FLOAT_SIGN_BIT = 0x80000000;
static const uint RESERVOIR_FLOAT_MAGNITUDE_BITS = 0x7fffffff;
static const uint RESERVOIR_FLOAT_EXPONENT_BITS = 0x7f800000;
static const uint RESERVOIR_FLOAT_SMALLEST_NORMAL_BITS = 0x00800000;
static const uint RESERVOIR_FLOAT_ONE_BITS = 0x3f800000;
static const uint RESERVOIR_MAX_CANDIDATE_COUNT = 0xffffffff;

// CPU WeightedReservoir; MakeWeightedReservoir() is the empty state. Treat the
// fields as read-only and update them only through TryAddReservoirCandidate.
struct WeightedReservoir
{
    float WeightSum;
    uint CandidateCount;
    uint SelectedIndex;
};

WeightedReservoir MakeWeightedReservoir()
{
    WeightedReservoir reservoir;
    reservoir.WeightSum = 0;
    reservoir.CandidateCount = 0;
    reservoir.SelectedIndex = 0;
    return reservoir;
}

// Empty exactly while the weight sum is zero; a zero total contributes zero.
bool HasReservoirSelection(WeightedReservoir reservoir)
{
    return reservoir.WeightSum > 0;
}

bool IsReservoirFiniteBits(uint bits)
{
    return (bits & RESERVOIR_FLOAT_EXPONENT_BITS) != RESERVOIR_FLOAT_EXPONENT_BITS;
}

// Strictly negative: -0 is zero, not negative.
bool IsReservoirNegativeBits(uint bits)
{
    return (bits & RESERVOIR_FLOAT_SIGN_BIT) != 0 && (bits & RESERVOIR_FLOAT_MAGNITUDE_BITS) != 0;
}

// +-0 and every subnormal count as zero.
bool IsReservoirZeroOrSubnormalBits(uint bits)
{
    return (bits & RESERVOIR_FLOAT_MAGNITUDE_BITS) < RESERVOIR_FLOAT_SMALLEST_NORMAL_BITS;
}

bool IsReservoirBelowMinimumWeightBits(uint bits)
{
    return (bits & RESERVOIR_FLOAT_MAGNITUDE_BITS) < WEIGHTED_RESERVOIR_MINIMUM_WEIGHT_BITS;
}

// CPU TryAddReservoirCandidate: streams the candidate at position
// CandidateCount, adds its weight and selects it when ticket * sum < weight.
// Returns false, leaving the reservoir unchanged and outIsSelected false, for a
// nonfinite, negative or (ticket) out-of-[0,1) input, a weight-sum overflow or
// a full candidate count; callers must branch on the result. The CPU status code
// distinguishes those failures.
bool TryAddReservoirCandidate(inout WeightedReservoir inoutReservoir,
                              float weight,
                              float ticket,
                              out bool outIsSelected)
{
    outIsSelected = false;
    uint weightBits = asuint(weight);
    uint ticketBits = asuint(ticket);
    if (IsReservoirFiniteBits(weightBits) == false || IsReservoirFiniteBits(ticketBits) == false)
        return false;
    if (IsReservoirNegativeBits(weightBits) == true || IsReservoirNegativeBits(ticketBits) == true ||
        (ticketBits & RESERVOIR_FLOAT_MAGNITUDE_BITS) >= RESERVOIR_FLOAT_ONE_BITS)
        return false;
    if (inoutReservoir.CandidateCount == RESERVOIR_MAX_CANDIDATE_COUNT)
        return false;
    float acceptedWeight = IsReservoirBelowMinimumWeightBits(weightBits) == true ? 0 : weight;
    float acceptedTicket = IsReservoirZeroOrSubnormalBits(ticketBits) == true ? 0 : ticket;
    float weightSum = inoutReservoir.WeightSum + acceptedWeight;
    if (IsReservoirFiniteBits(asuint(weightSum)) == false)
        return false;
    bool isSelected = acceptedTicket * weightSum < acceptedWeight;
    if (isSelected == true)
        inoutReservoir.SelectedIndex = inoutReservoir.CandidateCount;
    inoutReservoir.WeightSum = weightSum;
    inoutReservoir.CandidateCount += 1;
    outIsSelected = isSelected;
    return true;
}

// CPU TryComputeReservoirContributionWeight: W = WeightSum / (CandidateCount *
// selectedTarget). An empty reservoir yields true with outWeight zero. Returns
// false, with outWeight zero, for a nonfinite, nonpositive or subnormal target
// or a nonfinite result. The division follows the backend's FP32 division
// accuracy, which may differ from the CPU's correctly rounded quotient.
bool TryComputeReservoirContributionWeight(WeightedReservoir reservoir, float selectedTarget, out float outWeight)
{
    outWeight = 0;
    if (HasReservoirSelection(reservoir) == false)
        return true;
    uint targetBits = asuint(selectedTarget);
    if (IsReservoirFiniteBits(targetBits) == false || IsReservoirNegativeBits(targetBits) == true ||
        IsReservoirZeroOrSubnormalBits(targetBits) == true)
        return false;
    float denominator = float(reservoir.CandidateCount) * selectedTarget;
    if (IsReservoirFiniteBits(asuint(denominator)) == false)
        return false;
    float weight = reservoir.WeightSum / denominator;
    uint weightBits = asuint(weight);
    if (IsReservoirFiniteBits(weightBits) == false)
        return false;
    outWeight = IsReservoirZeroOrSubnormalBits(weightBits) == true ? 0 : weight;
    return true;
}
