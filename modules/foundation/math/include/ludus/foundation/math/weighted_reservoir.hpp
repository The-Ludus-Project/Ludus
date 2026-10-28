#pragma once

// Sequential weighted reservoir for resampled importance sampling (RIS).
// With thanks to Chris Wyman, "Weighted Reservoir Sampling: Randomly Sampling
// Streams", Ray Tracing Gems II, chapter 22, printed pages 345-349 (2021). We
// adopt its single-sample, with-replacement streaming update over an explicit
// candidate order: w_sum += w, then select when ticket * w_sum < w. No chapter
// code is copied. Bit-level weight/ticket validation, the minimum weight and
// the explicit empty result are Ludus contracts shared with the GPU port in
// FoundationMath's shaders/weighted_reservoir.slang. Review:
// docs/architecture/randomness-gems-review.md#streaming-selection; contract:
// docs/architecture/randomness.md#sequential-weighted-reservoir.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/status.hpp>

namespace ludus::foundation::math
{
using core::float32;
using core::uint32;

/// @brief Frozen identifier of the reservoir's selection arithmetic, for replay/content manifests.
///
/// Changing the selection rule, weight/ticket classification, subnormal flush or contribution-weight formula
/// requires a new version. FoundationMath's shaders/weighted_reservoir.slang mirrors it.
inline constexpr uint32 WeightedReservoirSelectionVersion = 1;

/// @brief Smallest weight the reservoir accepts as positive, 2^-102 = 2^24 times FP32's smallest normal value.
///
/// Smaller nonnegative weights, subnormals included, count as zero. Above it, Ticket * sum stays normal wherever it
/// can approach the weight, so FP32 rounding and GPU subnormal flushing cannot change a selection, and the first
/// positive candidate is always selected. The bias is at most this value per candidate.
inline constexpr float32 WeightedReservoirMinimumWeight = 0x1p-102f;

/// @brief One streamed candidate: its resampling weight and its own uniform ticket.
///
/// Named fields keep the weight and ticket from being exchanged.
struct ReservoirCandidate
{
    /// @brief Resampling weight, for RIS target / source density; finite and nonnegative.
    ///
    /// Weights below WeightedReservoirMinimumWeight count as zero.
    float32 Weight = 0.0f;
    /// @brief Uniform ticket in [0,1), normally SampleFloat01 at a dimension reserved for this candidate alone.
    float32 Ticket = 0.0f;
};

/// @brief Single-sample, with-replacement weighted reservoir over a caller-fixed candidate order.
///
/// A default-constructed reservoir is empty. Stream every candidate, including zero-weight ones, through
/// TryAddReservoirCandidate in the same order on every evaluation: the outcome depends on that order and on each
/// candidate's ticket, not on an address alone. The reservoir holds no candidate payload; record the payload of a
/// candidate when the update reports it as selected. Plain value type with no allocation or thread affinity.
class WeightedReservoir
{
public:
    /// @brief Sum of the weights streamed so far, after the minimum-weight flush; zero when empty.
    [[nodiscard]] LUDUS_INLINE float32 GetWeightSum() const noexcept
    {
        return mWeightSum;
    }
    /// @brief Number of candidates streamed so far, zero-weight candidates included (RIS M).
    [[nodiscard]] LUDUS_INLINE uint32 GetCandidateCount() const noexcept
    {
        return mCandidateCount;
    }
    /// @brief Zero-based stream position of the selected candidate; zero when HasSelection() is false.
    [[nodiscard]] LUDUS_INLINE uint32 GetSelectedIndex() const noexcept
    {
        return mSelectedIndex;
    }
    /// @brief True once any candidate with a weight of at least WeightedReservoirMinimumWeight has been streamed.
    ///
    /// An empty reservoir after the whole stream (zero total weight) is a valid outcome that contributes zero, not
    /// an invalid sample.
    [[nodiscard]] LUDUS_INLINE bool HasSelection() const noexcept
    {
        return mWeightSum > 0.0f;
    }

private:
    friend MathStatus TryAddReservoirCandidate(ReservoirCandidate candidate,
                                               WeightedReservoir& inoutReservoir,
                                               bool& outIsSelected) noexcept;
    float32 mWeightSum = 0.0f;
    uint32 mCandidateCount = 0;
    uint32 mSelectedIndex = 0;
};

/// @brief Streams the next candidate: adds its weight to the sum and selects it when Ticket * sum < Weight.
///
/// A zero-weight candidate is counted but never selected; weights below WeightedReservoirMinimumWeight count as zero.
/// The first candidate with a positive weight is always selected, so the reservoir is empty exactly while the sum is
/// zero. A subnormal ticket counts as zero, because GPU arithmetic may flush it. Weight and ticket are classified from
/// their bit patterns, so -0 is a zero weight and a valid zero ticket.
/// Acceptance is quantized by the ticket's resolution (2^-24 for SampleFloat01), a declared approximation.
/// @param candidate Weight and ticket of the candidate at stream position GetCandidateCount().
/// @param inoutReservoir Reservoir updated on success and left unchanged on failure.
/// @param outIsSelected Written on success: true when this candidate replaced the selection.
/// @return Success; NonFiniteInput for a NaN or infinite weight or ticket; InvalidArgument for a negative weight or
///         a ticket outside [0,1); OutOfRange when the weight sum would overflow to infinity or the candidate count
///         would exceed 2^32 - 1. Failure leaves both outputs unchanged.
[[nodiscard]] MathStatus
TryAddReservoirCandidate(ReservoirCandidate candidate, WeightedReservoir& inoutReservoir, bool& outIsSelected) noexcept;

/// @brief Computes the RIS contribution weight W = w_sum / (M * target(selected)) of a finished stream.
///
/// For any integrand g that is zero wherever the target is zero, E[W * g(Y)] equals the sum (or integral) of g.
/// An empty reservoir is a valid zero contribution: outWeight becomes 0 and selectedTarget is not examined. The
/// division is FP32; GPU backends may round it differently within their division accuracy. A subnormal result is
/// flushed to zero, as GPU arithmetic may do.
/// @param reservoir Reservoir after the caller streamed all of its candidates.
/// @param selectedTarget Unnormalized target density of the selected candidate, as used in its weight. It must be a
///        finite, positive, normal FP32 value when the reservoir has a selection.
/// @param outWeight Written on success with W, or with zero for an empty reservoir.
/// @return Success; NonFiniteInput for a NaN or infinite target; InvalidArgument for a zero, negative or subnormal
///         target; OutOfRange when M * target or the quotient is not finite. Failure leaves outWeight unchanged.
[[nodiscard]] MathStatus TryComputeReservoirContributionWeight(const WeightedReservoir& reservoir,
                                                               float32 selectedTarget,
                                                               float32& outWeight) noexcept;
} // namespace ludus::foundation::math
