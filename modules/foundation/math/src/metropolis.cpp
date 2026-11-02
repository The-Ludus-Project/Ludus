#include <ludus/foundation/math/metropolis.hpp>

#include <ludus/foundation/math/scalar.hpp>

// Thanks to Sascha Holl, Gurprit Singh and Hans-Peter Seidel, "Rao-Blackwellized
// Markov chain Monte Carlo Light Transport", SIGGRAPH 2026, section 5.4 and
// Algorithm 5.2, https://arxiv.org/abs/2605.09117 . Adapt the rejection-product
// recurrence, including the accepting proposal. The weighted ratio and symmetric
// toroidal primary-sample mutation are our bounded integration contracts.
namespace ludus::foundation::math
{
namespace
{
constexpr float32 MINIMUM_NORMAL = 0x1p-126F;
bool IsNonnegative(Vector3 value) noexcept
{
    return value.X >= 0 && value.Y >= 0 && value.Z >= 0;
}
bool IsNonnegative(Vector4 value) noexcept
{
    return value.X >= 0 && value.Y >= 0 && value.Z >= 0 && value.W >= 0;
}
} // namespace

MathStatus
TryComputeSymmetricMetropolisAcceptance(float32 currentTarget, float32 proposedTarget, float32& outAcceptance) noexcept
{
    if (IsFinite(currentTarget) == false || IsFinite(proposedTarget) == false)
        return MathStatus::NonFiniteInput;
    if (currentTarget < MINIMUM_NORMAL || proposedTarget < MINIMUM_NORMAL)
        return MathStatus::InvalidArgument;
    outAcceptance = proposedTarget >= currentTarget ? 1 : proposedTarget / currentTarget;
    return MathStatus::Success;
}

MathStatus TryMutateMetropolisPrimary(float32 current,
                                      float32 ticket,
                                      float32 radius,
                                      bool isLargeStep,
                                      float32& outSample) noexcept
{
    if (IsFinite(current) == false || IsFinite(ticket) == false || IsFinite(radius) == false)
        return MathStatus::NonFiniteInput;
    if (current < 0 || current >= 1 || ticket < 0 || ticket >= 1 || radius <= 0 || radius > 0.5F)
        return MathStatus::InvalidArgument;
    float32 sample = isLargeStep == true ? ticket : current + (2 * ticket - 1) * radius;
    if (sample < 0)
        sample += 1;
    if (sample >= 1)
        sample -= 1;
    outSample = sample;
    return MathStatus::Success;
}

// Thanks to Holl, Singh and Seidel, "Rao-Blackwellized Markov chain Monte Carlo
// Light Transport", SIGGRAPH 2026, sections 6.1/6.3 and Appendix C,
// https://arxiv.org/abs/2605.09117 . Sample the exponential race through its
// independent event identity and minimum time, equivalent to Algorithm 6.1.
MathStatus TryComputeJumpRestoreRates(float32 target, float32 restorationScale, JumpRestoreRates& outRates) noexcept
{
    if (IsFinite(target) == false || IsFinite(restorationScale) == false)
        return MathStatus::NonFiniteInput;
    if (target < MINIMUM_NORMAL || restorationScale < MINIMUM_NORMAL)
        return MathStatus::InvalidArgument;
    JumpRestoreRates rates;
    if (target >= restorationScale)
    {
        const float32 ratio = restorationScale / target;
        rates.MeanHoldingTime = 1 / (1 + ratio);
        rates.RestoreProbability = ratio * rates.MeanHoldingTime;
    }
    else
    {
        const float32 ratio = target / restorationScale;
        rates.RestoreProbability = 1 / (1 + ratio);
        rates.MeanHoldingTime = ratio * rates.RestoreProbability;
    }
    if (rates.MeanHoldingTime <= 0 || rates.RestoreProbability <= 0)
        return MathStatus::OutOfRange;
    outRates = rates;
    return MathStatus::Success;
}

MathStatus TrySampleJumpRestoreHoldingTime(float32 ticket, float32 meanHoldingTime, float32& outTime) noexcept
{
    if (IsFinite(ticket) == false || IsFinite(meanHoldingTime) == false)
        return MathStatus::NonFiniteInput;
    if (ticket < 0 || ticket >= 1 || meanHoldingTime <= 0 || meanHoldingTime > 1)
        return MathStatus::InvalidArgument;
    const float32 time = static_cast<float32>(-Log(static_cast<float64>(1 - ticket))) * meanHoldingTime;
    if (IsFinite(time) == false)
        return MathStatus::OutOfRange;
    outTime = time;
    return MathStatus::Success;
}

MathStatus TryAdvanceMetropolisTour(float32 acceptance, MetropolisTour& inoutTour) noexcept
{
    if (IsFinite(acceptance) == false || IsFinite(inoutTour.RejectionProduct) == false ||
        IsFinite(inoutTour.HoldingWeight) == false)
        return MathStatus::NonFiniteInput;
    if (acceptance < 0 || acceptance > 1 || inoutTour.RejectionProduct < 0 || inoutTour.RejectionProduct > 1 ||
        inoutTour.HoldingWeight < 1)
        return MathStatus::InvalidArgument;
    MetropolisTour candidate = inoutTour;
    candidate.RejectionProduct *= 1 - acceptance;
    candidate.HoldingWeight += candidate.RejectionProduct;
    if (IsFinite(candidate.HoldingWeight) == false)
        return MathStatus::OutOfRange;
    inoutTour = candidate;
    return MathStatus::Success;
}

MathStatus TryAddWeightedRadiance(Vector3 radiance, float32 weight, WeightedRadiance& inoutAccumulation) noexcept
{
    if (IsFinite(radiance) == false || IsFinite(weight) == false || IsFinite(inoutAccumulation.Sum) == false ||
        IsFinite(inoutAccumulation.Compensation) == false)
        return MathStatus::NonFiniteInput;
    if (IsNonnegative(radiance) == false || weight < 0 || IsNonnegative(inoutAccumulation.Sum) == false)
        return MathStatus::InvalidArgument;
    if (weight == 0)
        return MathStatus::Success;
    const Vector4 contribution{radiance.X * weight, radiance.Y * weight, radiance.Z * weight, weight};
    const Vector4 adjusted = contribution - inoutAccumulation.Compensation;
    const Vector4 combined = inoutAccumulation.Sum + adjusted;
    const Vector4 compensation = (combined - inoutAccumulation.Sum) - adjusted;
    if (IsFinite(contribution) == false || IsFinite(combined) == false || IsFinite(compensation) == false ||
        IsNonnegative(combined) == false)
        return MathStatus::OutOfRange;
    inoutAccumulation.Sum = combined;
    inoutAccumulation.Compensation = compensation;
    return MathStatus::Success;
}

MathStatus TryResolveWeightedRadiance(const WeightedRadiance& accumulation, Vector3& outRadiance) noexcept
{
    if (IsFinite(accumulation.Sum) == false || IsFinite(accumulation.Compensation) == false)
        return MathStatus::NonFiniteInput;
    if (IsNonnegative(accumulation.Sum) == false)
        return MathStatus::InvalidArgument;
    if (accumulation.Sum.W == 0 && (accumulation.Sum.X != 0 || accumulation.Sum.Y != 0 || accumulation.Sum.Z != 0))
        return MathStatus::InvalidArgument;
    const Vector3 numerator{accumulation.Sum.X, accumulation.Sum.Y, accumulation.Sum.Z};
    const Vector3 result = accumulation.Sum.W > 0 ? numerator / accumulation.Sum.W : Vector3{};
    if (IsFinite(result) == false)
        return MathStatus::OutOfRange;
    outRadiance = result;
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
