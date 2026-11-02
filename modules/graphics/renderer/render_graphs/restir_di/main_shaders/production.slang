#include "production_reservoir.slang"

ProductionReservoir
MakeProductionCandidate(SceneContext scene, RayLaunch launch, SurfaceHit receiver, inout RayQueryContext queries)
{
    ProductionReservoir result = (ProductionReservoir)0;
    result.Value = MakeReceiverReservoir(scene, launch, receiver);
    result.VisibilityReceiverWorld = receiver.PositionWorld;
    const uint2 tilePixel = launch.PositionPixel / PRODUCTION_TILE_WIDTH;
    const uint tilesWide = (launch.ExtentPixel.x + PRODUCTION_TILE_WIDTH - 1) / PRODUCTION_TILE_WIDTH;
    const uint tile = tilePixel.y * tilesWide + tilePixel.x;
    RandomAddress poolAddress = MakeRandomAddress(uint2(tile, GetGraphPassIndex()), launch.SampleIndex, 0);
    const uint pool = SampleProductionIndex(launch, poolAddress, PRODUCTION_POOL_COUNT, queries);
    float weightSum = 0;
    [loop] for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
    {
        const uint dimension = PRODUCTION_CANDIDATE_DIMENSION_FIRST + candidate * PRODUCTION_CANDIDATE_DIMENSION_COUNT;
        const uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
        RandomAddress address = MakeRandomAddress(uint2(pixel, GetGraphPassIndex()), launch.SampleIndex, dimension);
        const uint offset = SampleProductionIndex(launch, address, PRODUCTION_POOL_SAMPLES, queries);
        const uint4 encoded = GraphBuffer0[pool * PRODUCTION_POOL_SAMPLES + offset];
        const float inversePdfArea = asfloat(encoded.w);
        if (isfinite(inversePdfArea) == false || inversePdfArea < 0)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
            break;
        }
        const float weight =
            encoded.x == SCENE_INVALID_INDEX
                ? 0
                : EvaluateSampleTarget(scene, receiver, encoded.x, asfloat(encoded.yz)) * inversePdfArea;
        StreamReservoir(encoded.x,
                        asfloat(encoded.yz),
                        weight,
                        1,
                        SampleReuseRandom(launch, dimension + 1),
                        result.Value,
                        weightSum,
                        queries);
    }
    FinalizeReservoir(scene, receiver, weightSum, result.Value, queries);
    result.IsVisible = TestProductionVisibility(scene, receiver, result.Value, queries);
    // Section 7.2: only fresh-candidate visibility enters the reservoir weights. Never multiply the temporal or
    // spatiotemporal reservoir weight by a new visibility result, which would compound darkening bias.
    if (result.IsVisible == false)
        result.Value.Weight = 0;
    return result;
}

void MergeProductionCandidate(SceneContext scene,
                              SurfaceHit receiver,
                              ProductionReservoir source,
                              float randomValue,
                              inout ProductionReservoir inoutResult,
                              inout float inoutWeightSum,
                              inout RayQueryContext queries)
{
    float weight = 0;
    if (source.Value.TriangleIndex != SCENE_INVALID_INDEX && source.Value.CandidateCount > 0)
        weight = EvaluateSampleTarget(scene, receiver, source.Value.TriangleIndex, source.Value.Barycentric) *
                 source.Value.Weight * float(source.Value.CandidateCount);
    StreamReservoir(source.Value.TriangleIndex,
                    source.Value.Barycentric,
                    weight,
                    source.Value.CandidateCount,
                    randomValue,
                    inoutResult.Value,
                    inoutWeightSum,
                    queries);
    if (weight > 0 && randomValue < weight / inoutWeightSum)
    {
        inoutResult.IsVisible = source.IsVisible;
        inoutResult.VisibilityReceiverWorld = source.VisibilityReceiverWorld;
    }
}

