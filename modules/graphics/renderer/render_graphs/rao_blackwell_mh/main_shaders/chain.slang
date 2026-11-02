#pragma once
#include "path.slang"

// Thanks to Sascha Holl, Gurprit Singh and Hans-Peter Seidel, "Rao-Blackwellized
// Markov chain Monte Carlo Light Transport", SIGGRAPH 2026, section 5.4 and
// Algorithm 5.2, https://arxiv.org/abs/2605.09117 . The numerical recurrence is
// FoundationMath's metropolis.slang. Our per-pixel primary-space ratio estimator
// applies each tour weight to both L/p and 1/p. It is not multiplexed MLT.
static const uint MH_STANDARD = 0;
static const uint MH_WASTE_RECYCLING = 1;
static const uint MH_RAO_BLACKWELL = 2;
static const uint MH_COMPONENTS_PER_WORD = LUDUS_MH_COMPONENTS_PER_WORD;
static const uint MH_PRIMARY_WORDS = (MH_PRIMARY_DIMENSIONS + MH_COMPONENTS_PER_WORD - 1) / MH_COMPONENTS_PER_WORD;
static const uint MH_CURRENT_WORD = MH_PRIMARY_WORDS;
static const uint MH_TOUR_WORD = MH_CURRENT_WORD + 1;
static const uint MH_GENERATION_WORD = LUDUS_MH_GENERATION_WORD;
static const uint MH_IDENTITY_WORD = MH_GENERATION_WORD + 1;
static const uint MH_RECORD_WORDS = MH_IDENTITY_WORD + 1;
static const uint MH_ESTIMATOR_COUNT = LUDUS_MH_ESTIMATOR_COUNT;
static const uint MH_RECORD_VERSION = LUDUS_MH_RECORD_VERSION;
static const uint MH_ACCEPTANCE_DIMENSION = MH_PRIMARY_DIMENSIONS;
static const uint MH_LARGE_STEP_DIMENSION = MH_ACCEPTANCE_DIMENSION + 1;
static const uint MH_INITIAL_SCOPE = 1;
static const uint MH_PROPOSAL_SCOPE = 2;
static const uint JR_INITIAL_SCOPE = 3;
static const uint JR_PROPOSAL_SCOPE = 4;
static const uint JR_HOLDING_TIME_DIMENSION = MH_LARGE_STEP_DIMENSION + 1;

float SampleMetropolisRandom(RayLaunch launch, uint scope, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    return SampleRandomFloat01(launch.Seed, MakeRandomAddress(uint2(pixel, scope), launch.SampleIndex, dimension));
}

