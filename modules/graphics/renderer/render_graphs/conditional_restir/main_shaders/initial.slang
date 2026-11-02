#include "reservoir.slang"

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
        support = TracePrefix(scene, launch, primary, queries);
    if (support.IsValid == true)
    {
        result.SupportTriangleIndex = support.Hit.TriangleIndex;
        result.SupportBarycentric = support.Hit.Barycentric;
        result.SupportViewWorld = support.ViewWorld;
    }
    float massSum = 0;
    uint emitterCount = GetEmitterCount(scene);
    [loop] for (uint candidate = 0; emitterCount > 0 && candidate < CRIS_INITIAL_CANDIDATES; ++candidate)
    {
        ++result.CandidateCount;
        uint dimension = PREFIX_DIMENSIONS + candidate * CANDIDATE_DIMENSIONS;
        uint emitter = SamplePassIndex(launch, dimension, emitterCount, queries);
        uint triangleCount = uint(GetEmitter(scene, emitter).y);
        if (triangleCount == 0)
            continue;
        TriangleData triangle =
            GetEmitterTriangle(scene, emitter, SamplePassIndex(launch, dimension + 1, triangleCount, queries));
        if (triangle.IsValid == false || triangle.AreaWorld <= 0)
            continue;
        float root = sqrt(SamplePassRandom(launch, dimension + 2));
        const uint BARYCENTRIC_DIMENSION = 3;
        const uint COIN_DIMENSION = 4;
        float2 barycentric = float2(root * (1 - SamplePassRandom(launch, dimension + BARYCENTRIC_DIMENSION)),
                                    root * SamplePassRandom(launch, dimension + BARYCENTRIC_DIMENSION));
        float target = EvaluateTarget(scene, support, triangle.TriangleIndex, barycentric, queries);
        float inverseDensityArea = float(emitterCount) * float(triangleCount) * triangle.AreaWorld;
        StreamCandidate(triangle.TriangleIndex,
                        barycentric,
                        ComputeConditionalMass(target, inverseDensityArea, 1),
                        0,
                        SamplePassRandom(launch, dimension + COIN_DIMENSION),
                        result,
                        massSum,
                        queries);
    }
    FinalizeReservoir(scene, support, massSum, result, queries);
    StoreReservoir(launch, false, result, queries);
    return float3(0);
}
