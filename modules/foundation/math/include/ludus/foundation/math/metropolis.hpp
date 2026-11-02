#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math
{
/// Version of the symmetric primary-sample mutations and RB holding-time recurrence.
inline constexpr uint32 METROPOLIS_VERSION = 1;

/// One unfinished accepted-state tour of Holl, Singh and Seidel (2026), Algorithm 5.2.
/// Preserve both fields across dispatches and reset to the default only after acceptance.
struct MetropolisTour final
{
    /// Product of rejection probabilities for proposals at the current state.
    float32 RejectionProduct = 1;
    /// Estimated holding time, including the initial visit and the accepting proposal.
    float32 HoldingWeight = 1;
};

/// Compensated RGB numerator and scalar denominator of a weighted radiance estimate.
/// No allocation or thread affinity. Each chain/pixel owns its accumulator exclusively.
struct WeightedRadiance final
{
    /// XYZ is sum(weight * radiance), W is sum(weight).
    Vector4 Sum{};
    /// Kahan compensation for the four sums; not another contribution.
    Vector4 Compensation{};
};

/// Compute min(1, proposedTarget/currentTarget) for a symmetric proposal kernel.
/// Both unnormalized targets must be finite, positive normal FP32 values. This
/// restriction preserves support even at zero-radiance paths. Failure preserves outAcceptance.
/// @return Success, NonFiniteInput, or InvalidArgument for a nonpositive/subnormal target.
[[nodiscard]] MathStatus
TryComputeSymmetricMetropolisAcceptance(float32 currentTarget, float32 proposedTarget, float32& outAcceptance) noexcept;

/// Jump Restore event law for unit-rate local MH dynamics and uniform restorations.
/// Killing rate is restorationScale/target, with the target normalization absorbed
/// into the positive scale (Holl et al. 2026, section 6 and Appendix C).
struct JumpRestoreRates final
{
    /// Probability that the next event restores a new uniform primary state.
    float32 RestoreProbability = 0;
    /// Conditional mean holding time, 1/(1 + restorationScale/target), in local-clock units.
    float32 MeanHoldingTime = 0;
};

/// Compute the competing-event probabilities without forming an overflowing rate.
/// Both inputs must be finite, positive normal FP32 values. No allocation/thread affinity.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange if the mean underflows;
/// failure preserves outRates.
[[nodiscard]] MathStatus
TryComputeJumpRestoreRates(float32 target, float32 restorationScale, JumpRestoreRates& outRates) noexcept;

/// Sample Exp(1/meanHoldingTime) with an independent uniform ticket in [0,1).
/// meanHoldingTime must be finite and in (0,1], matching the unit local-rate event law.
/// Uses -log(1-ticket)*meanHoldingTime; a zero ticket gives zero time, never infinity.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange; failure preserves outTime.
[[nodiscard]] MathStatus
TrySampleJumpRestoreHoldingTime(float32 ticket, float32 meanHoldingTime, float32& outTime) noexcept;

/// Wrap a symmetric uniform small step on [0,1), or return ticket for a large step.
/// Inputs are finite, current/ticket in [0,1), and radius in (0,0.5]. Large-step choice
/// must be independent of the state. The mixture then needs no proposal-density ratio.
/// @return Success, NonFiniteInput or InvalidArgument; failure preserves outSample.
[[nodiscard]] MathStatus TryMutateMetropolisPrimary(float32 current,
                                                    float32 ticket,
                                                    float32 radius,
                                                    bool isLargeStep,
                                                    float32& outSample) noexcept;

/// Include one proposal in the paper's truncated holding-time estimate:
/// P *= 1 - acceptance; holding += P. Call before testing that proposal's acceptance.
/// The accepting proposal is included. This practical truncation is biased; see
/// Holl et al., section 5.4. No extra proposals are generated.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange. Failure preserves the tour.
[[nodiscard]] MathStatus TryAdvanceMetropolisTour(float32 acceptance, MetropolisTour& inoutTour) noexcept;

/// Add one finite, nonnegative RGB sample with a finite, nonnegative weight.
/// A zero weight is a successful no-op. Computes compensated FP32 numerator and
/// denominator transactionally; negative samples, invalid state and overflow reject.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange; failure preserves the accumulator.
[[nodiscard]] MathStatus
TryAddWeightedRadiance(Vector3 radiance, float32 weight, WeightedRadiance& inoutAccumulation) noexcept;

/// Resolve numerator/denominator, returning black for an empty accumulator.
/// @return Success, NonFiniteInput, InvalidArgument or OutOfRange; failure preserves outRadiance.
[[nodiscard]] MathStatus TryResolveWeightedRadiance(const WeightedRadiance& accumulation,
                                                    Vector3& outRadiance) noexcept;
} // namespace ludus::foundation::math
