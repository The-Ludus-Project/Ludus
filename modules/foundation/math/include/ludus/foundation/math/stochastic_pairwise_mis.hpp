#pragma once
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/status.hpp>

// Thanks to Trevor Hedstrom, Markus Kettunen, Daqi Lin, Chris Wyman and Tzu-Mao Li,
// "Stochastic Pairwise MIS for Unbiased Large-Kernel Reuse in Real-Time",
// Computer Graphics Forum 45(2), 2026, equations 10/11/14/15/18/19 and section 4.3:
// https://doi.org/10.1111/cgf.70391
// Independently implemented scalar arithmetic, with caller-owned samples, shifts,
// probability distributions and storage. See docs/architecture/randomness.md.

namespace ludus::foundation::math
{
using core::float32;
using core::uint32;

/// Version of the shared stochastic pairwise MIS arithmetic and input contracts.
inline constexpr uint32 STOCHASTIC_PAIRWISE_MIS_VERSION = 1;

/// Compute the defensive neighbor-confidence scale min(drawCount / neighborCount, 1).
/// Zero neighbors produces zero; drawCount must be positive. Apply this one scale to every
/// neighbor confidence and to their sum before computing either canonical or neighbor MIS.
/// @param drawCount Positive number of independent with-replacement draws.
/// @param neighborCount Number of noncanonical proposals in the reuse set; may be zero.
/// @param outScale Written on success; preserved on failure.
/// @return Success or InvalidArgument for zero draws. No allocation or thread affinity.
[[nodiscard]] MathStatus
TryComputePairwiseConfidenceScale(uint32 drawCount, uint32 neighborCount, float32& outScale) noexcept;

/// Compute the defensive canonical base weight c_c / (c_sum + c_c), equation 18.
/// Confidences are finite, nonnegative and in common arbitrary units; canonicalConfidence
/// must be positive. confidenceSum is the sum of scaled noncanonical confidences.
/// @param confidenceSum Sum of all scaled noncanonical confidences; finite and nonnegative.
/// @param canonicalConfidence Finite positive confidence of the destination proposal.
/// @param outWeight Written on success; preserved on failure.
/// @return Success, NonFiniteInput, or InvalidArgument. No allocation or thread affinity.
[[nodiscard]] MathStatus
TryComputePairwiseCanonicalBase(float32 confidenceSum, float32 canonicalConfidence, float32& outWeight) noexcept;

/// Compute a deterministic defensive noncanonical pairwise MIS weight, equation 11a.
/// Targets must be finite, nonnegative densities in the destination measure at the same
/// shifted sample. sourceTargetInDestinationMeasure is p_i(x_i) / abs(J_i_to_c), including
/// source proposal support; it is NOT the untransformed source target for non-unit shifts.
/// Confidences must be finite, 0 <= neighborConfidence <= confidenceSum and
/// canonicalConfidence > 0. Zero neighbor confidence or two zero targets yields zero.
/// @param neighborConfidence Scaled confidence of this noncanonical proposal in common confidence units.
/// @param confidenceSum Sum of all scaled noncanonical confidences; finite and nonnegative.
/// @param canonicalConfidence Finite positive confidence of the destination proposal.
/// @param sourceTargetInDestinationMeasure Finite nonnegative source target divided by its forward absolute Jacobian,
/// including inverse proposal support.
/// @param destinationTarget Finite nonnegative destination density at this same shifted sample.
/// @param outWeight Written on success; preserved on failure.
/// @return Success, NonFiniteInput, or InvalidArgument. No allocation or thread affinity.
/// @note Callers own shifts, support tests and valid input contribution weights. The formula
/// alone does not establish an unbiased estimator. FP32 rounding/subnormal flushing applies.
[[nodiscard]] MathStatus TryComputePairwiseNeighborMis(float32 neighborConfidence,
                                                       float32 confidenceSum,
                                                       float32 canonicalConfidence,
                                                       float32 sourceTargetInDestinationMeasure,
                                                       float32 destinationTarget,
                                                       float32& outWeight) noexcept;

/// Compute one canonical pairwise correction beta_i, equation 19, at the canonical sample.
/// Inputs have the same domains and common-measure contract as TryComputePairwiseNeighborMis.
/// For a neighbor unable to propose the canonical sample, pass a zero source target.
/// Add beta_i / (canonicalDrawCount * canonicalSelectionPmf) per independently drawn
/// neighbor to TryComputePairwiseCanonicalBase; repeated draws retain multiplicity.
/// @param neighborConfidence Scaled confidence of this noncanonical proposal in common confidence units.
/// @param confidenceSum Sum of all scaled noncanonical confidences; finite and nonnegative.
/// @param canonicalConfidence Finite positive confidence of the destination proposal.
/// @param sourceTargetInDestinationMeasure Finite nonnegative source target divided by its forward absolute Jacobian,
/// including inverse proposal support.
/// @param destinationTarget Finite nonnegative destination density at this same shifted sample.
/// @param outWeight Written on success; preserved on failure.
/// @return Success, NonFiniteInput, or InvalidArgument. No allocation or thread affinity.
[[nodiscard]] MathStatus TryComputePairwiseCanonicalTerm(float32 neighborConfidence,
                                                         float32 confidenceSum,
                                                         float32 canonicalConfidence,
                                                         float32 sourceTargetInDestinationMeasure,
                                                         float32 destinationTarget,
                                                         float32& outWeight) noexcept;

/// Correct one with-replacement draw by its actual selection probability, equation 15.
/// Returns misWeight / (drawCount * selectionPmf). misWeight is finite and nonnegative;
/// selectionPmf is finite in (0,1] and drawCount is positive. Stream each occurrence or
/// multiply by its multiplicity. A proposal must have positive probability wherever its
/// MIS-weighted contribution can be nonzero. Draw coins must be fresh conditional on inputs.
/// @param misWeight Finite nonnegative MIS weight.
/// @param selectionPmf Actual probability of this drawn proposal in (0,1].
/// @param drawCount Positive number of independent with-replacement draws.
/// @param outWeight Written on success; preserved on failure.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange for overflow.
/// @note No allocation or thread affinity; probability/ticket quantization is caller-owned.
[[nodiscard]] MathStatus
TryComputeStochasticMisWeight(float32 misWeight, float32 selectionPmf, uint32 drawCount, float32& outWeight) noexcept;

/// Express a source target in the destination measure: p_i(x) / abs(J_i_to_c), equation 10.
/// Inputs are finite and nonnegative. A zero Jacobian denotes a failed partial shift and
/// produces zero support. Callers must also zero support for impossible inverse proposals.
/// @param sourceTarget Finite nonnegative density in the source measure.
/// @param forwardJacobian Finite nonnegative absolute Jacobian from source to destination; zero denotes failed shift.
/// @param outTarget Written on success; preserved on failure.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange for overflow.
/// No allocation or thread affinity.
[[nodiscard]] MathStatus
TryComputePairwiseShiftedTarget(float32 sourceTarget, float32 forwardJacobian, float32& outTarget) noexcept;

/// Compute a shifted GRIS resampling weight m * p_c(y) * W_i * abs(J_i_to_c), equation 14.
/// All inputs must be finite and nonnegative. misWeight normally includes the stochastic
/// correction; sourceContributionWeight is a valid input UCW, not a streaming weight sum.
/// forwardJacobian is zero for a failed shift. The caller supplies visibility/support policy.
/// @param misWeight Finite nonnegative MIS weight, stochastically corrected when used for resampling.
/// @param destinationTarget Finite nonnegative destination density at this same shifted sample.
/// @param sourceContributionWeight Finite nonnegative input UCW in the source measure.
/// @param forwardJacobian Finite nonnegative absolute Jacobian from source to destination; zero denotes failed shift.
/// @param outWeight Written on success; preserved on failure.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange for FP32 intermediate overflow.
/// No allocation or thread affinity.
[[nodiscard]] MathStatus TryComputePairwiseResamplingWeight(float32 misWeight,
                                                            float32 destinationTarget,
                                                            float32 sourceContributionWeight,
                                                            float32 forwardJacobian,
                                                            float32& outWeight) noexcept;

/// Finalize pairwise GRIS with W = weightSum / selectedTarget, equation 7, WITHOUT dividing by M.
/// weightSum must be finite and nonnegative. An empty sum yields zero without examining
/// selectedTarget; otherwise selectedTarget must be finite and positive, in the destination
/// measure and identical to the target used for the selected candidate's resampling weight.
/// @param weightSum Finite nonnegative sum of streamed candidate resampling weights.
/// @param selectedTarget Selected density in the destination measure; positive for a nonempty sum.
/// @param outWeight Written on success; preserved on failure.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange for overflow.
/// No allocation or thread affinity.
[[nodiscard]] MathStatus
TryComputePairwiseContributionWeight(float32 weightSum, float32 selectedTarget, float32& outWeight) noexcept;

} // namespace ludus::foundation::math
