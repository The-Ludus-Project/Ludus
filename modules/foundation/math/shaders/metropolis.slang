#pragma once

// GPU counterpart of metropolis.hpp/metropolis.cpp. Thanks to Sascha Holl,
// Gurprit Singh and Hans-Peter Seidel, "Rao-Blackwellized Markov chain Monte
// Carlo Light Transport", SIGGRAPH 2026, section 5.4, Algorithm 5.2,
// https://arxiv.org/abs/2605.09117 . Include the accepting proposal in P and
// HoldingWeight; preserve an unfinished tour across dispatch boundaries.
static const uint METROPOLIS_VERSION = 1;
static const float METROPOLIS_MINIMUM_NORMAL = 0x1p-126f;

struct MetropolisTour
{
    float RejectionProduct;
    float HoldingWeight;
};
struct WeightedRadiance
{
    float4 Sum;
    float4 Compensation;
};

bool TryComputeSymmetricMetropolisAcceptance(float currentTarget, float proposedTarget, out float outAcceptance)
{
    outAcceptance = 0;
    if (isfinite(currentTarget) == false || isfinite(proposedTarget) == false ||
        currentTarget < METROPOLIS_MINIMUM_NORMAL || proposedTarget < METROPOLIS_MINIMUM_NORMAL)
        return false;
    outAcceptance = proposedTarget >= currentTarget ? 1 : proposedTarget / currentTarget;
    return true;
}

bool TryMutateMetropolisPrimary(float current, float ticket, float radius, bool isLargeStep, out float outSample)
{
    outSample = 0;
    if (isfinite(current) == false || isfinite(ticket) == false || isfinite(radius) == false || current < 0 ||
        current >= 1 || ticket < 0 || ticket >= 1 || radius <= 0 || radius > 0.5)
        return false;
    float sample = isLargeStep == true ? ticket : current + (2 * ticket - 1) * radius;
    if (sample < 0)
        sample += 1;
    if (sample >= 1)
        sample -= 1;
    outSample = sample;
    return true;
}

bool TryAdvanceMetropolisTour(float acceptance, inout MetropolisTour inoutTour)
{
    if (isfinite(acceptance) == false || isfinite(inoutTour.RejectionProduct) == false ||
        isfinite(inoutTour.HoldingWeight) == false || acceptance < 0 || acceptance > 1 ||
        inoutTour.RejectionProduct < 0 || inoutTour.RejectionProduct > 1 || inoutTour.HoldingWeight < 1)
        return false;
    MetropolisTour candidate = inoutTour;
    candidate.RejectionProduct *= 1 - acceptance;
    candidate.HoldingWeight += candidate.RejectionProduct;
    if (isfinite(candidate.HoldingWeight) == false)
        return false;
    inoutTour = candidate;
    return true;
}

bool TryAddWeightedRadiance(float3 radiance, float weight, inout WeightedRadiance inoutAccumulation)
{
    if (all(isfinite(radiance)) == false || isfinite(weight) == false || any(radiance < 0) == true || weight < 0 ||
        all(isfinite(inoutAccumulation.Sum)) == false || all(isfinite(inoutAccumulation.Compensation)) == false ||
        any(inoutAccumulation.Sum < 0) == true)
        return false;
    if (weight == 0)
        return true;
    precise float4 contribution = float4(radiance * weight, weight);
    precise float4 adjusted = contribution - inoutAccumulation.Compensation;
    precise float4 combined = inoutAccumulation.Sum + adjusted;
    precise float4 compensation = (combined - inoutAccumulation.Sum) - adjusted;
    if (all(isfinite(contribution)) == false || all(isfinite(combined)) == false ||
        all(isfinite(compensation)) == false || any(combined < 0) == true)
        return false;
    inoutAccumulation.Sum = combined;
    inoutAccumulation.Compensation = compensation;
    return true;
}

bool TryResolveWeightedRadiance(WeightedRadiance accumulation, out float3 outRadiance)
{
    outRadiance = float3(0);
    if (all(isfinite(accumulation.Sum)) == false || all(isfinite(accumulation.Compensation)) == false ||
        any(accumulation.Sum < 0) == true || (accumulation.Sum.w == 0 && any(accumulation.Sum.xyz != 0) == true))
        return false;
    float3 result = accumulation.Sum.w > 0 ? accumulation.Sum.xyz / accumulation.Sum.w : float3(0);
    if (all(isfinite(result)) == false)
        return false;
    outRadiance = result;
    return true;
}
