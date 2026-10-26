#pragma once
// Multiple importance sampling (MIS) weights for the two direct-lighting strategies of the shade passes.
// Thanks to Eric Veach and Leonidas J. Guibas, "Optimally Combining Sampling Techniques for Monte
// Carlo Rendering", SIGGRAPH 1995, pp. 419-428, https://doi.org/10.1145/218380.218498 : their
// multi-sample estimator sum_i (1 / n_i) sum_j w_i(X_ij) f(X_ij) / p_i(X_ij) stays unbiased whenever
// the weights sum to one where f is nonzero. They prove the balance heuristic close to optimal and
// add the cutoff, power and maximum heuristics, which sharpen it where one strategy is already good.
//
// Each strategy here draws one sample (n_i = 1), so a density is also its strategy's effective
// sample density n_i p_i. Both densities must use one measure: the shade passes use solid angle.
// MIS_HEURISTIC, MIS_POWER_EXPONENT and MIS_CUTOFF_THRESHOLD are Graph settings (main.lushader.json);
// the builder declares them before this source and validates their ranges.

static const uint MIS_HEURISTIC_BALANCE = 0;
static const uint MIS_HEURISTIC_POWER = 1;
static const uint MIS_HEURISTIC_CUTOFF = 2;
static const uint MIS_HEURISTIC_MAXIMUM = 3;

/// Weight of the strategy that drew a sample against the one other strategy that could have drawn it.
/// A nonfinite or nonpositive sampled density returns 0. A zero other density, for a delta light or a
/// direction the other strategy never proposes, returns 1; a nonfinite one returns 0. For finite
/// positive densities ComputeMisWeight(p, q) + ComputeMisWeight(q, p) == 1 under every heuristic,
/// so summing both strategies' weighted estimates is unbiased.
float ComputeMisWeight(float sampledDensity, float otherDensity)
{
    if (isfinite(sampledDensity) == false || sampledDensity <= 0)
        return 0;
    if (otherDensity <= 0)
        return 1;
    if (isfinite(otherDensity) == false)
        return 0;
    // Scale both by the larger density, so powers neither overflow nor flush both terms to zero.
    float largerDensity = max(sampledDensity, otherDensity);
    float sampled = sampledDensity / largerDensity;
    float other = otherDensity / largerDensity;
    if (MIS_HEURISTIC == MIS_HEURISTIC_POWER)
    {
        sampled = pow(sampled, MIS_POWER_EXPONENT);
        other = pow(other, MIS_POWER_EXPONENT);
    }
    else if (MIS_HEURISTIC == MIS_HEURISTIC_CUTOFF)
    {
        // A strategy below the threshold fraction of the larger density gets no weight; the rest share
        // it as in the balance heuristic. The larger density is 1 here, so one strategy always remains.
        if (sampled < MIS_CUTOFF_THRESHOLD)
            return 0;
        if (other < MIS_CUTOFF_THRESHOLD)
            return 1;
    }
    else if (MIS_HEURISTIC == MIS_HEURISTIC_MAXIMUM)
    {
        // Equal densities split the sample evenly, which keeps the two weights summing to one.
        if (sampled == other)
            return 0.5;
        return sampled > other ? 1 : 0;
    }
    return sampled / (sampled + other);
}
