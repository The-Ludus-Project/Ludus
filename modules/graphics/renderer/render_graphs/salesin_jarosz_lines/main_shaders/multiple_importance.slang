#pragma once
// Multiple importance sampling (MIS) weights for the three direct-lighting strategies of the shade
// passes: BSDF sampling and line sampling in the two directions U and V (point-line design D8b).
// Thanks to Eric Veach and Leonidas J. Guibas, "Optimally Combining Sampling Techniques for Monte
// Carlo Rendering", SIGGRAPH 1995, pp. 419-428, https://doi.org/10.1145/218380.218498 : their
// multi-sample estimator sum_i (1 / n_i) sum_j w_i(X_ij) f(X_ij) / p_i(X_ij) stays unbiased whenever
// the weights sum to one where f is nonzero, for any number of strategies. Their balance heuristic
// w_i = p_i / sum_k p_k and power heuristic w_i = p_i^beta / sum_k p_k^beta are used here for three.
// Thanks to Katherine Salesin and Wojciech Jarosz, "Combining Point and Line Samples for Direct
// Illumination", Computer Graphics Forum 38(4) (EGSR 2019), pp. 159-169,
// https://doi.org/10.1111/cgf.13779 , section 4.2: a line sample is a point sample with the joint
// density p(u, v) = p(v) p(u | v), so line samples in two directions and BSDF samples combine with
// these heuristics in one sampling space. Their balance heuristic is the default. Their section 4.3
// discontinuity-smoothing heuristic is not adopted: it needs stratified samples (design D10).
//
// Each strategy draws one sample (n_i = 1), so a density is also its strategy's effective sample
// density n_i p_i. All densities must use one measure: the shade passes use solid angle at the
// receiver, or the discrete selection probability for a delta light. MIS_HEURISTIC and
// MIS_POWER_EXPONENT are Graph settings (main.lushader.json); the builder declares them before this
// source and validates their ranges.

static const uint MIS_HEURISTIC_BALANCE = 0;
static const uint MIS_HEURISTIC_POWER = 1;

/// Weight of the strategy that drew a sample against the two other strategies that could have drawn
/// it. A nonfinite or nonpositive sampled density returns 0. A zero other density drops that strategy:
/// a pass without it passes 0, as does a strategy that never proposes the sample (BSDF sampling and a
/// delta light, or a line strategy and an occluded point). A nonfinite other density returns 0. For
/// finite positive densities p, q and r, ComputeMisWeight(p, q, r) + ComputeMisWeight(q, p, r) +
/// ComputeMisWeight(r, p, q) == 1 within float rounding under both heuristics, so summing the three
/// strategies' weighted estimates is unbiased; with r == 0 the first two weights sum to one.
float ComputeMisWeight(float sampledDensity, float otherDensity0, float otherDensity1)
{
    if (isfinite(sampledDensity) == false || sampledDensity <= 0)
        return 0;
    if (isfinite(otherDensity0) == false || isfinite(otherDensity1) == false)
        return 0;
    float other0Density = max(otherDensity0, 0);
    float other1Density = max(otherDensity1, 0);
    // Scale every density by the largest, so powers neither overflow nor flush every term to zero.
    float largestDensity = max(sampledDensity, max(other0Density, other1Density));
    float sampled = sampledDensity / largestDensity;
    float other0 = other0Density / largestDensity;
    float other1 = other1Density / largestDensity;
    if (MIS_HEURISTIC == MIS_HEURISTIC_POWER)
    {
        // A dropped strategy stays dropped without pow(0, beta), which fast math need not keep exact.
        sampled = pow(sampled, MIS_POWER_EXPONENT);
        other0 = other0 > 0 ? pow(other0, MIS_POWER_EXPONENT) : 0;
        other1 = other1 > 0 ? pow(other1, MIS_POWER_EXPONENT) : 0;
    }
    return sampled / (sampled + other0 + other1);
}
