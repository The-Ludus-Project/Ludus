#include "pairwise_common.slang"

float3 ShadeStochasticReservoir(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasReservoirCapacity(launch) == false || HasPairwiseCellCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, launch.CameraRay, hit);
    if (status != SCENE_QUERY_SURFACE)
    {
        if (status == SCENE_QUERY_MISS)
        {
            Reservoir empty = (Reservoir)0;
            StoreReservoir(queries.RandomPixel, true, empty);
        }
        return float3(0);
    }
    Reservoir current = LoadReservoir(queries.RandomPixel, false);
    Reservoir result = MakeReceiverReservoir(scene, launch, hit);
    if (HasMatchingIdentity(current, scene, launch) == false || HasMatchingTemporalReceiver(current, result) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    float3 radiance = EvaluateEmission(scene, hit, result.ViewDirectionWorld);
    if (current.CandidateCount == 0)
    {
        StoreReservoir(queries.RandomPixel, true, result);
        return radiance;
    }
    uint2 extentPixel = GetPairwiseTileExtent(launch);
    uint neighborCount = extentPixel.x * extentPixel.y - 1;
    uint canonicalSlot = GetPairwiseCanonicalSlot(launch);
    uint4 cell = LoadPairwiseCell(launch, queries.RandomPixel);
    float canonicalTarget = asfloat(cell.y);
    if (cell.z < current.CandidateCount || isfinite(canonicalTarget) == false || canonicalTarget <= 0 ||
        isfinite(asfloat(cell.w)) == false || asfloat(cell.w) < 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    float canonicalConfidence = float(current.CandidateCount);
    float confidenceScale = neighborCount > 0 ? min(float(RESTIR_PAIRWISE_SAMPLES) / float(neighborCount), 1) : 0;
    float confidenceSum = float(cell.z - current.CandidateCount) * confidenceScale;
    float canonicalMis = 1;
    if (confidenceSum > 0)
    {
        canonicalMis = canonicalConfidence / (confidenceSum + canonicalConfidence);
        [loop] for (uint draw = 0; draw < RESTIR_PAIRWISE_CANONICAL_SAMPLES; ++draw)
        {
            uint slot = SamplePairwiseIndex(launch, draw, neighborCount, queries);
            slot += slot >= canonicalSlot ? 1 : 0;
            Reservoir neighbor = LoadReservoir(GetPairwisePixel(launch, slot), false);
            if (HasMatchingIdentity(neighbor, scene, launch) == false || neighbor.CandidateCount == 0)
                continue;
            SurfaceHit receiver = GetPairwiseReceiver(scene, neighbor);
            float target = EvaluateSampleTarget(scene, receiver, current.TriangleIndex, current.Barycentric);
            canonicalMis += float(neighborCount) / float(RESTIR_PAIRWISE_CANONICAL_SAMPLES) *
                            EvaluatePairwiseCanonicalBeta(float(neighbor.CandidateCount) * confidenceScale,
                                                          confidenceSum,
                                                          canonicalConfidence,
                                                          target,
                                                          canonicalTarget);
        }
    }
    float weightSum = 0;
    const uint CANONICAL_STREAM_DIMENSION = 32;
    StreamReservoir(current.TriangleIndex,
                    current.Barycentric,
                    canonicalMis * canonicalTarget * current.Weight,
                    0,
                    SampleReuseRandom(launch, CANONICAL_STREAM_DIMENSION),
                    result,
                    weightSum,
                    queries);
    if (confidenceSum > 0)
    {
        // Subtract the actual CDF increment, matching the implemented distribution.
        float totalMass = max(asfloat(cell.w) - GetPairwiseMass(launch, canonicalSlot), 0);
        const uint NEIGHBOR_RANDOM_BASE = 64;
        const uint DIMENSIONS_PER_DRAW = 4;
        [loop] for (uint draw = 0; draw < RESTIR_PAIRWISE_SAMPLES; ++draw)
        {
            uint dimension = NEIGHBOR_RANDOM_BASE + draw * DIMENSIONS_PER_DRAW;
            float probability;
            uint slot = SamplePairwiseNeighbor(launch, dimension, canonicalSlot, totalMass, probability, queries);
            uint pixel = GetPairwisePixel(launch, slot);
            Reservoir neighbor = LoadReservoir(pixel, false);
            if (HasMatchingIdentity(neighbor, scene, launch) == false || neighbor.CandidateCount == 0)
                continue;
            float sourceTarget = asfloat(LoadPairwiseCell(launch, pixel).y);
            float destinationTarget = EvaluateSampleTarget(scene, hit, neighbor.TriangleIndex, neighbor.Barycentric);
            float mis = EvaluatePairwiseNeighborMis(float(neighbor.CandidateCount) * confidenceScale,
                                                    confidenceSum,
                                                    canonicalConfidence,
                                                    sourceTarget,
                                                    destinationTarget);
            float weight = mis * destinationTarget * neighbor.Weight / (float(RESTIR_PAIRWISE_SAMPLES) * probability);
            StreamReservoir(neighbor.TriangleIndex,
                            neighbor.Barycentric,
                            weight,
                            0,
                            SampleReuseRandom(launch, dimension + DIMENSIONS_PER_DRAW - 1),
                            result,
                            weightSum,
                            queries);
        }
    }
    // Equation 7: UCW = sum(w) / target(Y), with NO additional M division.
    result.CandidateCount = current.CandidateCount;
    if (result.TriangleIndex != SCENE_INVALID_INDEX)
        result.Weight = weightSum / EvaluateSampleTarget(scene, hit, result.TriangleIndex, result.Barycentric);
    if (isfinite(result.Weight) == false || result.Weight < 0)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
    radiance += ShadeReservoir(scene, hit, result, queries);
    if (queries.HasFailed == false)
        StoreReservoir(queries.RandomPixel, true, result);
    return radiance;
}