ProductionReservoir
MakeProductionSpatial(SceneContext scene, RayLaunch launch, SurfaceHit receiver, inout RayQueryContext queries)
{
    ProductionReservoir result = (ProductionReservoir)0;
    result.Value = MakeReceiverReservoir(scene, launch, receiver);
    float weightSum = 0;
    // Section 6.4: one tap by default, adjustable up to four. Neighbours come exclusively from the other bank,
    // never a partially updated current-frame reservoir. Sampling with replacement needs no mutable neighbour state.
    [loop] for (uint i = 0; i < RESTIR_PRODUCTION_SPATIAL_NEIGHBORS; ++i)
    {
        const uint AXIAL_NEIGHBOR_COUNT = 4;
        const uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
        RandomAddress address = MakeRandomAddress(uint2(pixel, GetGraphPassIndex()),
                                                  launch.SampleIndex,
                                                  PRODUCTION_NEIGHBOR_DIMENSION_FIRST + 2 * i);
        const uint direction = SampleProductionIndex(launch, address, AXIAL_NEIGHBOR_COUNT, queries);
        const int2 offsetPixel = direction < 2 ? int2(direction == 0 ? -1 : 1, 0) : int2(0, direction == 2 ? -1 : 1);
        const int2 positionPixel = int2(launch.PositionPixel) + offsetPixel;
        if (any(positionPixel < 0) == true || any(positionPixel >= int2(launch.ExtentPixel)) == true)
            continue;
        ProductionReservoir neighbor =
            LoadProductionHistory(launch, uint(positionPixel.y) * launch.ExtentPixel.x + uint(positionPixel.x));
        if (HasProductionHistory(neighbor, scene, launch) == false ||
            CanReuseSpatialReceiver(scene, receiver, neighbor.Value) == false)
            continue;
        neighbor.VisibilityReceiverWorld =
            GetSurfaceHit(scene, neighbor.Value.ReceiverTriangleIndex, neighbor.Value.ReceiverBarycentric)
                .PositionWorld;
        MergeProductionCandidate(scene,
                                 receiver,
                                 neighbor,
                                 SampleReuseRandom(launch, address.Dimension + 1),
                                 result,
                                 weightSum,
                                 queries);
    }
    FinalizeReservoir(scene, receiver, weightSum, result.Value, queries);
    if (result.Value.TriangleIndex != SCENE_INVALID_INDEX && result.Value.Weight > 0)
    {
        // Section 7.3's quality/cost knob: only shading reuses a nearby source's visibility. A zero threshold
        // reuses it solely for an identical world-space receiver; other spatial samples get a fresh shadow query.
        const float3 differenceWorld = result.VisibilityReceiverWorld - receiver.PositionWorld;
        const bool canReuseVisibility =
            dot(differenceWorld, differenceWorld) <=
            RESTIR_PRODUCTION_VISIBILITY_DISTANCE_WORLD * RESTIR_PRODUCTION_VISIBILITY_DISTANCE_WORLD;
        if (canReuseVisibility == false)
            result.IsVisible = TestProductionVisibility(scene, receiver, result.Value, queries);
    }
    return result;
}

// Sections 6.2 and 7: one pixel kernel selects new candidates, reads last-frame history, shades all three groups,
// and writes only its own new reservoir. Presampling is a separate coherent pass, independent of receiver reuse.
float3 ShadeProductionDi(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasProductionCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    ProductionReservoir result = (ProductionReservoir)0;
    result.Value.TriangleIndex = SCENE_INVALID_INDEX;
    result.Value.ReceiverTriangleIndex = SCENE_INVALID_INDEX;
    result.Value.SceneGeneration = scene.Generation;
    result.Value.ExtentPixel = launch.ExtentPixel;
    float3 radiance = float3(0);
    SurfaceHit receiver;
    if (TraceClosest(scene, queries, launch.CameraRay, receiver) == SCENE_QUERY_SURFACE)
    {
        result.Value = MakeReceiverReservoir(scene, launch, receiver);
        radiance = EvaluateEmission(scene, receiver, result.Value.ViewDirectionWorld);
        if (any(radiance > 0) == false)
        {
            ProductionReservoir candidate = MakeProductionCandidate(scene, launch, receiver, queries);
            const uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
            ProductionReservoir temporal = LoadProductionHistory(launch, pixel);
            if (IS_RESTIR_TEMPORAL_ENABLED == false || HasProductionHistory(temporal, scene, launch) == false ||
                HasMatchingTemporalReceiver(temporal.Value, result.Value) == false)
                temporal = (ProductionReservoir)0;
            ProductionReservoir spatial = MakeProductionSpatial(scene, launch, receiver, queries);
            // The conditional expectation of shading a resampled sample: its selection probability times the
            // finalized RIS estimate. With Z=sum(M), this simplifies to sum(f_i V_i W_i M_i)/sum(M_i).
            // It shades candidates even when the reservoir coin discards them, without an extra ray per input tap.
            const uint count =
                candidate.Value.CandidateCount + temporal.Value.CandidateCount + spatial.Value.CandidateCount;
            if (count > 0)
            {
                radiance +=
                    (EvaluateProductionShading(scene, receiver, candidate) * float(candidate.Value.CandidateCount) +
                     EvaluateProductionShading(scene, receiver, temporal) * float(temporal.Value.CandidateCount) +
                     EvaluateProductionShading(scene, receiver, spatial) * float(spatial.Value.CandidateCount)) /
                    float(count);
            }
            float weightSum = 0;
            MergeProductionCandidate(scene,
                                     receiver,
                                     candidate,
                                     SampleReuseRandom(launch, PRODUCTION_MERGE_DIMENSION_FIRST),
                                     result,
                                     weightSum,
                                     queries);
            MergeProductionCandidate(scene,
                                     receiver,
                                     temporal,
                                     SampleReuseRandom(launch, PRODUCTION_MERGE_DIMENSION_FIRST + 1),
                                     result,
                                     weightSum,
                                     queries);
            MergeProductionCandidate(scene,
                                     receiver,
                                     spatial,
                                     SampleReuseRandom(launch, PRODUCTION_MERGE_DIMENSION_FIRST + 2),
                                     result,
                                     weightSum,
                                     queries);
            FinalizeReservoir(scene, receiver, weightSum, result.Value, queries);
        }
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    StoreProductionHistory(launch, result, queries);
    return radiance;
}
