#pragma once

// Thanks to Trevor Hedstrom, Markus Kettunen, Daqi Lin, Chris Wyman and Tzu-Mao Li,
// "Stochastic Pairwise MIS for Unbiased Large-Kernel Reuse in Real-Time",
// Computer Graphics Forum 45(2), 2026, sections 4.1-4.3, equations 16, 18 and 19:
// https://doi.org/10.1111/cgf.70391
// Independently implemented for direct-light samples in common emitter-area measure
// (identity shifts, unit Jacobian). Confidence inputs include the N/M scaling.
float EvaluatePairwiseNeighborMis(float neighborConfidence,
                                  float confidenceSum,
                                  float canonicalConfidence,
                                  float sourceTarget,
                                  float destinationTarget)
{
    float targetScale = max(sourceTarget, destinationTarget);
    if (neighborConfidence <= 0 || confidenceSum <= 0 || targetScale <= 0)
        return 0;
    float source = sourceTarget / targetScale;
    float destination = destinationTarget / targetScale;
    float denominator = confidenceSum * source + canonicalConfidence * destination;
    return denominator > 0
               ? (confidenceSum / (confidenceSum + canonicalConfidence)) * (neighborConfidence * source / denominator)
               : 0;
}

float EvaluatePairwiseCanonicalBeta(float neighborConfidence,
                                    float confidenceSum,
                                    float canonicalConfidence,
                                    float neighborTarget,
                                    float canonicalTarget)
{
    float targetScale = max(neighborTarget, canonicalTarget);
    if (neighborConfidence <= 0 || targetScale <= 0)
        return 0;
    float neighbor = neighborTarget / targetScale;
    float canonical = canonicalTarget / targetScale;
    float denominator = confidenceSum * neighbor + canonicalConfidence * canonical;
    return denominator > 0 ? (neighborConfidence / (confidenceSum + canonicalConfidence)) *
                                 (canonicalConfidence * canonical / denominator)
                           : 0;
}
