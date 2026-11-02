#pragma once

// Compatibility names for project-owned DI shaders. The engine-owned CPU/shader
// implementation and contracts live in FoundationMath's stochastic_pairwise_mis.
float EvaluatePairwiseNeighborMis(float neighborConfidence,
                                  float confidenceSum,
                                  float canonicalConfidence,
                                  float sourceTarget,
                                  float destinationTarget)
{
    float weight = 0;
    TryComputePairwiseNeighborMis(neighborConfidence,
                                  confidenceSum,
                                  canonicalConfidence,
                                  sourceTarget,
                                  destinationTarget,
                                  weight);
    return weight;
}
float EvaluatePairwiseCanonicalBeta(float neighborConfidence,
                                    float confidenceSum,
                                    float canonicalConfidence,
                                    float neighborTarget,
                                    float canonicalTarget)
{
    float weight = 0;
    TryComputePairwiseCanonicalTerm(neighborConfidence,
                                    confidenceSum,
                                    canonicalConfidence,
                                    neighborTarget,
                                    canonicalTarget,
                                    weight);
    return weight;
}