float3 AdvanceMetropolisChain(SceneContext scene,
                              RayLaunch launch,
                              inout RayQueryContext queries,
                              uint estimator,
                              bool isJumpRestore)
{
    uint wordCount;
    uint stride;
    GraphBuffer0.GetDimensions(wordCount, stride);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    if (pixel >= wordCount / MH_RECORD_WORDS || Counts.w != 0)
    {
        FailSceneQuery(queries,
                       pixel >= wordCount / MH_RECORD_WORDS ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY
                                                            : LUDUS_SCENE_FAILURE_BSDF_ONLY_LIGHTS);
        return float3(0);
    }
    uint filmWordCount;
    GraphBuffer1.GetDimensions(filmWordCount, stride);
    if (pixel >= filmWordCount / LUDUS_MH_FILM_WORDS)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    uint base = pixel * MH_RECORD_WORDS;
    uint filmBase = pixel * LUDUS_MH_FILM_WORDS;
    uint4 generation = GraphBuffer0[base + MH_GENERATION_WORD];
    uint4 identity = GraphBuffer0[base + MH_IDENTITY_WORD];
    bool hasHistory = all(generation == uint4(scene.Generation, launch.ExtentPixel)) == true &&
                      all(identity.xy == launch.Seed) == true && identity.z == MH_RECORD_VERSION &&
                      identity.w == launch.SampleIndex && launch.SampleIndex != 0;
    if (estimator != MH_RAO_BLACKWELL)
    {
        // The RB pass owns the single transport step and updates all estimators.
        // These two passes only resolve their own weighted sums from that step.
        if (all(generation == uint4(scene.Generation, launch.ExtentPixel)) == false ||
            all(identity.xy == launch.Seed) == false || identity.z != MH_RECORD_VERSION ||
            identity.w != launch.SampleIndex + 1)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
            return float3(0);
        }
        WeightedRadiance film;
        film.Sum = asfloat(GraphBuffer1[filmBase + estimator * LUDUS_MH_ACCUMULATOR_WORDS]);
        film.Compensation = asfloat(GraphBuffer1[filmBase + estimator * LUDUS_MH_ACCUMULATOR_WORDS + 1]);
        float3 resolved;
        if (TryResolveWeightedRadiance(film, resolved) == false)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return ResolveGraphRadiance(resolved);
    }
    PrimarySamples current;
    float3 currentRadiance = float3(0);
    float currentTarget = 0;
    MetropolisTour tour;
    tour.RejectionProduct = 1;
    tour.HoldingWeight = 1;
    uint iteration = 0;
    WeightedRadiance films[MH_ESTIMATOR_COUNT];
    uint initialScope = isJumpRestore == true ? JR_INITIAL_SCOPE : MH_INITIAL_SCOPE;
    uint proposalScope = isJumpRestore == true ? JR_PROPOSAL_SCOPE : MH_PROPOSAL_SCOPE;
    [unroll] for (uint index = 0; index < MH_ESTIMATOR_COUNT; ++index) films[index] = (WeightedRadiance)0;
    if (hasHistory == true)
    {
        [unroll] for (uint dimension = 0; dimension < MH_PRIMARY_DIMENSIONS; ++dimension) current.Values[dimension] =
            asfloat(GraphBuffer0[base + dimension / MH_COMPONENTS_PER_WORD][dimension % MH_COMPONENTS_PER_WORD]);
        float4 value = asfloat(GraphBuffer0[base + MH_CURRENT_WORD]);
        currentRadiance = value.xyz;
        currentTarget = value.w;
        float4 storedTour = asfloat(GraphBuffer0[base + MH_TOUR_WORD]);
        tour.RejectionProduct = storedTour.x;
        tour.HoldingWeight = storedTour.y;
        iteration = asuint(storedTour.z);
        [unroll] for (uint index = 0; index < MH_ESTIMATOR_COUNT; ++index)
        {
            films[index].Sum = asfloat(GraphBuffer1[filmBase + index * LUDUS_MH_ACCUMULATOR_WORDS]);
            films[index].Compensation = asfloat(GraphBuffer1[filmBase + index * LUDUS_MH_ACCUMULATOR_WORDS + 1]);
        }
    }
    else
    {
        [unroll] for (uint dimension = 0; dimension < MH_PRIMARY_DIMENSIONS; ++dimension) current.Values[dimension] =
            SampleMetropolisRandom(launch, initialScope, dimension);
        currentRadiance = EvaluateMetropolisPath(scene, GenerateMetropolisCameraRay(launch, current), current, queries);
        currentTarget = ComputeMetropolisTarget(currentRadiance);
    }
    JumpRestoreRates rates;
    rates.RestoreProbability = 0;
    rates.MeanHoldingTime = 1;
    float holdingTime = 1;
    bool isRestoration = false;
    if (isJumpRestore == true)
    {
        if (TryComputeJumpRestoreRates(currentTarget, JR_RESTORATION_SCALE, rates) == false ||
            TrySampleJumpRestoreHoldingTime(SampleMetropolisRandom(launch, proposalScope, JR_HOLDING_TIME_DIMENSION),
                                            rates.MeanHoldingTime,
                                            holdingTime) == false)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        isRestoration =
            SampleMetropolisRandom(launch, proposalScope, MH_LARGE_STEP_DIMENSION) < rates.RestoreProbability;
    }
    PrimarySamples proposal;
    bool isLargeStep = isJumpRestore == true ? isRestoration
                                             : SampleMetropolisRandom(launch, proposalScope, MH_LARGE_STEP_DIMENSION) <
                                                   MH_LARGE_STEP_PROBABILITY;
    [unroll] for (uint dimension = 0; dimension < MH_PRIMARY_DIMENSIONS; ++dimension)
    {
        if (TryMutateMetropolisPrimary(current.Values[dimension],
                                       SampleMetropolisRandom(launch, proposalScope, dimension),
                                       MH_SMALL_STEP_RADIUS,
                                       isLargeStep,
                                       proposal.Values[dimension]) == false)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
    }
    float3 proposedRadiance =
        EvaluateMetropolisPath(scene, GenerateMetropolisCameraRay(launch, proposal), proposal, queries);
    float proposedTarget = ComputeMetropolisTarget(proposedRadiance);
    float acceptance;
    if (TryComputeSymmetricMetropolisAcceptance(currentTarget, proposedTarget, acceptance) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    bool isAccepted =
        isRestoration == true || SampleMetropolisRandom(launch, proposalScope, MH_ACCEPTANCE_DIMENSION) < acceptance;
    bool canCollect = iteration >= MH_BURN_IN;
    if (canCollect == true)
    {
        float currentWeight = isRestoration == true ? 1 : 1 - acceptance;
        if (TryAddWeightedRadiance(currentRadiance, holdingTime / currentTarget, films[MH_STANDARD]) == false ||
            TryAddWeightedRadiance(currentRadiance,
                                   holdingTime * currentWeight / currentTarget,
                                   films[MH_WASTE_RECYCLING]) == false ||
            (isRestoration == false && TryAddWeightedRadiance(proposedRadiance,
                                                              holdingTime * acceptance / proposedTarget,
                                                              films[MH_WASTE_RECYCLING]) == false) ||
            (isRestoration == false && TryAdvanceMetropolisTour(acceptance, tour) == false))
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        if (isAccepted == true && TryAddWeightedRadiance(currentRadiance,
                                                         tour.HoldingWeight * rates.MeanHoldingTime / currentTarget,
                                                         films[MH_RAO_BLACKWELL]) == false)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    }

    if (isAccepted == true)
    {
        current = proposal;
        currentRadiance = proposedRadiance;
        currentTarget = proposedTarget;
        tour.RejectionProduct = 1;
        tour.HoldingWeight = 1;
    }
    float3 result;
    if (TryResolveWeightedRadiance(films[MH_RAO_BLACKWELL], result) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    // No record is published on query/transport/numerical failure. The renderer
    // latches the error and preserves its last committed image and count.
    if (queries.HasFailed == true)
        return float3(0);
    [unroll] for (uint word = 0; word < MH_PRIMARY_WORDS; ++word)
    {
        uint4 packed = uint4(0);
        [unroll] for (uint component = 0; component < MH_COMPONENTS_PER_WORD; ++component)
        {
            uint dimension = word * MH_COMPONENTS_PER_WORD + component;
            if (dimension < MH_PRIMARY_DIMENSIONS)
                packed[component] = asuint(current.Values[dimension]);
        }
        GraphBuffer0[base + word] = packed;
    }
    GraphBuffer0[base + MH_CURRENT_WORD] = asuint(float4(currentRadiance, currentTarget));
    GraphBuffer0[base + MH_TOUR_WORD] =
        uint4(asuint(tour.RejectionProduct), asuint(tour.HoldingWeight), iteration + 1, 0);
    [unroll] for (uint index = 0; index < MH_ESTIMATOR_COUNT; ++index)
    {
        GraphBuffer1[filmBase + index * LUDUS_MH_ACCUMULATOR_WORDS] = asuint(films[index].Sum);
        GraphBuffer1[filmBase + index * LUDUS_MH_ACCUMULATOR_WORDS + 1] = asuint(films[index].Compensation);
    }
    GraphBuffer0[base + MH_GENERATION_WORD] = uint4(scene.Generation, launch.ExtentPixel);
    GraphBuffer0[base + MH_IDENTITY_WORD] = uint4(launch.Seed, MH_RECORD_VERSION, launch.SampleIndex + 1);
    return ResolveGraphRadiance(result);
}

float3 AdvanceMetropolisChain(SceneContext scene, RayLaunch launch, inout RayQueryContext queries, uint estimator)
{
    return AdvanceMetropolisChain(scene, launch, queries, estimator, false);
}

float3 ShadeMetropolis(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return AdvanceMetropolisChain(scene, launch, queries, MH_STANDARD);
}
float3 ShadeWasteRecycling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return AdvanceMetropolisChain(scene, launch, queries, MH_WASTE_RECYCLING);
}
float3 ShadeRaoBlackwell(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return AdvanceMetropolisChain(scene, launch, queries, MH_RAO_BLACKWELL);
}

// Thanks to Holl, Singh and Seidel, "Rao-Blackwellized Markov chain Monte Carlo
// Light Transport", SIGGRAPH 2026, Algorithm 6.2 and section 6,
// https://arxiv.org/abs/2605.09117 . The exponential race is sampled by its
// independent event identity and minimum time; RB uses the conditional mean.
// Restorations flush the pending RB state without an additional local proposal.
float3 ShadeJumpRestore(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return AdvanceMetropolisChain(scene, launch, queries, MH_RAO_BLACKWELL, true);
}
