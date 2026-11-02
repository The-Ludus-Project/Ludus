#pragma once

// Thanks to Trevor Hedstrom, Markus Kettunen, Daqi Lin, Chris Wyman and Tzu-Mao Li,
// "Stochastic Pairwise MIS for Unbiased Large-Kernel Reuse in Real-Time",
// Computer Graphics Forum 45(2), 2026, equations 10/11/14/15/18/19 and section 4.3:
// https://doi.org/10.1111/cgf.70391
// Independently implemented scalar arithmetic, with caller-owned samples, shifts,
// probability distributions and storage. See docs/architecture/randomness.md.

/// Version of the scalar MIS arithmetic and input contracts, matching the C++ API.
static const uint STOCHASTIC_PAIRWISE_MIS_VERSION = 1;
static const uint PAIRWISE_FLOAT_EXPONENT_MASK = 0x7f800000u;
// Bit classification remains valid with Metal finite/fast-math compilation.
bool IsPairwiseFinite(float value)
{
    return (asuint(value) & PAIRWISE_FLOAT_EXPONENT_MASK) != PAIRWISE_FLOAT_EXPONENT_MASK;
}
bool ArePairwiseInputsValid(float neighborConfidence,
                            float confidenceSum,
                            float canonicalConfidence,
                            float sourceTarget,
                            float destinationTarget)
{
    return IsPairwiseFinite(neighborConfidence) == true && IsPairwiseFinite(confidenceSum) == true &&
           IsPairwiseFinite(canonicalConfidence) == true && IsPairwiseFinite(sourceTarget) == true &&
           IsPairwiseFinite(destinationTarget) == true && neighborConfidence >= 0 &&
           neighborConfidence <= confidenceSum && canonicalConfidence > 0 && sourceTarget >= 0 &&
           destinationTarget >= 0;
}
// Every Try function mirrors stochastic_pairwise_mis.hpp. False preserves outputs;
// true writes the finite nonnegative result. All arithmetic is FP32; backend
// division, rounding and subnormal flushing can differ from the CPU implementation.

/// Compute the defensive neighbor-confidence scale min(drawCount / neighborCount, 1).
/// Zero neighbors produces zero; drawCount must be positive. Apply this one scale to every
/// neighbor confidence and to their sum before computing either canonical or neighbor MIS.
/// @param inoutScale Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
bool TryComputePairwiseConfidenceScale(uint drawCount, uint neighborCount, inout float inoutScale)
{
    if (drawCount == 0)
        return false;
    float value = neighborCount == 0 ? 0 : min(float(drawCount) / float(neighborCount), float(1));
    inoutScale = value;
    return true;
}

/// Compute the defensive canonical base weight c_c / (c_sum + c_c), equation 18.
/// Confidences are finite, nonnegative and in common arbitrary units; canonicalConfidence
/// must be positive. confidenceSum is the sum of scaled noncanonical confidences.
/// @param inoutWeight Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
bool TryComputePairwiseCanonicalBase(float confidenceSum, float canonicalConfidence, inout float inoutWeight)
{
    if (ArePairwiseInputsValid(0, confidenceSum, canonicalConfidence, 0, 0) == false)
        return false;
    float scale = max(confidenceSum, canonicalConfidence);
    float value = (canonicalConfidence / scale) / (confidenceSum / scale + canonicalConfidence / scale);
    inoutWeight = value;
    return true;
}

/// Compute a deterministic defensive noncanonical pairwise MIS weight, equation 11a.
/// Targets must be finite, nonnegative densities in the destination measure at the same
/// shifted sample. sourceTargetInDestinationMeasure is p_i(x_i) / abs(J_i_to_c), including
/// source proposal support; it is NOT the untransformed source target for non-unit shifts.
/// Confidences must be finite, 0 <= neighborConfidence <= confidenceSum and
/// canonicalConfidence > 0. Zero neighbor confidence or two zero targets yields zero.
/// @param inoutWeight Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
/// @note Callers own shifts, support tests and valid input contribution weights. The formula
/// alone does not establish an unbiased estimator. FP32 rounding/subnormal flushing applies.
bool TryComputePairwiseNeighborMis(float neighborConfidence,
                                   float confidenceSum,
                                   float canonicalConfidence,
                                   float sourceTargetInDestinationMeasure,
                                   float destinationTarget,
                                   inout float inoutWeight)
{
    if (ArePairwiseInputsValid(neighborConfidence,
                               confidenceSum,
                               canonicalConfidence,
                               sourceTargetInDestinationMeasure,
                               destinationTarget) == false)
        return false;
    float value = 0;
    float targetScale = max(sourceTargetInDestinationMeasure, destinationTarget);
    if (neighborConfidence > 0 && targetScale > 0)
    {
        float confidenceScale = max(confidenceSum, canonicalConfidence);
        float source = sourceTargetInDestinationMeasure / targetScale;
        float destination = destinationTarget / targetScale;
        float sum = confidenceSum / confidenceScale;
        float canonical = canonicalConfidence / confidenceScale;
        float neighbor = neighborConfidence / confidenceScale;
        float denominator = sum * source + canonical * destination;
        if (denominator > 0)
            value = (sum / (sum + canonical)) * (neighbor * source / denominator);
    }
    inoutWeight = value;
    return true;
}

