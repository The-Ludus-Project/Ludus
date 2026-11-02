#include "caustic_evaluation.slang"

// Directly visible caustics use photon-to-camera splats, not a radius estimate.
// Their reservoir is temporal only. Under the admitted static history, the camera
// reprojection is identity and the confidence interpolation reduces to this pixel's
// previous confidence, independently of how many photons reached its linked list.
float3 ResampleCaustics(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgCausticCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    FgCausticReservoir result = (FgCausticReservoir)0;
    result.Photon.Triangle = FG_INVALID;
    result.Confidence = 1;
    result.SampleIndex = launch.SampleIndex + 1;
    result.Version = FG_VERSION;
    result.Identity = uint4(scene.Generation, launch.ExtentPixel);
    if (HasCurrentFgPool(scene, launch, queries) == true)
    {
        uint index = GraphBuffer0[FG_HEAD_OFFSET + GetFgPixel(launch)].y;
        float weightSum = 0;
        float selectedTarget = 0;
        uint visits = 0;
        while (index != FG_INVALID)
        {
            if (index >= FG_PHOTON_CAPACITY || visits++ >= FG_PHOTON_CAPACITY)
            {
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
                break;
            }
            uint photonIndex = index;
            FgPhoton photon = LoadFgPhoton(false, FG_PHOTON_OFFSET + index * FG_PHOTON_WORDS);
            index = photon.NextCaustic;
            float target = ComputeFgLuminance(EvaluateFgCaustic(scene, launch, photon, queries));
            if (isfinite(target) == false || target < 0)
            {
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
                break;
            }
            weightSum += target;
            if (target > 0 && SampleRandomFloat01(launch.Seed,
                                                  MakeRandomAddress(uint2(GetFgPixel(launch), photonIndex + 1),
                                                                    launch.SampleIndex,
                                                                    FG_RESAMPLE_FIRST_DIMENSION + 2)) *
                                      weightSum <
                                  target)
            {
                result.Photon = photon;
                selectedTarget = target;
            }
        }
        result.Weight = selectedTarget > 0 ? weightSum / selectedTarget : 0;
        FgCausticReservoir history = LoadFgCaustic(launch, true);
        if (IS_FG_TEMPORAL_ENABLED == true && launch.SampleIndex > 0 && history.Version == FG_VERSION &&
            history.SampleIndex == launch.SampleIndex && all(history.Identity == result.Identity) == true &&
            (history.Photon.Triangle == FG_INVALID || all(history.Photon.RandomKey.xy == launch.Seed) == true))
        {
            uint confidence = min(history.Confidence, FG_HISTORY_LIMIT);
            float historyTarget = ComputeFgLuminance(EvaluateFgCaustic(scene, launch, history.Photon, queries));
            float totalConfidence = float(confidence + 1);
            float currentWeight = selectedTarget * result.Weight / totalConfidence;
            float historyWeight = historyTarget * history.Weight * float(confidence) / totalConfidence;
            float sum = currentWeight + historyWeight;
            if (historyWeight > 0 && SampleFgRandom(launch, FG_RESAMPLE_FIRST_DIMENSION) * sum < historyWeight)
            {
                result.Photon = history.Photon;
                selectedTarget = historyTarget;
            }
            result.Weight = selectedTarget > 0 ? sum / selectedTarget : 0;
            result.Confidence = min(confidence + 1, FG_HISTORY_LIMIT);
        }
        RecordFgGuiding(result.Photon, EvaluateFgCaustic(scene, launch, result.Photon, queries) * result.Weight);
    }
    if (isfinite(result.Weight) == false || result.Weight < 0)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    if (queries.HasFailed == true)
    {
        result.Version = 0;
        result.Weight = 0;
        result.Confidence = queries.FailureReason;
    }
    StoreFgCaustic(launch, result);
    return float3(0);
}
