#include "initial_sampling.slang"
#include "shading.slang"

// Thanks to Chris Wyman and Alexey Panteleev, "Rearchitecting Spatiotemporal Resampling for Production",
// High-Performance Graphics 2021, sections 6.2 and 7.1, https://doi.org/10.2312/hpg.20211281 .
// Adapted previous-frame neighbour reuse and decoupled shading to Ouyang et al.'s GI estimator. Light-pool
// presampling and approximate DI visibility do not transfer to receiver-dependent GI paths. Each candidate retains
// the GI Jacobian and, in unbiased mode, its own Algorithm 4 count Z. Shading evaluates the conditional expectation
// of the same reservoir estimator instead of applying DI's sum(M) shortcut to GI's visibility-dependent Z.
static const uint PRODUCTION_HISTORY_BANK_COUNT = 2;
static const uint PRODUCTION_CANDIDATE_COUNT = 3;
static const uint PRODUCTION_INITIAL_CANDIDATE = 0;
static const uint PRODUCTION_TEMPORAL_CANDIDATE = 1;
static const uint PRODUCTION_SPATIAL_CANDIDATE = 2;
// The inline path, reservoir coins, neighbour offset and direct lighting use disjoint random dimensions.
static const uint PRODUCTION_RESAMPLING_DIMENSION_FIRST =
    INITIAL_PATH_DIMENSION_FIRST + (RESTIR_GI_PATH_BOUNCES + 1) * VERTEX_DIMENSION_COUNT;
static const uint PRODUCTION_OFFSET_DIMENSION_FIRST =
    PRODUCTION_RESAMPLING_DIMENSION_FIRST + PRODUCTION_CANDIDATE_COUNT;

uint GetProductionGiAddress(RayLaunch launch, uint pixel, bool isPrevious)
{
    const uint bank = (launch.SampleIndex + (isPrevious == true ? 1u : 0u)) % PRODUCTION_HISTORY_BANK_COUNT;
    return bank * launch.ExtentPixel.x * launch.ExtentPixel.y + pixel;
}

Reservoir LoadProductionGiHistory(SceneContext scene, RayLaunch launch, uint pixel)
{
    Reservoir source = LoadReservoir(FIRST_BUFFER, GetProductionGiAddress(launch, pixel, true));
    if (launch.SampleIndex == 0 || source.SampleIndex != launch.SampleIndex - 1 ||
        HasMatchingIdentity(source, scene, launch, RESTIR_GI_SPATIAL_MAX_M) == false)
    {
        return MakeEmptyReservoir(scene, launch);
    }
    // One history now feeds both temporal and spatial reuse. Bound each borrowed reservoir to the temporal M
    // without rescaling W, as in the existing GI policy. The newly combined reservoir keeps the spatial cap.
    source.CandidateCount = min(source.CandidateCount, RESTIR_GI_TEMPORAL_MAX_M);
    return source;
}

void StoreProductionGiHistory(RayLaunch launch, Reservoir reservoir, RayQueryContext queries)
{
    reservoir.HasFailed = queries.HasFailed;
    reservoir.FailureReason = queries.FailureReason;
    if (reservoir.HasFailed == true)
    {
        reservoir.Sample.TriangleIndex = SCENE_INVALID_INDEX;
        reservoir.Weight = 0;
        reservoir.CandidateCount = 0;
    }
    StoreReservoir(FIRST_BUFFER, GetProductionGiAddress(launch, GetPixelIndex(launch), false), reservoir);
}

Reservoir LoadProductionGiNeighbor(SceneContext scene, RayLaunch launch, Receiver receiver)
{
    Reservoir empty = MakeEmptyReservoir(scene, launch);
    if (IS_RESTIR_GI_SPATIAL_ENABLED == false)
        return empty;
    // Section 6.4's one spatiotemporal tap; retain GI's disk and geometric similarity test. Reading the previous
    // bank removes the dependency on the current frame's temporal pass, including at disocclusions.
    const float radiusPixels =
        max(SPATIAL_INITIAL_RADIUS_FRACTION * float(max(launch.ExtentPixel.x, launch.ExtentPixel.y)),
            SPATIAL_MINIMUM_RADIUS_PIXELS);
    const float distancePixels = radiusPixels * sqrt(SampleGiRandom(launch, PRODUCTION_OFFSET_DIMENSION_FIRST));
    const float angle = 2 * PI * SampleGiRandom(launch, PRODUCTION_OFFSET_DIMENSION_FIRST + 1);
    const int2 offsetPixel = int2(round(float2(cos(angle), sin(angle)) * distancePixels));
    const int2 positionPixel = int2(launch.PositionPixel) + offsetPixel;
    if (all(offsetPixel == 0) == true || any(positionPixel < 0) == true ||
        any(positionPixel >= int2(launch.ExtentPixel)) == true)
        return empty;
    Reservoir source =
        LoadProductionGiHistory(scene, launch, uint(positionPixel.y) * launch.ExtentPixel.x + uint(positionPixel.x));
    Receiver sourceReceiver = LoadReceiver(scene, source.ReceiverTriangleIndex, source.ReceiverBarycentric);
    return AreReceiversSimilar(launch, receiver, sourceReceiver) == true ? source : empty;
}

