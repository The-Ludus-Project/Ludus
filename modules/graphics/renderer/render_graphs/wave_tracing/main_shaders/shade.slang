#include "hit_record.slang"
// Scene optical renderer: truncated spectral surface paths with coherent, scalar finite-light connections.
// Thanks to Steinberg and Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics", CGF45(2),
// e70322 (2026), Sections3, 4.2 and 5.2, https://doi.org/10.1111/cgf.70322 . Diffraction depends on the wavelength
// (Section 3, "Dispersion and color"), so with WT_SPECTRUM 1 each path draws one wavelength over 380-780 nm: its
// authored RGB emission, reflection and transmission factors are uplifted to spectra at that wavelength, its
// connections and glass relays diffract at it, glass refracts at its declared index for it (dispersion), and the
// renderer's CIE 1931 camera (spectral_sensor.slang) reads the spectral radiance. WT_SPECTRUM 0 keeps one scalar
// wavelength scaling RGB transport. WT_OPTICAL_CONNECTION selects the Gaussian angular-spectrum connection
// (wave_connection.cpp), the UTD edge connection (wave_edge_diffraction.cpp) or the Fraunhofer edge connection
// (wave_fraunhofer_diffraction.cpp). All geometry, lights, textures and supported typed reflection materials come from
// the current World. Rough surface vertices sample light endpoints only by NEE; emitter/environment hits after them are
// omitted, so the scalar optical visibility and classical continuation visibility cannot receive inconsistent MIS.
// Glass reflection and refraction are delta events that no light sample reaches: emission found after them keeps full
// weight, and its connection is relayed coherently back through the glass events (wave_relay.slang) to the vertex
// whose rough scattering ended coherence, or to the camera.
static const float WAVE_METRES_PER_NANOMETRE = 0.000000001;
static const float WAVE_MAXIMUM_TRACE_DISTANCE_WORLD = 2000000;
static const uint WAVE_VERTEX_LIMIT = 8;
static const uint WAVE_RANDOM_VERTEX_STRIDE = 8;
static const uint WAVE_LIGHT_SHAPE_FIRST = 1;
static const uint WAVE_CONTINUATION_FIRST = 4;
static const uint WAVE_MATERIAL_RANDOM_SCOPE = 0x57415645;
/// WT_SPECTRUM: 0 evaluates WT_WAVELENGTH_NANOMETERS alone, 1 samples the visible spectrum.
static const uint WAVE_SPECTRUM_VISIBLE = 1;
/// Each path's wavelength draw follows every vertex's light and continuation dimensions.
static const uint WAVE_WAVELENGTH_DIMENSION =
    SHADE_RANDOM_DIMENSION_FIRST + WAVE_VERTEX_LIMIT * WAVE_RANDOM_VERTEX_STRIDE;

