#include "caustic_evaluation.slang"

// Raw linear radiance: current emission/direct lighting, the resampled final gather
// and the separately resampled backprojected caustics. Accumulate and ToneMap are
// ordinary editable graph nodes; no denoiser is used to establish correctness.
float3 ShadeGuidedFg(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgCausticCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    FgPathReservoir reservoir = LoadFgPath(false, GetFgPixel(launch));
    if (IsCurrentFgPath(reservoir, scene, launch, queries) == false)
        return float3(0);
    FgCausticReservoir caustic = LoadFgCaustic(launch, false);
    if (caustic.Version != FG_VERSION)
    {
        FailSceneQuery(queries, caustic.Confidence);
        return float3(0);
    }
    float3 emission;
    uint4 key = uint4(launch.Seed, launch.SampleIndex, GetFgPixel(launch));
    FgPoint receiver = TraceFgCamera(scene, launch, key, queries, emission);
    float3 radiance = emission;
    if (receiver.IsValid == true)
        radiance += EstimateFgDirect(scene, launch, receiver, queries);
    FgPoint reservoirReceiver = LoadFgReceiver(scene, reservoir, queries);
    radiance += EvaluateFgPath(scene, reservoir, reservoirReceiver, false, queries) * reservoir.Weight;
    radiance += EvaluateFgCaustic(scene, launch, caustic.Photon, queries) * caustic.Weight;
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    return radiance;
}
