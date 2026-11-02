#include "reservoir.slang"

float3 ShadeConditionalRestir(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasCapacity(launch, false) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    Reservoir reservoir = LoadReservoir(GetPixelIndex(launch), false);
    if (IsCurrentRecord(reservoir, scene, launch, queries) == false)
        return float3(0);
    SurfaceHit primary;
    uint status = TraceClosest(scene, queries, launch.CameraRay, primary);
    if (status == SCENE_QUERY_MISS)
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    if (status != SCENE_QUERY_SURFACE)
        return float3(0);
    Prefix direct = (Prefix)0;
    direct.Hit = primary;
    direct.ViewWorld = -launch.CameraRay.DirectionWorld;
    direct.Throughput = float3(1);
    direct.MaterialRandom = MakeMaterialRandom(launch, DIRECT_MATERIAL_EVENT);
    direct.IsValid = true;
    float3 radiance = EvaluateEmission(scene, primary, direct.ViewWorld);
    // With a delayed gather, the primary owns only direct mesh lighting. All higher scattering
    // orders belong to the secondary prefix, preventing overlap with its variable-length suffix.
    if (CRIS_PREFIX_BOUNCES == 0 || reservoir.SuffixBounces == 0)
        radiance += GatherSuffix(scene, direct, reservoir, queries);
    if (CRIS_PREFIX_BOUNCES > 0)
    {
        // Independent of the supporting prefixes AND the reservoir coins. The joint UCW is the
        // stored conditional area weight times this new prefix's f*cos/pdf throughput (Equation 22).
        Prefix gather = TracePrefix(scene, launch, primary, queries);
        radiance += GatherSuffix(scene, gather, reservoir, queries);
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    return radiance;
}