float3 ShadeProductionGi(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasRecordCapacity(launch, RESERVOIR_RECORD_WORDS * PRODUCTION_HISTORY_BANK_COUNT, 0) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    Reservoir result = MakeEmptyReservoir(scene, launch);
    float3 radiance = float3(0);
    Receiver receiver;
    if (TryTraceReceiver(scene, launch, queries, receiver) == true)
    {
        ShadingPoint shading;
        TargetContext target;
        if (TryMakeShadingPoint(scene, receiver.Hit, -launch.CameraRay.DirectionWorld, queries, shading) == true &&
            TryMakeTargetContext(scene, receiver, queries, target) == true)
        {
            SampleRecord initial = MakeInitialGiSample(scene, launch, receiver, queries);
            result.ReceiverTriangleIndex = receiver.Hit.TriangleIndex;
            result.ReceiverBarycentric = receiver.Hit.Barycentric;
            radiance = EvaluateEmission(scene, receiver.Hit, shading.ViewDirectionWorld) +
                       EstimateDirectLighting(scene, launch, shading, PRODUCTION_OFFSET_DIMENSION_FIRST + 2, queries);
            Reservoir candidates[PRODUCTION_CANDIDATE_COUNT];
            Receiver receivers[PRODUCTION_CANDIDATE_COUNT];
            [unroll] for (uint i = 0; i < PRODUCTION_CANDIDATE_COUNT; ++i)
            {
                candidates[i] = MakeEmptyReservoir(scene, launch);
                receivers[i] = receiver;
            }
            candidates[PRODUCTION_INITIAL_CANDIDATE] = result;
            candidates[PRODUCTION_INITIAL_CANDIDATE].Sample = initial.Sample;
            candidates[PRODUCTION_INITIAL_CANDIDATE].CandidateCount = 1;
            if (initial.Sample.TriangleIndex != SCENE_INVALID_INDEX)
            {
                if (isfinite(initial.SourcePdf) == false || initial.SourcePdf <= 0)
                    FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
                else
                    candidates[PRODUCTION_INITIAL_CANDIDATE].Weight = 1 / initial.SourcePdf;
            }
            if (IS_RESTIR_GI_TEMPORAL_ENABLED == true)
            {
                Reservoir temporal = LoadProductionGiHistory(scene, launch, GetPixelIndex(launch));
                Receiver previousReceiver =
                    LoadReceiver(scene, temporal.ReceiverTriangleIndex, temporal.ReceiverBarycentric);
                if (AreReceiversSimilar(launch, receiver, previousReceiver) == true)
                {
                    candidates[PRODUCTION_TEMPORAL_CANDIDATE] = temporal;
                    receivers[PRODUCTION_TEMPORAL_CANDIDATE] = previousReceiver;
                }
            }
            candidates[PRODUCTION_SPATIAL_CANDIDATE] = LoadProductionGiNeighbor(scene, launch, receiver);
            receivers[PRODUCTION_SPATIAL_CANDIDATE] =
                LoadReceiver(scene,
                             candidates[PRODUCTION_SPATIAL_CANDIDATE].ReceiverTriangleIndex,
                             candidates[PRODUCTION_SPATIAL_CANDIDATE].ReceiverBarycentric);
            float weightSum = 0;
            uint selected = CANDIDATE_NONE;
            float weights[PRODUCTION_CANDIDATE_COUNT];
            uint counts[PRODUCTION_CANDIDATE_COUNT];
            [unroll] for (uint i = 0; i < PRODUCTION_CANDIDATE_COUNT; ++i)
            {
                weights[i] = ComputeShiftedWeight(scene,
                                                  target,
                                                  receivers[i],
                                                  candidates[i],
                                                  AreSameReceiver(candidates[i], receiver),
                                                  queries);
                StreamCandidate(candidates[i].Sample,
                                weights[i],
                                candidates[i].CandidateCount,
                                i,
                                SampleGiRandom(launch, PRODUCTION_RESAMPLING_DIMENSION_FIRST + i),
                                result,
                                weightSum,
                                selected,
                                queries);
                counts[i] = 0;
            }
            // Rao-Blackwellize the selection coin: each candidate is shaded using its own visibility-dependent Z.
            // Reuse still selects exactly one sample. Rejected/zero-weight candidates retain their M in Z wherever
            // their source receiver could have generated the shaded sample, just as Algorithm 4 requires.
            [unroll] for (uint i = 0; i < PRODUCTION_CANDIDATE_COUNT; ++i)
            {
                if (weights[i] <= 0)
                    continue;
                SurfaceHit samplePoint =
                    GetSurfaceHit(scene, candidates[i].Sample.TriangleIndex, candidates[i].Sample.Barycentric);
                [unroll] for (uint j = 0; j < PRODUCTION_CANDIDATE_COUNT; ++j)
                {
                    if (candidates[j].CandidateCount == 0)
                        continue;
                    if (IS_RESTIR_GI_UNBIASED == false || j == i || AreSameReceiver(candidates[j], receiver) == true ||
                        CanGenerateSample(scene, receivers[j], samplePoint, true, queries) == true)
                    {
                        counts[i] += candidates[j].CandidateCount;
                    }
                }
                const float targetValue = EvaluateTarget(target, candidates[i].Sample, samplePoint);
                if (counts[i] > 0 && targetValue > 0)
                {
                    radiance += EvaluateGiIndirectContribution(scene, shading, candidates[i].Sample, queries) *
                                (weights[i] / (float(counts[i]) * targetValue));
                }
            }
            uint normalizationCount = 0;
            if (selected < PRODUCTION_CANDIDATE_COUNT)
                normalizationCount = counts[selected];
            FinalizeReservoir(scene, target, weightSum, normalizationCount, RESTIR_GI_SPATIAL_MAX_M, result, queries);
        }
    }
    else if (queries.HasFailed == false)
    {
        radiance = EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    StoreProductionGiHistory(launch, result, queries);
    return radiance;
}
