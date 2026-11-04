#include "hit_record.slang"
// Scene optical renderer: truncated RGB surface paths with coherent, scalar paraxial finite-light connections.
// Thanks to Steinberg and Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics", CGF45(2),
// e70322 (2026), Sections3.1 and4.2, https://doi.org/10.1111/cgf.70322 . See wave_connection.cpp for the
// Gaussian connection integral and angular-spectrum departure from their Fraunhofer/UTD operators.
// All geometry, lights, textures and supported typed reflection materials come from the current World.
// Each surface path samples light endpoints only by NEE. Secondary emitter/environment hits are omitted,
// so the scalar optical visibility and classical continuation visibility cannot receive inconsistent MIS.
static const float WAVE_METRES_PER_NANOMETRE = 0.000000001;
static const float WAVE_MAXIMUM_TRACE_DISTANCE_WORLD = 2000000;
static const uint WAVE_VERTEX_LIMIT = 8;
static const uint WAVE_RANDOM_VERTEX_STRIDE = 8;
static const uint WAVE_LIGHT_SHAPE_FIRST = 1;
static const uint WAVE_CONTINUATION_FIRST = 4;
static const uint WAVE_MATERIAL_RANDOM_SCOPE = 0x57415645;

CoatingRandomContext MakeWaveMaterialRandom(RayLaunch launch, uint depth)
{
    CoatingRandomContext result;
    result.Key = launch.Seed;
    result.Address = MakeRandomAddress(
        uint2(launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x, WAVE_MATERIAL_RANDOM_SCOPE),
        launch.SampleIndex,
        depth);
    return result;
}
float EvaluateOpticalVisibility(SceneContext scene,
                                SurfaceHit receiver,
                                LightSample light,
                                inout RayQueryContext queries)
{
    if (light.HasFiniteEndpoint == false)
        return TestVisibility(scene, queries, receiver, light) == SceneQuery::Miss ? 1 : 0;
    if (queries.VisibilityQueries >= queries.MaximumVisibilityQueries)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_VISIBILITY_BUDGET);
        return 0;
    }
    queries.VisibilityQueries += 1;
    Ray shadow = SpawnSurfaceRay(receiver, light.DirectionWorld, 0);
    float3 sourceWorld = light.PositionWorld;
    if (light.TriangleIndex != SCENE_INVALID_INDEX)
        sourceWorld = OffsetPositionWorld(sourceWorld, light.TriangleIndex, -light.DirectionWorld);
    float3 deltaWorld = shadow.OriginWorld - sourceWorld;
    float distanceWorld = length(deltaWorld);
    float2 field = float2(0);
    WaveQuery status = EvaluateWaveConnectionToReceiver(scene,
                                                        sourceWorld,
                                                        shadow.OriginWorld,
                                                        deltaWorld / distanceWorld,
                                                        distanceWorld,
                                                        WT_WAVELENGTH_NANOMETERS * WAVE_METRES_PER_NANOMETRE,
                                                        field);
    if (status != WaveQuery::Ready)
    {
        FailSceneQuery(queries,
                       status == WaveQuery::Capacity
                           ? LUDUS_SCENE_FAILURE_WAVE_CAPACITY
                           : ((status == WaveQuery::PropagationCapacity || status == WaveQuery::EscapeCapacity)
                                  ? LUDUS_SCENE_FAILURE_WAVE_PROPAGATION_CAPACITY
                                  : LUDUS_SCENE_FAILURE_WAVE_CONNECTION));
        return 0;
    }
    return dot(field, field);
}
float3 TraceOpticalSurfacePath(SceneContext scene, RayLaunch launch, inout RayQueryContext queries, uint mode)
{
    if (RequireSceneBsdf(scene, SceneBsdfRequirement::Transport, queries) == false)
        return float3(0);
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    Ray ray = MakeCameraRay(launch, record.JitterPixel);
    if (record.IsHit == false)
        return mode == 2 ? float3(0) : EvaluateEnvironment(scene, ray.DirectionWorld);
    if (WT_PATH_VERTICES == 0 || WT_PATH_VERTICES > WAVE_VERTEX_LIMIT)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SURFACE_BUDGET);
        return float3(0);
    }
    SurfaceHit hit = GetSurfaceHit(scene, record.TriangleIndex, record.Barycentric);
    hit.IncomingRay = ray;
    hit.DistanceWorld = record.DistanceWorld;
    hit.IsFrontFace = record.IsFrontFace;
    float3 radiance = mode == 2 ? float3(0) : EvaluateEmission(scene, hit, -ray.DirectionWorld);
    float3 throughput = float3(1);
    for (uint depth = 0; depth < WT_PATH_VERTICES; ++depth)
    {
        float3 viewWorld = -ray.DirectionWorld;
        MaterialSample material = EvaluateMaterial(scene, hit);
        if (material.IsValid == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
            return float3(0);
        }
        float3 shadingNormalWorld = dot(material.ShadingNormalWorld, viewWorld) < 0 ? -material.ShadingNormalWorld
                                                                                    : material.ShadingNormalWorld;
        // Glass keeps its outward normal, which tells its BSDF whether the path is inside; it takes no light sample.
        bool isDielectric = IsDielectricMaterial(material);
        if (isDielectric == false)
            material.ShadingNormalWorld = shadingNormalWorld;
        float3 geometricNormalWorld =
            dot(hit.GeometricNormalWorld, viewWorld) < 0 ? -hit.GeometricNormalWorld : hit.GeometricNormalWorld;
        uint dimension = SHADE_RANDOM_DIMENSION_FIRST + depth * WAVE_RANDOM_VERTEX_STRIDE;
        SelectedLight selected = SelectLight(scene, launch, dimension);
        if (selected.HasSample == true)
        {
            float3 shapeSample = float3(SampleSceneRandom(launch, dimension + WAVE_LIGHT_SHAPE_FIRST),
                                        SampleSceneRandom(launch, dimension + WAVE_LIGHT_SHAPE_FIRST + 1),
                                        SampleSceneRandom(launch, dimension + WAVE_LIGHT_SHAPE_FIRST + 2));
            LightSample light = SampleIncidentLight(scene, selected, hit, shapeSample);
            float probability = light.SelectionPmf * light.ConditionalPdf;
            float cosine = dot(shadingNormalWorld, light.DirectionWorld);
            if (isDielectric == false && light.HasSample == true && probability > 0 && cosine > 0 &&
                dot(geometricNormalWorld, light.DirectionWorld) > 0 && any(light.IncidentCoefficient > 0) == true)
            {
                float3 value;
                float pdf;
                EvaluateStandardBsdf(material,
                                     viewWorld,
                                     light.DirectionWorld,
                                     MakeWaveMaterialRandom(launch, depth),
                                     value,
                                     pdf);
                float transmission = mode == 1
                                         ? (TestVisibility(scene, queries, hit, light) == SceneQuery::Miss ? 1 : 0)
                                         : EvaluateOpticalVisibility(scene, hit, light, queries);
                if (mode == 2)
                    return float3(transmission);
                radiance += throughput * value * light.IncidentCoefficient * (cosine * transmission / probability);
            }
        }
        if (mode == 2 || depth + 1 == WT_PATH_VERTICES)
            break;
        float3 samples = float3(SampleSceneRandom(launch, dimension + WAVE_CONTINUATION_FIRST),
                                SampleSceneRandom(launch, dimension + WAVE_CONTINUATION_FIRST + 1),
                                SampleSceneRandom(launch, dimension + WAVE_CONTINUATION_FIRST + 2));
        StandardBsdfTransportSample continuation =
            SampleStandardBsdfTransport(material, viewWorld, samples, MakeWaveMaterialRandom(launch, depth));
        // Refraction must also leave through the geometric back side.
        bool isContinuationAbove = dot(geometricNormalWorld, continuation.DirectionWorld) > 0;
        if (continuation.HasSample == false || isContinuationAbove == continuation.IsTransmission)
            break;
        throughput *= continuation.Weight;
        ray = SpawnSurfaceRay(hit, continuation.DirectionWorld, WAVE_MAXIMUM_TRACE_DISTANCE_WORLD);
        SceneQuery query = TraceClosest(scene, queries, ray, hit);
        if (query != SceneQuery::Surface)
            break;
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}
float3 ShadeWave(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return TraceOpticalSurfacePath(scene, launch, queries, 0);
}
float3 ShadeGeometric(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return TraceOpticalSurfacePath(scene, launch, queries, 1);
}
float3 MeasureTransmission(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return TraceOpticalSurfacePath(scene, launch, queries, 2);
}