/// The path's sampled vacuum wavelength in nanometres, or zero for monochromatic RGB transport.
float SampleWaveWavelengthNanometers(RayLaunch launch)
{
    return WT_SPECTRUM == WAVE_SPECTRUM_VISIBLE
               ? SampleWavelengthNanometers(SampleUniformFloat01(launch, WAVE_WAVELENGTH_DIMENSION), 0)
               : 0;
}
/// Vacuum wavelength of the path's optical connections in metres: WT_WAVELENGTH_NANOMETERS belongs to 550 nm light,
/// and a spectral path scales it by its sampled wavelength (equivalent to scaling the scene).
float GetOpticalWavelengthWorld(float wavelengthNanometers)
{
    float declaredWorld = WT_WAVELENGTH_NANOMETERS * WAVE_METRES_PER_NANOMETRE;
    return wavelengthNanometers > 0
               ? declaredWorld * (wavelengthNanometers / LUDUS_WAVE_SPECTRAL_REFERENCE_WAVELENGTH_NANOMETERS)
               : declaredWorld;
}
/// Spectral value of an authored linear RGB factor at a spectral path's wavelength, in every channel; RGB paths keep
/// it.
float3 ProjectWaveSpectrum(float wavelengthNanometers, float3 linearRgb)
{
    return wavelengthNanometers > 0 ? float3(EvaluateUpliftedSpectrum(linearRgb, wavelengthNanometers)) : linearRgb;
}
/// Linear RGB of a path's radiance: the camera's sensor reads a spectral path's spectral radiance.
float3 ResolveWaveSpectrum(float wavelengthNanometers, float3 radiance)
{
    return wavelengthNanometers > 0 ? ConvertSpectralRadiance(radiance.x, wavelengthNanometers) : radiance;
}

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
void FailWaveConnection(inout RayQueryContext queries, WaveQuery status)
{
    FailSceneQuery(queries,
                   status == WaveQuery::Capacity
                       ? LUDUS_SCENE_FAILURE_WAVE_CAPACITY
                       : ((status == WaveQuery::PropagationCapacity || status == WaveQuery::EscapeCapacity)
                              ? LUDUS_SCENE_FAILURE_WAVE_PROPAGATION_CAPACITY
                              : LUDUS_SCENE_FAILURE_WAVE_CONNECTION));
}
float EvaluateOpticalVisibility(SceneContext scene,
                                SurfaceHit receiver,
                                IncidentLightSample light,
                                float wavelengthWorld,
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
    WaveConnectionOperator connection = WaveConnectionOperator(WT_OPTICAL_CONNECTION);
    WaveQuery status;
    if (connection == WaveConnectionOperator::UtdEdgeDiffraction)
        status = EvaluateUtdConnectionToReceiver(scene,
                                                 sourceWorld,
                                                 shadow.OriginWorld,
                                                 deltaWorld / distanceWorld,
                                                 distanceWorld,
                                                 wavelengthWorld,
                                                 field);
    else if (connection == WaveConnectionOperator::FraunhoferEdgeDiffraction)
        status = EvaluateFraunhoferConnectionToReceiver(scene,
                                                        sourceWorld,
                                                        shadow.OriginWorld,
                                                        deltaWorld / distanceWorld,
                                                        distanceWorld,
                                                        wavelengthWorld,
                                                        field);
    else
        status = EvaluateWaveConnectionToReceiver(scene,
                                                  sourceWorld,
                                                  shadow.OriginWorld,
                                                  deltaWorld / distanceWorld,
                                                  distanceWorld,
                                                  wavelengthWorld,
                                                  field);
    if (status != WaveQuery::Ready)
    {
        FailWaveConnection(queries, status);
        return 0;
    }
    return dot(field, field);
}
/// Power of the emitter connection from sourceWorld, relayed back through the glass events of `relay`.
float EvaluateRelayedVisibility(SceneContext scene,
                                WaveRelay relay,
                                float3 sourceWorld,
                                float wavelengthWorld,
                                inout RayQueryContext queries)
{
    if (queries.VisibilityQueries >= queries.MaximumVisibilityQueries)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_VISIBILITY_BUDGET);
        return 0;
    }
    queries.VisibilityQueries += 1;
    float2 field = float2(0);
    bool isAdmitted;
    WaveQuery status = EvaluateWaveRelay(scene,
                                         relay,
                                         sourceWorld,
                                         wavelengthWorld,
                                         WaveConnectionOperator(WT_OPTICAL_CONNECTION),
                                         isAdmitted,
                                         field);
    if (status == WaveQuery::Ready && isAdmitted == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_WAVE_INTERFACE);
        return 0;
    }
    if (status != WaveQuery::Ready)
    {
        FailWaveConnection(queries, status);
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
    float wavelengthNanometers = SampleWaveWavelengthNanometers(launch);
    float wavelengthWorld = GetOpticalWavelengthWorld(wavelengthNanometers);
    if (record.IsHit == false)
        return mode == 2 ? float3(0)
                         : ResolveWaveSpectrum(wavelengthNanometers,
                                               ProjectWaveSpectrum(wavelengthNanometers,
                                                                   EvaluateEnvironment(scene, ray.DirectionWorld)));
    if (WT_PATH_VERTICES == 0 || WT_PATH_VERTICES > WAVE_VERTEX_LIMIT)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SURFACE_BUDGET);
        return float3(0);
    }
    SurfaceHit hit = GetSurfaceHit(scene, record.TriangleIndex, record.Barycentric);
    hit.IncomingRay = ray;
    hit.DistanceWorld = record.DistanceWorld;
    hit.IsFrontFace = record.IsFrontFace;
    float3 radiance =
        mode == 2 ? float3(0)
                  : ProjectWaveSpectrum(wavelengthNanometers, EvaluateEmission(scene, hit, -ray.DirectionWorld));
    float3 throughput = float3(1);
    WaveRelay relay = BeginWaveRelay(ray.OriginWorld);
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
        SceneLightSample sample = SampleLight(scene,
                                              hit,
                                              launch,
                                              dimension,
                                              dimension + WAVE_LIGHT_SHAPE_FIRST,
                                              LightSelectionPolicy::Uniform,
                                              queries);
        if (sample.Selection.HasSample == true)
        {
            IncidentLightSample light = sample.Incident;
            float probability = sample.Selection.SelectionPmf * light.ConditionalProbability;
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
                                         : EvaluateOpticalVisibility(scene, hit, light, wavelengthWorld, queries);
                if (mode == 2)
                    return float3(transmission);
                radiance += throughput * ProjectWaveSpectrum(wavelengthNanometers, value) *
                            ProjectWaveSpectrum(wavelengthNanometers, light.IncidentCoefficient) *
                            (cosine * transmission / probability);
            }
        }
        if (mode == 2 || depth + 1 == WT_PATH_VERTICES)
            break;
        float3 samples = float3(SampleUniformFloat01(launch, dimension + WAVE_CONTINUATION_FIRST),
                                SampleUniformFloat01(launch, dimension + WAVE_CONTINUATION_FIRST + 1),
                                SampleUniformFloat01(launch, dimension + WAVE_CONTINUATION_FIRST + 2));
        StandardBsdfTransportSample continuation = SampleStandardBsdfTransport(material,
                                                                               viewWorld,
                                                                               samples,
                                                                               MakeWaveMaterialRandom(launch, depth),
                                                                               wavelengthNanometers);
        // Refraction must also leave through the geometric back side.
        bool isContinuationAbove = dot(geometricNormalWorld, continuation.DirectionWorld) > 0;
        if (continuation.HasSample == false || isContinuationAbove == continuation.IsTransmission)
            break;
        throughput *= ProjectWaveSpectrum(wavelengthNanometers, continuation.Weight);
        ray = SpawnSurfaceRay(hit, continuation.DirectionWorld, WAVE_MAXIMUM_TRACE_DISTANCE_WORLD);
        if (continuation.IsSpecular == true)
            AppendWaveRelayEvent(relay,
                                 material.Model == MaterialModel::Dielectric,
                                 GetDielectricIndex(material, wavelengthNanometers),
                                 dot(material.ShadingNormalWorld, viewWorld) < 0,
                                 continuation.IsTransmission == false,
                                 hit.PositionWorld);
        else
            relay = BeginWaveRelay(ray.OriginWorld);
        SceneQuery query = TraceClosest(scene, queries, ray, hit);
        if (query == SceneQuery::Miss && continuation.IsSpecular == true)
            radiance +=
                throughput * ProjectWaveSpectrum(wavelengthNanometers, EvaluateEnvironment(scene, ray.DirectionWorld));
        if (query != SceneQuery::Surface)
            break;
        if (continuation.IsSpecular == true)
        {
            // No light sample reaches a delta direction: the emitter keeps full weight, behind the relayed field.
            float3 emission =
                ProjectWaveSpectrum(wavelengthNanometers, EvaluateEmission(scene, hit, -ray.DirectionWorld));
            if (any(emission > 0) == true)
            {
                float transmission =
                    mode == 1 ? 1
                              : EvaluateRelayedVisibility(
                                    scene,
                                    relay,
                                    OffsetPositionWorld(hit.PositionWorld, hit.TriangleIndex, -ray.DirectionWorld),
                                    wavelengthWorld,
                                    queries);
                radiance += throughput * emission * transmission;
            }
        }
    }
    radiance = ResolveWaveSpectrum(wavelengthNanometers, radiance);
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
