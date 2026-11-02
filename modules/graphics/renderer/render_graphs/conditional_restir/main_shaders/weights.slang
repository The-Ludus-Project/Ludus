#pragma once

// Thanks to Markus Kettunen, Daqi Lin, Ravi Ramamoorthi, Thomas Bashford-Rogers and Chris Wyman,
// "Conditional Resampled Importance Sampling and ReSTIR", SIGGRAPH Asia 2023, sections 4-6,
// Equations 12-17 and 22, https://doi.org/10.1145/3610548.3618245 .
// Specialization: suffixes are mesh-light endpoints in world area, with full support for every prefix.
// The identity shift has Jacobian one. Confidence/M gives a constant partition of unity, so conditional
// RIS reduces to target * UCW * M and W = sum / (M * selectedTarget). No support-dependent rejection,
// visibility conditioning, PDF reconstruction from W, or prefix throughput enters reservoir selection.

/// Conditional RIS mass of a represented group, in the common world-area measure.
float ComputeConditionalMass(float target, float contributionWeight, uint candidateCount)
{
    return target * contributionWeight * float(candidateCount);
}

/// Final conditional UCW before confidence is capped. Empty groups have zero contribution.
float ComputeConditionalWeight(float massSum, float selectedTarget, uint candidateCount)
{
    return candidateCount > 0 && selectedTarget > 0 ? massSum / (float(candidateCount) * selectedTarget) : 0;
}

/// Accumulate a nonnegative finite mass, returning whether the reservoir should replace its sample.
/// Invalid mass or overflow returns false and leaves the sum unchanged; zero masses never select.
bool TryStreamConditionalMass(float mass, float coin, inout float inoutMassSum, out bool outShouldSelect)
{
    outShouldSelect = false;
    float sum = inoutMassSum + mass;
    if (isfinite(mass) == false || mass < 0 || isfinite(sum) == false)
        return false;
    inoutMassSum = sum;
    outShouldSelect = mass > 0 && coin < mass / sum;
    return true;
}
