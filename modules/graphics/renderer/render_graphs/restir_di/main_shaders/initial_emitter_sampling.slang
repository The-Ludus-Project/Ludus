#pragma once
// Include after emitter_sampling.slang and the scene-query interface.

// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, 4e,
// Appendix A.2, "Reservoir Sampling":
// https://www.pbr-book.org/4ed/Sampling_Algorithms/Reservoir_Sampling
// Adapt independent single-item weighted reservoirs to one shared emitter stream.
// Each candidate still selects emitter e with probability weight(e) / totalWeight;
// only receiver-dependent geometric weight evaluation is shared, never selection coins.
struct InitialEmitterSelection
{
    uint EmitterIndex;
    float SelectionPmf;
};

#if LUDUS_EMITTER_SAMPLING_MODE == LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED
// This is a local algorithm capacity, not a scene admission limit. Larger sets
// retain the exact same weighted proposal through the streaming path below.
static const uint INITIAL_EMITTER_ALIAS_CAPACITY = 16;

// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, 4e,
// Appendix A.1, "The Alias Method":
// https://www.pbr-book.org/4ed/Sampling_Algorithms/The_Alias_Method
// Adapt the small/large work lists to bounded GPU-local storage and use separate
// bounded-index and threshold draws. The table uses binary32 for at most 16 bins.
bool TrySelectInitialEmitterAlias(SceneContext scene,
                                  SurfaceHit receiver,
                                  uint emitterCount,
                                  inout RayQueryContext queries,
                                  out InitialEmitterSelection outSelections[RESTIR_INITIAL_CANDIDATE_COUNT])
{
    float weights[INITIAL_EMITTER_ALIAS_CAPACITY];
    float probabilities[INITIAL_EMITTER_ALIAS_CAPACITY];
    uint aliases[INITIAL_EMITTER_ALIAS_CAPACITY];
    uint work[INITIAL_EMITTER_ALIAS_CAPACITY];
    float totalWeight = 0;
    [loop] for (uint emitter = 0; emitter < emitterCount; ++emitter)
    {
        const float weight = ComputeSceneEmitterWeight(scene, emitter, receiver);
        if (isfinite(weight) == false || weight < 0)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            return false;
        }
        weights[emitter] = weight;
        totalWeight += weight;
        if (isfinite(totalWeight) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            return false;
        }
    }
    if (totalWeight <= 0)
    {
        return false;
    }
    uint smallCount = 0;
    uint largeBegin = emitterCount;
    [loop] for (uint emitter = 0; emitter < emitterCount; ++emitter)
    {
        probabilities[emitter] = (weights[emitter] / totalWeight) * float(emitterCount);
        aliases[emitter] = emitter;
        if (probabilities[emitter] < 1)
        {
            work[smallCount++] = emitter;
        }
        else
        {
            work[--largeBegin] = emitter;
        }
    }
    [loop] while (smallCount > 0 && largeBegin < emitterCount)
    {
        const uint small = work[--smallCount];
        const uint large = work[largeBegin++];
        aliases[small] = large;
        probabilities[large] = (probabilities[large] + probabilities[small]) - 1;
        if (probabilities[large] < 1)
        {
            work[smallCount++] = large;
        }
        else
        {
            work[--largeBegin] = large;
        }
    }
    // Residual entries differ from one only through floating-point accumulation.
    [loop] while (smallCount > 0)
    {
        probabilities[work[--smallCount]] = 1;
    }
    [loop] while (largeBegin < emitterCount)
    {
        probabilities[work[largeBegin++]] = 1;
    }
    [unroll] for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
    {
        const uint column = SampleSceneRandom(queries, 0u, emitterCount);
        const float randomValue = SampleSceneRandom(queries);
        if (queries.HasFailed == true)
        {
            return false;
        }
        const uint emitter = randomValue < probabilities[column] ? column : aliases[column];
        outSelections[candidate].EmitterIndex = emitter;
        outSelections[candidate].SelectionPmf = weights[emitter] / totalWeight;
    }
    return true;
}

#endif

bool TrySelectInitialEmitters(SceneContext scene,
                              SurfaceHit receiver,
                              inout RayQueryContext queries,
                              out InitialEmitterSelection outSelections[RESTIR_INITIAL_CANDIDATE_COUNT])
{
    [unroll] for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
    {
        outSelections[candidate] = (InitialEmitterSelection)0;
    }
    const uint emitterCount = GetEmitterCount(scene);
    if (emitterCount == 0)
    {
        return false;
    }
#if defined(LUDUS_DEBUG_LIGHT_SOURCE_INDEX)
    [unroll] for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
    {
        outSelections[candidate].EmitterIndex = uint(LUDUS_DEBUG_LIGHT_SOURCE_INDEX) % emitterCount;
        outSelections[candidate].SelectionPmf = 1;
    }
#elif LUDUS_EMITTER_SAMPLING_MODE == LUDUS_EMITTER_SAMPLING_MODE_UNIFORM
    [unroll] for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
    {
        outSelections[candidate].EmitterIndex = SampleSceneRandom(queries, 0u, emitterCount);
        outSelections[candidate].SelectionPmf = 1.0f / float(emitterCount);
    }
#else
    if (emitterCount <= INITIAL_EMITTER_ALIAS_CAPACITY)
    {
        return TrySelectInitialEmitterAlias(scene, receiver, emitterCount, queries, outSelections);
    }
    float totalWeight = 0;
    [loop] for (uint emitter = 0; emitter < emitterCount; ++emitter)
    {
        // This can visit every triangle of the emitter. Evaluate it once per receiver,
        // rather than once for each initial candidate. Storage is bounded by candidates,
        // so large emitter sets require no fixed-size cache or changed proposal fallback.
        const float weight = ComputeSceneEmitterWeight(scene, emitter, receiver);
        if (isfinite(weight) == false || weight < 0)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            return false;
        }
        if (weight == 0)
        {
            continue;
        }
        totalWeight += weight;
        if (isfinite(totalWeight) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            return false;
        }
        const float replacementProbability = weight / totalWeight;
        [unroll] for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
        {
            const float randomValue = SampleSceneRandom(queries);
            if (queries.HasFailed == true)
            {
                return false;
            }
            if (randomValue < replacementProbability)
            {
                outSelections[candidate].EmitterIndex = emitter;
                // Retain the selected weight until the complete stream's normalization.
                outSelections[candidate].SelectionPmf = weight;
            }
        }
    }
    if (totalWeight <= 0)
    {
        return false;
    }
    [unroll] for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
    {
        outSelections[candidate].SelectionPmf /= totalWeight;
    }
#endif
    return queries.HasFailed == false;
}
