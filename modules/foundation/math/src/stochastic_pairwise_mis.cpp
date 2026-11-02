#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/stochastic_pairwise_mis.hpp>

// Thanks to Trevor Hedstrom, Markus Kettunen, Daqi Lin, Chris Wyman and Tzu-Mao Li,
// "Stochastic Pairwise MIS for Unbiased Large-Kernel Reuse in Real-Time",
// Computer Graphics Forum 45(2), 2026, equations 10/11/14/15/18/19 and section 4.3:
// https://doi.org/10.1111/cgf.70391
// Independently implemented scalar arithmetic, with caller-owned samples, shifts,
// probability distributions and storage. See docs/architecture/randomness.md.

namespace ludus::foundation::math
{
namespace
{
MathStatus ValidatePairwiseInputs(float32 neighborConfidence,
                                  float32 confidenceSum,
                                  float32 canonicalConfidence,
                                  float32 sourceTarget,
                                  float32 destinationTarget) noexcept
{
    if (IsFinite(neighborConfidence) == false || IsFinite(confidenceSum) == false ||
        IsFinite(canonicalConfidence) == false || IsFinite(sourceTarget) == false ||
        IsFinite(destinationTarget) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (neighborConfidence < 0 || neighborConfidence > confidenceSum || canonicalConfidence <= 0 || sourceTarget < 0 ||
        destinationTarget < 0)
    {
        return MathStatus::InvalidArgument;
    }
    return MathStatus::Success;
}
} // namespace

MathStatus TryComputePairwiseConfidenceScale(uint32 drawCount, uint32 neighborCount, float32& outScale) noexcept
{
    if (drawCount == 0)
    {
        return MathStatus::InvalidArgument;
    }
    float32 value = neighborCount == 0 ? 0
                                       : Min(static_cast<float32>(drawCount) / static_cast<float32>(neighborCount),
                                             static_cast<float32>(1));
    outScale = value;
    return MathStatus::Success;
}

MathStatus
TryComputePairwiseCanonicalBase(float32 confidenceSum, float32 canonicalConfidence, float32& outWeight) noexcept
{
    const MathStatus status = ValidatePairwiseInputs(0, confidenceSum, canonicalConfidence, 0, 0);
    if (status != MathStatus::Success)
    {
        return status;
    }
    float32 scale = Max(confidenceSum, canonicalConfidence);
    float32 value = (canonicalConfidence / scale) / (confidenceSum / scale + canonicalConfidence / scale);
    outWeight = value;
    return MathStatus::Success;
}

MathStatus TryComputePairwiseNeighborMis(float32 neighborConfidence,
                                         float32 confidenceSum,
                                         float32 canonicalConfidence,
                                         float32 sourceTargetInDestinationMeasure,
                                         float32 destinationTarget,
                                         float32& outWeight) noexcept
{
    const MathStatus status = ValidatePairwiseInputs(neighborConfidence,
                                                     confidenceSum,
                                                     canonicalConfidence,
                                                     sourceTargetInDestinationMeasure,
                                                     destinationTarget);
    if (status != MathStatus::Success)
    {
        return status;
    }
    float32 value = 0;
    float32 targetScale = Max(sourceTargetInDestinationMeasure, destinationTarget);
    if (neighborConfidence > 0 && targetScale > 0)
    {
        float32 confidenceScale = Max(confidenceSum, canonicalConfidence);
        float32 source = sourceTargetInDestinationMeasure / targetScale;
        float32 destination = destinationTarget / targetScale;
        float32 sum = confidenceSum / confidenceScale;
        float32 canonical = canonicalConfidence / confidenceScale;
        float32 neighbor = neighborConfidence / confidenceScale;
        float32 denominator = sum * source + canonical * destination;
        if (denominator > 0)
        {
            value = (sum / (sum + canonical)) * (neighbor * source / denominator);
        }
    }
    outWeight = value;
    return MathStatus::Success;
}

MathStatus TryComputePairwiseCanonicalTerm(float32 neighborConfidence,
                                           float32 confidenceSum,
                                           float32 canonicalConfidence,
                                           float32 sourceTargetInDestinationMeasure,
                                           float32 destinationTarget,
                                           float32& outWeight) noexcept
{
    const MathStatus status = ValidatePairwiseInputs(neighborConfidence,
                                                     confidenceSum,
                                                     canonicalConfidence,
                                                     sourceTargetInDestinationMeasure,
                                                     destinationTarget);
    if (status != MathStatus::Success)
    {
        return status;
    }
    float32 value = 0;
    float32 targetScale = Max(sourceTargetInDestinationMeasure, destinationTarget);
    if (neighborConfidence > 0 && targetScale > 0)
    {
        float32 confidenceScale = Max(confidenceSum, canonicalConfidence);
        float32 source = sourceTargetInDestinationMeasure / targetScale;
        float32 destination = destinationTarget / targetScale;
        float32 sum = confidenceSum / confidenceScale;
        float32 canonical = canonicalConfidence / confidenceScale;
        float32 neighbor = neighborConfidence / confidenceScale;
        float32 denominator = sum * source + canonical * destination;
        if (denominator > 0)
        {
            value = (neighbor / (sum + canonical)) * (canonical * destination / denominator);
        }
    }
    outWeight = value;
    return MathStatus::Success;
}

MathStatus
TryComputeStochasticMisWeight(float32 misWeight, float32 selectionPmf, uint32 drawCount, float32& outWeight) noexcept
{
    if (IsFinite(misWeight) == false || IsFinite(selectionPmf) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (misWeight < 0 || selectionPmf <= 0 || selectionPmf > 1 || drawCount == 0)
    {
        return MathStatus::InvalidArgument;
    }
    float32 value = (misWeight / static_cast<float32>(drawCount)) / selectionPmf;
    if (IsFinite(value) == false)
    {
        return MathStatus::OutOfRange;
    }
    outWeight = value;
    return MathStatus::Success;
}

MathStatus TryComputePairwiseShiftedTarget(float32 sourceTarget, float32 forwardJacobian, float32& outTarget) noexcept
{
    if (IsFinite(sourceTarget) == false || IsFinite(forwardJacobian) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (sourceTarget < 0 || forwardJacobian < 0)
    {
        return MathStatus::InvalidArgument;
    }
    float32 value = forwardJacobian == 0 ? 0 : sourceTarget / forwardJacobian;
    if (IsFinite(value) == false)
    {
        return MathStatus::OutOfRange;
    }
    outTarget = value;
    return MathStatus::Success;
}

MathStatus TryComputePairwiseResamplingWeight(float32 misWeight,
                                              float32 destinationTarget,
                                              float32 sourceContributionWeight,
                                              float32 forwardJacobian,
                                              float32& outWeight) noexcept
{
    if (IsFinite(misWeight) == false || IsFinite(destinationTarget) == false ||
        IsFinite(sourceContributionWeight) == false || IsFinite(forwardJacobian) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (misWeight < 0 || destinationTarget < 0 || sourceContributionWeight < 0 || forwardJacobian < 0)
    {
        return MathStatus::InvalidArgument;
    }
    float32 value = misWeight * destinationTarget * sourceContributionWeight * forwardJacobian;
    if (IsFinite(value) == false)
    {
        return MathStatus::OutOfRange;
    }
    outWeight = value;
    return MathStatus::Success;
}

MathStatus TryComputePairwiseContributionWeight(float32 weightSum, float32 selectedTarget, float32& outWeight) noexcept
{
    if (IsFinite(weightSum) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (weightSum < 0)
    {
        return MathStatus::InvalidArgument;
    }
    float32 value = 0;
    if (weightSum > 0)
    {
        if (IsFinite(selectedTarget) == false)
        {
            return MathStatus::NonFiniteInput;
        }
        if (selectedTarget <= 0)
        {
            return MathStatus::InvalidArgument;
        }
        value = weightSum / selectedTarget;
        if (IsFinite(value) == false)
        {
            return MathStatus::OutOfRange;
        }
    }
    outWeight = value;
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
