#include "projection.slang"
#include "storage.slang"

// Slang's pinned SPIR-V 1.3 backend crashes on vector-element InterlockedExchange.
// Compare/exchange implements the same atomic prepend without changing the target profile.
[noinline] uint ExchangeFgHead(uint address, bool isCaustic, uint value)
{
    uint expected = FG_INVALID;
    [loop] while (true)
    {
        uint observed;
        if (isCaustic == true)
            InterlockedCompareExchange(GraphBuffer0[address].y, expected, value, observed);
        else
            InterlockedCompareExchange(GraphBuffer0[address].x, expected, value, observed);
        if (observed == expected)
            return observed;
        expected = observed;
    }
}
float3 ResetPhotonMap(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasCurrentFgPool(scene, launch, queries) == false)
        return float3(0);
    GraphBuffer0[FG_HEAD_OFFSET + GetFgPixel(launch)] = uint4(FG_INVALID, FG_INVALID, 0, 0);
    return float3(0);
}
// Mesh photons are uniform in area and cosine-weighted in direction, including both
// emission faces. Point/spot photons use a uniform sphere; the analytic light's range
// window is applied segment-by-segment as in SampleIncidentLight at its first hit.
[noinline] bool TryEmitFgPhoton(SceneContext scene,
                                uint lightIndex,
                                uint4 key,
                                float2 directionUv,
                                float fluxScale,
                                inout RayQueryContext queries,
                                out Ray outRay,
                                out float3 outFlux)
{
    outRay = (Ray)0;
    outFlux = float3(0);
    LightInfo light = GetLightInfo(scene, lightIndex);
    float3 shape = SampleFgRandomTriple(key.xy, key.w, key.z, FG_SOURCE_FIRST_DIMENSION);
    outRay.MaxDistanceWorld = FG_MAXIMUM_DISTANCE_WORLD;
    if (light.Kind == SCENE_LIGHT_MESH)
    {
        float area = GetEmitter(scene, light.EmitterIndex).z;
        float target = shape.x * area;
        TriangleData triangle = (TriangleData)0;
        uint triangleCount = uint(GetEmitter(scene, light.EmitterIndex).y);
        for (uint i = 0; i < triangleCount; ++i)
        {
            triangle = GetEmitterTriangle(scene, light.EmitterIndex, i);
            target -= triangle.AreaWorld;
            if (target <= 0)
                break;
        }
        if (triangle.IsValid == false || area <= 0)
            return false;
        float root = sqrt(shape.y);
        SurfaceHit hit = GetSurfaceHit(scene, triangle.TriangleIndex, float2(root * (1 - shape.z), root * shape.z));
        MaterialSample material = EvaluateMaterial(scene, hit);
        float3 normalWorld = hit.GeometricNormalWorld;
        float faceScale = 1;
        if (material.IsEmissionTwoSided == true)
        {
            faceScale = 2;
            if (SampleFgRandom(key.xy, key.w, key.z, FG_SOURCE_FIRST_DIMENSION + FG_VERTEX_DIMENSIONS) < 0.5)
                normalWorld = -normalWorld;
        }
        // The engine's zero-specular-probability proposal is exactly a cosine hemisphere.
        float3 directionWorld = SampleScatteringBsdf(normalWorld, normalWorld, 1, 0, float3(0, directionUv));
        outRay = SpawnSurfaceRay(hit, directionWorld, FG_MAXIMUM_DISTANCE_WORLD);
        outFlux = EvaluateEmission(scene, hit, directionWorld) * (area * PI * faceScale * fluxScale);
    }
    else if (light.Kind == SCENE_LIGHT_POINT || light.Kind == SCENE_LIGHT_SPOT)
    {
        float cosine = 1 - 2 * directionUv.x;
        float radius = sqrt(max(0, 1 - cosine * cosine));
        float angle = 2 * PI * directionUv.y;
        outRay.OriginWorld = light.PositionWorld;
        outRay.DirectionWorld = float3(radius * cos(angle), cosine, radius * sin(angle));
        float cone = 1;
        if (light.Kind == SCENE_LIGHT_SPOT)
        {
            cone = saturate((dot(outRay.DirectionWorld, light.DirectionWorld) - light.OuterCos) /
                            (light.InnerCos - light.OuterCos));
            cone *= cone;
        }
        outFlux = light.Intensity * (4 * PI * cone * fluxScale);
    }
    return any(outFlux > 0) == true;
}
float3 TracePhotons(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasCurrentFgPool(scene, launch, queries) == false)
        return float3(0);
    uint path = GetFgPixel(launch);
    if (path >= GraphBuffer0[2].y)
        return float3(0);
    uint lightCount = GraphBuffer0[2].x;
    uint begin = 0;
    uint end = lightCount;
    while (begin < end)
    {
        uint middle = begin + (end - begin) / 2;
        if (GraphBuffer0[GetFgLightAddress(middle)].y <= path)
            begin = middle + 1;
        else
            end = middle;
    }
    uint light = begin;
    uint address = GetFgLightAddress(light);
    uint lightPaths = GraphBuffer0[address].z;
    uint localPath = path - (GraphBuffer0[address].y - lightPaths);
    uint4 key = uint4(launch.Seed, launch.SampleIndex, path);
    float2 directionUv = SampleFgRandomPair(key.xy, path, key.z, 0);
    uint assigned = 0;
    uint chosenCell = FG_INVALID;
    uint counts[FG_DIRECTION_CELLS];
    for (uint cell = 0; cell < FG_DIRECTION_CELLS; ++cell)
    {
        counts[cell] = IS_FG_GUIDING_ENABLED == true
                           ? ComputeFgDirectionCount(asfloat(GraphBuffer0[address + 1 + cell].x), lightPaths)
                           : 0;
        counts[cell] = min(counts[cell], lightPaths - assigned);
        if (chosenCell == FG_INVALID && localPath < assigned + counts[cell])
            chosenCell = cell;
        assigned += counts[cell];
    }
    uint residual = lightPaths - assigned;
    if (chosenCell != FG_INVALID)
        directionUv = (float2(chosenCell % FG_DIRECTION_SIDE, chosenCell / FG_DIRECTION_SIDE) + directionUv) /
                      float(FG_DIRECTION_SIDE);
    uint cell = GetFgDirectionCell(directionUv);
    float fluxScale = ComputeFgFluxScale(counts[cell], residual);
    Ray ray;
    float3 flux;
    bool hasRay = TryEmitFgPhoton(scene, light, key, directionUv, fluxScale, queries, ray, flux);
    bool isCaustic = false;
    LightInfo source = GetLightInfo(scene, light);
    [loop] for (uint depth = 0; depth < FG_MAXIMUM_PHOTON_DEPTH; ++depth)
    {
        uint index = path * FG_MAXIMUM_PHOTON_DEPTH + depth;
        FgPhoton photon = (FgPhoton)0;
        photon.Triangle = FG_INVALID;
        photon.NextGrid = FG_INVALID;
        photon.NextCaustic = FG_INVALID;
        if (hasRay == true && depth < FG_PHOTON_BOUNCES)
        {
            SurfaceHit hit;
            hasRay = TraceClosest(scene, queries, ray, hit) == SCENE_QUERY_SURFACE;
            FgPoint point;
            if (hasRay == true)
                hasRay = TryMakeFgPoint(scene, hit, -ray.DirectionWorld, queries, point);
            if (hasRay == true)
            {
                if (depth == 0 && source.Kind != SCENE_LIGHT_MESH)
                {
                    float ratio = hit.DistanceWorld / source.RangeWorld;
                    float square = ratio * ratio;
                    float window = max(0, 1 - square * square);
                    flux *= window * window;
                }
                bool isDiffuse = IsFgDiffuse(point.Material);
                if (isDiffuse == true)
                {
                    photon.Triangle = hit.TriangleIndex;
                    photon.Barycentric = hit.Barycentric;
                    photon.Flags = depth | (isCaustic == true ? FG_CAUSTIC_FLAG : 0);
                    photon.Flux = flux;
                    photon.IncomingWorld = -ray.DirectionWorld;
                    photon.RandomKey = key;
                    photon.DirectionUv = directionUv;
                    photon.FluxScale = fluxScale;
                    photon.Light = light;
                    photon.Cell = cell;
                    uint bucket = ComputeFgBucket(int3(floor(hit.PositionWorld / FG_GATHER_RADIUS_WORLD)),
                                                  GetFgPixelCount(launch));
                    photon.NextGrid = ExchangeFgHead(FG_HEAD_OFFSET + bucket, false, index);
                    uint pixel;
                    float jacobian;
                    if (isCaustic == true && TryProjectFgPhoton(launch,
                                                                hit.PositionWorld,
                                                                point.Material.ShadingNormalWorld,
                                                                pixel,
                                                                jacobian) == true)
                    {
                        LightSample camera = (LightSample)0;
                        camera.DirectionWorld = normalize(launch.CameraPositionWorld - hit.PositionWorld);
                        camera.PositionWorld = launch.CameraPositionWorld;
                        camera.DistanceWorld = length(launch.CameraPositionWorld - hit.PositionWorld);
                        camera.TriangleIndex = FG_INVALID;
                        camera.HasFiniteEndpoint = true;
                        camera.HasSample = true;
                        if (TestVisibility(scene, queries, hit, camera) == SCENE_QUERY_MISS)
                        {
                            photon.NextCaustic = ExchangeFgHead(FG_HEAD_OFFSET + pixel, true, index);
                        }
                    }
                }
                isCaustic = isDiffuse == false;
                float3 directionWorld;
                float3 weight;
                hasRay = depth + 1 < FG_PHOTON_BOUNCES &&
                         TrySampleFgBsdf(point,
                                         SampleFgRandomTriple(key.xy,
                                                              path,
                                                              key.z,
                                                              FG_SOURCE_FIRST_DIMENSION + FG_VERTEX_DIMENSIONS + 1 +
                                                                  depth * FG_VERTEX_DIMENSIONS),
                                         directionWorld,
                                         weight) == true;
                if (hasRay == true)
                {
                    flux *= weight;
                    ray = SpawnSurfaceRay(hit, directionWorld, FG_MAXIMUM_DISTANCE_WORLD);
                }
            }
        }
        if (all(isfinite(flux)) == false || any(flux < 0) == true)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
            hasRay = false;
        }
        StoreFgPhoton(false, FG_PHOTON_OFFSET + index * FG_PHOTON_WORDS, photon);
    }
    RecordFgPoolFailure(queries);
    return float3(0);
}