/// Compute one canonical pairwise correction beta_i, equation 19, at the canonical sample.
/// Inputs have the same domains and common-measure contract as TryComputePairwiseNeighborMis.
/// For a neighbor unable to propose the canonical sample, pass a zero source target.
/// Add beta_i / (canonicalDrawCount * canonicalSelectionPmf) per independently drawn
/// neighbor to TryComputePairwiseCanonicalBase; repeated draws retain multiplicity.
/// @param inoutWeight Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
bool TryComputePairwiseCanonicalTerm(float neighborConfidence,
                                     float confidenceSum,
                                     float canonicalConfidence,
                                     float sourceTargetInDestinationMeasure,
                                     float destinationTarget,
                                     inout float inoutWeight)
{
    if (ArePairwiseInputsValid(neighborConfidence,
                               confidenceSum,
                               canonicalConfidence,
                               sourceTargetInDestinationMeasure,
                               destinationTarget) == false)
        return false;
    float value = 0;
    float targetScale = max(sourceTargetInDestinationMeasure, destinationTarget);
    if (neighborConfidence > 0 && targetScale > 0)
    {
        float confidenceScale = max(confidenceSum, canonicalConfidence);
        float source = sourceTargetInDestinationMeasure / targetScale;
        float destination = destinationTarget / targetScale;
        float sum = confidenceSum / confidenceScale;
        float canonical = canonicalConfidence / confidenceScale;
        float neighbor = neighborConfidence / confidenceScale;
        float denominator = sum * source + canonical * destination;
        if (denominator > 0)
            value = (neighbor / (sum + canonical)) * (canonical * destination / denominator);
    }
    inoutWeight = value;
    return true;
}

/// Correct one with-replacement draw by its actual selection probability, equation 15.
/// Returns misWeight / (drawCount * selectionPmf). misWeight is finite and nonnegative;
/// selectionPmf is finite in (0,1] and drawCount is positive. Stream each occurrence or
/// multiply by its multiplicity. A proposal must have positive probability wherever its
/// MIS-weighted contribution can be nonzero. Draw coins must be fresh conditional on inputs.
/// @param inoutWeight Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
/// @note No allocation or thread affinity; probability/ticket quantization is caller-owned.
bool TryComputeStochasticMisWeight(float misWeight, float selectionPmf, uint drawCount, inout float inoutWeight)
{
    if (IsPairwiseFinite(misWeight) == false || IsPairwiseFinite(selectionPmf) == false)
        return false;
    if (misWeight < 0 || selectionPmf <= 0 || selectionPmf > 1 || drawCount == 0)
        return false;
    float value = (misWeight / float(drawCount)) / selectionPmf;
    if (IsPairwiseFinite(value) == false)
        return false;
    inoutWeight = value;
    return true;
}

/// Express a source target in the destination measure: p_i(x) / abs(J_i_to_c), equation 10.
/// Inputs are finite and nonnegative. A zero Jacobian denotes a failed partial shift and
/// produces zero support. Callers must also zero support for impossible inverse proposals.
/// @param inoutTarget Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
/// No resource access; callable from any shader invocation.
bool TryComputePairwiseShiftedTarget(float sourceTarget, float forwardJacobian, inout float inoutTarget)
{
    if (IsPairwiseFinite(sourceTarget) == false || IsPairwiseFinite(forwardJacobian) == false)
        return false;
    if (sourceTarget < 0 || forwardJacobian < 0)
        return false;
    float value = forwardJacobian == 0 ? 0 : sourceTarget / forwardJacobian;
    if (IsPairwiseFinite(value) == false)
        return false;
    inoutTarget = value;
    return true;
}

/// Compute a shifted GRIS resampling weight m * p_c(y) * W_i * abs(J_i_to_c), equation 14.
/// All inputs must be finite and nonnegative. misWeight normally includes the stochastic
/// correction; sourceContributionWeight is a valid input UCW, not a streaming weight sum.
/// forwardJacobian is zero for a failed shift. The caller supplies visibility/support policy.
/// @param inoutWeight Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
/// No resource access; callable from any shader invocation.
bool TryComputePairwiseResamplingWeight(float misWeight,
                                        float destinationTarget,
                                        float sourceContributionWeight,
                                        float forwardJacobian,
                                        inout float inoutWeight)
{
    if (IsPairwiseFinite(misWeight) == false || IsPairwiseFinite(destinationTarget) == false ||
        IsPairwiseFinite(sourceContributionWeight) == false || IsPairwiseFinite(forwardJacobian) == false)
        return false;
    if (misWeight < 0 || destinationTarget < 0 || sourceContributionWeight < 0 || forwardJacobian < 0)
        return false;
    float value = misWeight * destinationTarget * sourceContributionWeight * forwardJacobian;
    if (IsPairwiseFinite(value) == false)
        return false;
    inoutWeight = value;
    return true;
}

/// Finalize pairwise GRIS with W = weightSum / selectedTarget, equation 7, WITHOUT dividing by M.
/// weightSum must be finite and nonnegative. An empty sum yields zero without examining
/// selectedTarget; otherwise selectedTarget must be finite and positive, in the destination
/// measure and identical to the target used for the selected candidate's resampling weight.
/// @param inoutWeight Written on success; preserved on failure.
/// @return True with a finite result; false for invalid input or FP32 overflow.
/// No resource access; callable from any shader invocation.
bool TryComputePairwiseContributionWeight(float weightSum, float selectedTarget, inout float inoutWeight)
{
    if (IsPairwiseFinite(weightSum) == false)
        return false;
    if (weightSum < 0)
        return false;
    float value = 0;
    if (weightSum > 0)
    {
        if (IsPairwiseFinite(selectedTarget) == false)
            return false;
        if (selectedTarget <= 0)
            return false;
        value = weightSum / selectedTarget;
        if (IsPairwiseFinite(value) == false)
            return false;
    }
    inoutWeight = value;
    return true;
}
