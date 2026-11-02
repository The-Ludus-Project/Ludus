#include "path_evaluation.slang"

// Streaming RIS of the photons inside a density estimate, preserving one full photon
// and its original emission random key, UV cell and flux scale (paper section 4.3).
void GatherFgPhotons(SceneContext scene,
                     RayLaunch launch,
                     FgPoint receiver,
                     FgPoint gather,
                     bool isRadiance,
                     float sourceWeight,
                     inout RayQueryContext queries,
                     inout FgPathReservoir inoutReservoir,
                     inout float inoutWeightSum,
                     inout uint inoutCoin)
{
    int3 centerWorld = int3(floor(gather.Hit.PositionWorld / FG_GATHER_RADIUS_WORLD));
    static const int CELL_NEIGHBOR_MINIMUM = -1;
    for (int z = CELL_NEIGHBOR_MINIMUM; z <= 1; ++z)
        for (int y = CELL_NEIGHBOR_MINIMUM; y <= 1; ++y)
            for (int x = CELL_NEIGHBOR_MINIMUM; x <= 1; ++x)
            {
                int3 cellWorld = centerWorld + int3(x, y, z);
                uint bucket = ComputeFgBucket(cellWorld, GetFgPixelCount(launch));
                uint index = GraphBuffer0[FG_HEAD_OFFSET + bucket].x;
                uint visits = 0;
                while (index != FG_INVALID)
                {
                    if (index >= FG_PHOTON_CAPACITY || visits++ >= FG_PHOTON_CAPACITY)
                    {
                        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
                        return;
                    }
                    uint photonIndex = index;
                    FgPhoton photon = LoadFgPhoton(false, FG_PHOTON_OFFSET + index * FG_PHOTON_WORDS);
                    index = photon.NextGrid;
                    SurfaceHit hit = GetSurfaceHit(scene, photon.Triangle, photon.Barycentric);
                    // Distinct queried cells may hash to the same bucket. Test the original
                    // cell before the radius test, so a collision never counts a photon twice.
                    if (hit.IsValid == false ||
                        all(int3(floor(hit.PositionWorld / FG_GATHER_RADIUS_WORLD)) == cellWorld) == false ||
                        (isRadiance == true && (photon.Flags & FG_CAUSTIC_FLAG) == 0))
                        continue;
                    FgPathReservoir candidate = inoutReservoir;
                    candidate.Photon = photon;
                    candidate.GatherTriangle = gather.Hit.TriangleIndex;
                    candidate.GatherBarycentric = gather.Hit.Barycentric;
                    candidate.Flags = isRadiance == true ? FG_RADIANCE_FLAG : 0;
                    float target = ComputeFgLuminance(EvaluateFgPath(scene, candidate, receiver, false, queries));
                    float weight = target * sourceWeight;
                    if (isfinite(weight) == false || weight < 0)
                    {
                        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
                        return;
                    }
                    inoutWeightSum += weight;
                    // Each photon has an independent scope; long lists cannot overflow the dimension field.
                    float coin = SampleRandomFloat01(
                        launch.Seed,
                        MakeRandomAddress(uint2(GetFgPixel(launch), photonIndex + 1),
                                          launch.SampleIndex,
                                          FG_RESAMPLE_FIRST_DIMENSION + (isRadiance == true ? 1 : 0)));
                    ++inoutCoin;
                    if (weight > 0 && coin * inoutWeightSum < weight)
                        inoutReservoir = candidate;
                }
            }
}
float3 GeneratePathSamples(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    FgPathReservoir result = MakeFgPath(scene, launch);
    uint count;
    uint stride;
    GraphBuffer1.GetDimensions(count, stride);
    if (GetFgPixelCount(launch) > count / FG_PATH_WORDS)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    if (HasCurrentFgPool(scene, launch, queries) == false)
    {
        FinishFgPath(true, scene, launch, result, queries);
        return float3(0);
    }
    if (launch.IsOrthographic == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_PHOTON_CAMERA);
        FinishFgPath(true, scene, launch, result, queries);
        return float3(0);
    }
    float3 emission;
    FgPoint receiver = TraceFgCamera(scene, launch, result.RandomKey, queries, emission);
    if (receiver.IsValid == true)
    {
        SetFgReceiver(receiver, launch, result);
        result.Confidence = 1;
        float weightSum = 0;
        uint coin = 0;
        // One BSDF final-gather ray; photons at its diffuse endpoint include direct
        // photons, yielding two-diffuse-event transport without duplicating direct NEE.
        float3 directionWorld;
        float3 sourceWeight;
        if (TrySampleFgBsdf(
                receiver,
                SampleFgRandomTriple(launch.Seed, GetFgPixel(launch), launch.SampleIndex, FG_GATHER_FIRST_DIMENSION),
                directionWorld,
                sourceWeight) == true)
        {
            float3 value;
            float density;
            EvaluateStandardBsdf(receiver.Material, receiver.ViewDirectionWorld, directionWorld, value, density);
            SurfaceHit hit;
            if (density > 0 && TraceClosest(scene,
                                            queries,
                                            SpawnSurfaceRay(receiver.Hit, directionWorld, FG_MAXIMUM_DISTANCE_WORLD),
                                            hit) == SCENE_QUERY_SURFACE)
            {
                FgPoint gather;
                if (TryMakeFgPoint(scene, hit, -directionWorld, queries, gather) == true &&
                    IsFgDiffuse(gather.Material) == true)
                    GatherFgPhotons(scene,
                                    launch,
                                    receiver,
                                    gather,
                                    false,
                                    1 / density,
                                    queries,
                                    result,
                                    weightSum,
                                    coin);
            }
        }
        // A caustic behind a glossy camera prefix cannot project directly to the lens.
        // Its density estimate competes with the final gather in the path reservoir.
        if (receiver.Depth > 0)
            GatherFgPhotons(scene, launch, receiver, receiver, true, 1, queries, result, weightSum, coin);
        float target = ComputeFgLuminance(EvaluateFgPath(scene, result, receiver, false, queries));
        result.Weight = target > 0 ? weightSum / target : 0;
    }
    FinishFgPath(true, scene, launch, result, queries);
    return float3(0);
}
