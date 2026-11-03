#include "tail.slang"

float3 GenerateConditionalSample(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasCapacity(launch, false) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    Reservoir result = MakeEmptyReservoir(scene, launch);
    SurfaceHit primary;
    Prefix support = (Prefix)0;
    if (TraceClosest(scene, queries, launch.CameraRay, primary) == SCENE_QUERY_SURFACE)
    {
        // Smooth glass is followed to the first surface a prefix can scatter from; the shade pass draws the same
        // chain from the same default dimensions.
        SpecularChain chain =
            FollowSpecularChain(scene, launch, queries, primary, SCENE_SPECULAR_CHAIN_DEFAULT_DIMENSION_FIRST);
        if (chain.IsSurface == true)
            support = TracePrefix(scene, launch, chain.Hit, queries);
    }
    if (support.IsValid == true)
    {
        result.SupportTriangleIndex = support.Hit.TriangleIndex;
        result.SupportBarycentric = support.Hit.Barycentric;
        result.SupportViewWorld = support.ViewWorld;
    }
    // One canonical suffix samples a uniformly chosen term of the scattering series. Conditional
    // on length zero, endpoint RIS reduces light-sampling noise. Other lengths trace one tail only,
    // keeping the worst-case query budget independent of the endpoint candidate count.
    uint suffixBounces = SamplePassIndex(launch, SUFFIX_DEPTH_DIMENSION, CRIS_SUFFIX_BOUNCES + 1, queries);
    float massSum = 0;
    if (suffixBounces == 0)
    {
        [loop] for (uint candidate = 0; candidate < CRIS_INITIAL_CANDIDATES; ++candidate)
        {
            ++result.CandidateCount;
            uint dimension = SUFFIX_DIMENSION_FIRST + candidate * CANDIDATE_DIMENSIONS;
            SurfaceHit endpoint;
            float inverseDensityArea;
            if (TrySampleMeshEndpoint(scene, launch, dimension, queries, endpoint, inverseDensityArea) == false)
                continue;
            Reservoir suffix = MakeEmptyReservoir(scene, launch);
            suffix.TriangleIndex = endpoint.TriangleIndex;
            suffix.Barycentric = endpoint.Barycentric;
            float target = EvaluateTarget(scene, support, suffix, queries);
            const uint COIN_DIMENSION_OFFSET = 4;
            StreamCandidate(suffix,
                            ComputeConditionalMass(target, inverseDensityArea, 1),
                            0,
                            SamplePassRandom(launch, dimension + COIN_DIMENSION_OFFSET),
                            result,
                            massSum,
                            queries);
        }
    }
    else
    {
        float inverseDensityArea;
        Reservoir suffix = GenerateScatteringSuffix(scene, launch, suffixBounces, queries, inverseDensityArea);
        result.CandidateCount = 1;
        if (all(isfinite(suffix.TailCoefficient)) == false || any(suffix.TailCoefficient < 0) == true)
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        if (suffix.TriangleIndex != SCENE_INVALID_INDEX)
        {
            float target = EvaluateTarget(scene, support, suffix, queries);
            StreamCandidate(suffix, target * inverseDensityArea, 0, 0, result, massSum, queries);
        }
    }
    FinalizeReservoir(scene, support, massSum, result, queries);
    result.Weight *= float(CRIS_SUFFIX_BOUNCES + 1); // Inverse discrete path-length PMF.
    if (isfinite(result.Weight) == false || result.Weight < 0)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
    // The internal endpoint RIS is ONE canonical mixture sample. Its confidence must not depend on
    // the random length, or later M-weighted averages would preferentially weight shorter paths.
    result.CandidateCount = 1;
    StoreReservoir(launch, false, result, queries);
    return float3(0);
}
