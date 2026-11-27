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
// WT_BIDIRECTIONAL 1 adds the light-vertex strategy of the reference capture's ReferenceOpticalPathSampling::
// Bidirectional (reference_path.cpp owns its sources: Veach's thesis, Chapters 9-10, pbrt-v3 section 16.3, and
// Steinberg and Pharr's Section 6.1): at every rough vertex a light ray from a point on the selected finite light lands
// on a rough surface, which connects to the vertex by binary visibility. Its light segment keeps the optical connection
// that next-event estimation at that surface evaluates, so both strategies estimate the declared integral, and the
// power heuristic combines them (render-graph-authoring.md, "Bidirectional optical connections").
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
/// WT_BIDIRECTIONAL 1: each vertex's light subpath draws after the wavelength, one stride per vertex: the light
/// selection, three shape dimensions, two direction dimensions and the side of a two-sided emitter.
static const uint WAVE_BIDIRECTIONAL = 1;
static const uint WAVE_LIGHT_VERTEX_DIMENSION_FIRST = WAVE_WAVELENGTH_DIMENSION + 1;
static const uint WAVE_LIGHT_VERTEX_SHAPE_FIRST = 1;
static const uint WAVE_LIGHT_VERTEX_DIRECTION_FIRST = 4;
static const uint WAVE_LIGHT_VERTEX_SIDE = 6;
/// Shadow queries of a rough vertex with WT_BIDIRECTIONAL 1: next-event estimation, the light vertex's connection and
/// its optical light segment. The last vertex draws no light vertex.
static const uint WAVE_BIDIRECTIONAL_VISIBILITY_QUERIES_PER_VERTEX = 3;
static const uint WAVE_LIGHT_VERTEX_QUERIES = 2;
static const float WAVE_SPHERE_SOLID_ANGLE_FACTOR = 4;
static const float WAVE_FRAME_POLE_COSINE = 0.999;

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
/// The start y0 of a light subpath: a point on a mesh light or the position of a finite analytic light.
struct WaveLightEndpoint
{
    float3 PositionWorld;
    float3 NormalWorld; ///< Emitting-side normal of a mesh light; unused for an analytic light.
    uint TriangleIndex; ///< SCENE_INVALID_INDEX for an analytic light.
    bool IsTwoSided;
    bool HasEndpoint;
};
WaveLightEndpoint MakeWaveLightEndpoint(SceneContext scene, IncidentLightSample light)
{
    WaveLightEndpoint result = (WaveLightEndpoint)0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    if (light.HasSample == false || light.HasFiniteEndpoint == false)
        return result;
    result.PositionWorld = light.PositionWorld;
    result.TriangleIndex = light.TriangleIndex;
    if (light.TriangleIndex != SCENE_INVALID_INDEX)
    {
        SurfaceHit endpoint = GetSurfaceHit(scene, light.TriangleIndex, light.Barycentric);
        MaterialSample material = EvaluateMaterial(scene, endpoint);
        if (endpoint.IsValid == false || material.IsValid == false)
            return result;
        result.NormalWorld = endpoint.GeometricNormalWorld;
        result.IsTwoSided = material.IsEmissionTwoSided;
    }
    result.HasEndpoint = true;
    return result;
}
/// Density per steradian of the light ray's direction: cosine-weighted about a mesh light's emitting side (either side
/// with probability one half when two-sided), uniform over the sphere for an analytic light.
float EvaluateWaveLightDirectionPdf(WaveLightEndpoint endpoint, float3 directionWorld)
{
    if (endpoint.TriangleIndex == SCENE_INVALID_INDEX)
        return 1 / (WAVE_SPHERE_SOLID_ANGLE_FACTOR * PI);
    float cosine = dot(endpoint.NormalWorld, directionWorld);
    if (endpoint.IsTwoSided == true)
        return abs(cosine) / (2 * PI);
    return cosine > 0 ? cosine / PI : 0;
}
/// Area densities at the light segment's rough vertex y1: the camera's BSDF draw from x (bsdfPdf per steradian) in x,
/// the light ray from y0 in y.
float2 EvaluateWaveLightVertexDensities(WaveLightEndpoint endpoint,
                                        float3 vertexWorld,
                                        float3 vertexNormalWorld,
                                        float3 cameraVertexWorld,
                                        float bsdfPdf)
{
    float3 lightDeltaWorld = vertexWorld - endpoint.PositionWorld;
    float3 cameraDeltaWorld = vertexWorld - cameraVertexWorld;
    float lightDistanceSquared = dot(lightDeltaWorld, lightDeltaWorld);
    float cameraDistanceSquared = dot(cameraDeltaWorld, cameraDeltaWorld);
    if (lightDistanceSquared <= 0 || cameraDistanceSquared <= 0 || isfinite(lightDistanceSquared) == false ||
        isfinite(cameraDistanceSquared) == false)
        return float2(0);
    float3 lightDirectionWorld = lightDeltaWorld / sqrt(lightDistanceSquared);
    float3 cameraDirectionWorld = cameraDeltaWorld / sqrt(cameraDistanceSquared);
    return float2(max(bsdfPdf, 0) * abs(dot(vertexNormalWorld, cameraDirectionWorld)) / cameraDistanceSquared,
                  EvaluateWaveLightDirectionPdf(endpoint, lightDirectionWorld) *
                      abs(dot(vertexNormalWorld, lightDirectionWorld)) / lightDistanceSquared);
}
/// Power heuristic of the density `pdf` against `other`.
float ComputeWavePowerWeight(float pdf, float other)
{
    if (pdf <= 0)
        return 0;
    if (other <= 0)
        return 1;
    float maximum = max(pdf, other);
    float a = pdf / maximum;
    float b = other / maximum;
    return a * a / (a * a + b * b);
}
/// Trace the light ray from `endpoint` along directionWorld to its first surface.
SceneQuery TraceWaveLightRay(SceneContext scene,
                             WaveLightEndpoint endpoint,
                             float3 directionWorld,
                             inout RayQueryContext queries,
                             out SurfaceHit outHit)
{
    Ray ray;
    ray.OriginWorld = endpoint.TriangleIndex == SCENE_INVALID_INDEX
                          ? endpoint.PositionWorld
                          : OffsetPositionWorld(endpoint.PositionWorld, endpoint.TriangleIndex, directionWorld);
    ray.MinDistanceWorld = 0;
    ray.DirectionWorld = directionWorld;
    ray.MaxDistanceWorld = WAVE_MAXIMUM_TRACE_DISTANCE_WORLD;
    return TraceClosest(scene, queries, ray, outHit);
}
/// Next-event weight at the rough vertex `receiver` against the light-vertex strategy of the rough vertex at
/// previousWorld, which reaches this path only when the light ray towards the receiver lands on its triangle.
float ComputeWaveNextEventWeight(SceneContext scene,
                                 SurfaceHit receiver,
                                 float3 receiverNormalWorld,
                                 IncidentLightSample light,
                                 float3 previousWorld,
                                 float previousPdf,
                                 inout RayQueryContext queries)
{
    WaveLightEndpoint endpoint = MakeWaveLightEndpoint(scene, light);
    if (endpoint.HasEndpoint == false)
        return 1;
    float2 densities = EvaluateWaveLightVertexDensities(endpoint,
                                                        receiver.PositionWorld,
                                                        receiverNormalWorld,
                                                        previousWorld,
                                                        previousPdf);
    if (densities.y <= 0)
        return 1;
    SurfaceHit landing;
    SceneQuery query = TraceWaveLightRay(scene,
                                         endpoint,
                                         NormalizeOrZero(receiver.PositionWorld - endpoint.PositionWorld),
                                         queries,
                                         landing);
    return query == SceneQuery::Surface && landing.TriangleIndex == receiver.TriangleIndex
               ? ComputeWavePowerWeight(densities.x, densities.y)
               : 1;
}
/// The light-vertex strategy at the rough vertex `receiver` (material with its viewer-side shading normal): a light
/// subpath y0 -> y1 connects its rough vertex y1 to the receiver by binary visibility, with next-event estimation's
/// light connection at y1. Returns the spectral contribution before the path throughput.
float3 ConnectWaveLightVertex(SceneContext scene,
                              RayLaunch launch,
                              SurfaceHit receiver,
                              MaterialSample material,
                              float3 viewWorld,
                              float3 geometricNormalWorld,
                              uint depth,
                              float wavelengthNanometers,
                              float wavelengthWorld,
                              uint mode,
                              inout RayQueryContext queries)
{
    uint dimension = WAVE_LIGHT_VERTEX_DIMENSION_FIRST + depth * WAVE_RANDOM_VERTEX_STRIDE;
    SceneLightSample sample = SampleLight(scene,
                                          receiver,
                                          launch,
                                          dimension,
                                          dimension + WAVE_LIGHT_VERTEX_SHAPE_FIRST,
                                          LightSelectionPolicy::Uniform,
                                          queries);
    if (sample.Selection.HasSample == false)
        return float3(0);
    WaveLightEndpoint endpoint = MakeWaveLightEndpoint(scene, sample.Incident);
    if (endpoint.HasEndpoint == false)
        return float3(0); // Infinite lights source no light subpath.
    // Cosine-weighted about the emitting side of a mesh light, uniform over the sphere for an analytic light.
    float first = SampleUniformFloat01(launch, dimension + WAVE_LIGHT_VERTEX_DIRECTION_FIRST);
    float second = SampleUniformFloat01(launch, dimension + WAVE_LIGHT_VERTEX_DIRECTION_FIRST + 1);
    float3 axisWorld = float3(0, 0, 1);
    float axial = 1 - 2 * first;
    if (endpoint.TriangleIndex != SCENE_INVALID_INDEX)
    {
        bool isBackSide =
            endpoint.IsTwoSided == true && SampleUniformFloat01(launch, dimension + WAVE_LIGHT_VERTEX_SIDE) < 0.5;
        axisWorld = isBackSide == true ? -endpoint.NormalWorld : endpoint.NormalWorld;
        axial = sqrt(max(0, 1 - first));
    }
    float radial = sqrt(max(0, 1 - axial * axial));
    float angle = 2 * PI * second;
    float3 tangentWorld = NormalizeOrZero(
        cross(abs(axisWorld.z) < WAVE_FRAME_POLE_COSINE ? float3(0, 0, 1) : float3(0, 1, 0), axisWorld));
    float3 bitangentWorld = cross(axisWorld, tangentWorld);
    float3 lightDirectionWorld = NormalizeOrZero(tangentWorld * (radial * cos(angle)) +
                                                 bitangentWorld * (radial * sin(angle)) + axisWorld * axial);
    SurfaceHit vertex;
    if (TraceWaveLightRay(scene, endpoint, lightDirectionWorld, queries, vertex) != SceneQuery::Surface)
        return float3(0);
    float3 connectionDeltaWorld = vertex.PositionWorld - receiver.PositionWorld;
    float connectionDistance = length(connectionDeltaWorld);
    if (connectionDistance <= 0 || isfinite(connectionDistance) == false)
        return float3(0);
    float3 connectionWorld = connectionDeltaWorld / connectionDistance;
    float receiverCosine = dot(material.ShadingNormalWorld, connectionWorld);
    if (receiverCosine <= 0 || dot(geometricNormalWorld, connectionWorld) <= 0)
        return float3(0);
    // The light vertex y1, viewed from the receiver, with the normals that a camera path arriving there would use.
    float3 vertexViewWorld = -connectionWorld;
    MaterialSample vertexMaterial = EvaluateMaterial(scene, vertex);
    if (vertexMaterial.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    if (IsDielectricMaterial(vertexMaterial) == true)
        return float3(0); // Coherent relays through glass stay with the camera strategies.
    if (dot(vertexMaterial.ShadingNormalWorld, vertexViewWorld) < 0)
        vertexMaterial.ShadingNormalWorld = -vertexMaterial.ShadingNormalWorld;
    float3 vertexGeometricNormalWorld = dot(vertex.GeometricNormalWorld, vertexViewWorld) < 0
                                            ? -vertex.GeometricNormalWorld
                                            : vertex.GeometricNormalWorld;
    // Next-event estimation's proposal at y1 for the same light and shape dimensions, hence the same endpoint.
    IncidentLightSample light = SampleIncidentLight(scene,
                                                    sample.Selection.LightIndex,
                                                    vertex,
                                                    launch,
                                                    dimension + WAVE_LIGHT_VERTEX_SHAPE_FIRST,
                                                    queries);
    float probability = sample.Selection.SelectionPmf * light.ConditionalProbability;
    float vertexCosine = dot(vertexMaterial.ShadingNormalWorld, light.DirectionWorld);
    if (light.HasSample == false || probability <= 0 || vertexCosine <= 0 ||
        dot(vertexGeometricNormalWorld, light.DirectionWorld) <= 0 || any(light.IncidentCoefficient > 0) == false)
        return float3(0);
    float3 receiverValue;
    float receiverPdf;
    EvaluateStandardBsdf(material,
                         viewWorld,
                         connectionWorld,
                         MakeWaveMaterialRandom(launch, depth),
                         receiverValue,
                         receiverPdf);
    float3 vertexValue;
    float vertexPdf;
    EvaluateStandardBsdf(vertexMaterial,
                         vertexViewWorld,
                         light.DirectionWorld,
                         MakeWaveMaterialRandom(launch, depth + 1),
                         vertexValue,
                         vertexPdf);
    float2 densities = EvaluateWaveLightVertexDensities(endpoint,
                                                        vertex.PositionWorld,
                                                        vertexGeometricNormalWorld,
                                                        receiver.PositionWorld,
                                                        receiverPdf);
    if (densities.y <= 0)
        return float3(0);
    // Next-event estimate at y1 times the receiver's area-measure factor x -> y1, over the light ray's density at y1.
    float scale = receiverCosine * abs(dot(vertexGeometricNormalWorld, connectionWorld)) /
                  (connectionDistance * connectionDistance) * vertexCosine / probability / densities.y *
                  ComputeWavePowerWeight(densities.y, densities.x);
    float3 contribution = ProjectWaveSpectrum(wavelengthNanometers, receiverValue) *
                          ProjectWaveSpectrum(wavelengthNanometers, vertexValue) *
                          ProjectWaveSpectrum(wavelengthNanometers, light.IncidentCoefficient) * scale;
    if (any(contribution > 0) == false)
        return float3(0);
    // Binary visibility of the connection between the two rough vertices.
    IncidentLightSample connection = (IncidentLightSample)0;
    connection.TriangleIndex = vertex.TriangleIndex;
    connection.Barycentric = vertex.Barycentric;
    connection.DirectionWorld = connectionWorld;
    connection.PositionWorld = vertex.PositionWorld;
    connection.DistanceWorld = connectionDistance;
    connection.HasFiniteEndpoint = true;
    connection.HasSample = true;
    if (TestVisibility(scene, queries, receiver, connection) != SceneQuery::Miss)
        return float3(0);
    float transmission = mode == 1 ? (TestVisibility(scene, queries, vertex, light) == SceneQuery::Miss ? 1 : 0)
                                   : EvaluateOpticalVisibility(scene, vertex, light, wavelengthWorld, queries);
    return contribution * transmission;
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
    // Every strategy must be able to run, so that the weights of the others stay exact.
    bool isBidirectional = WT_BIDIRECTIONAL == WAVE_BIDIRECTIONAL && mode != 2;
    if (isBidirectional == true &&
        WAVE_BIDIRECTIONAL_VISIBILITY_QUERIES_PER_VERTEX * WT_PATH_VERTICES - WAVE_LIGHT_VERTEX_QUERIES >
            queries.MaximumVisibilityQueries)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_VISIBILITY_BUDGET);
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
    // The rough vertex before this one and its BSDF density for the direction that reached this one, for the
    // light-vertex weights; none after the camera or a glass event.
    bool hasRoughPrevious = false;
    float3 previousWorld = float3(0);
    float previousPdf = 0;
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
                float weight = isBidirectional == true && hasRoughPrevious == true
                                   ? ComputeWaveNextEventWeight(scene,
                                                                hit,
                                                                geometricNormalWorld,
                                                                light,
                                                                previousWorld,
                                                                previousPdf,
                                                                queries)
                                   : 1;
                radiance += throughput * ProjectWaveSpectrum(wavelengthNanometers, value) *
                            ProjectWaveSpectrum(wavelengthNanometers, light.IncidentCoefficient) *
                            (cosine * transmission * weight / probability);
            }
        }
        if (mode == 2 || depth + 1 == WT_PATH_VERTICES)
            break;
        if (isBidirectional == true && isDielectric == false)
            radiance += throughput * ConnectWaveLightVertex(scene,
                                                            launch,
                                                            hit,
                                                            material,
                                                            viewWorld,
                                                            geometricNormalWorld,
                                                            depth,
                                                            wavelengthNanometers,
                                                            wavelengthWorld,
                                                            mode,
                                                            queries);
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
        hasRoughPrevious = continuation.IsSpecular == false;
        previousWorld = hit.PositionWorld;
        previousPdf = continuation.Pdf;
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
