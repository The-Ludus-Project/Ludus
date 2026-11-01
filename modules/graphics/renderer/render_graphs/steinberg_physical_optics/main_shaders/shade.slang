#include "hit_record.slang"
#include "wave_optics.slang"

// Passes 2 to 4 (scene compute): physical light transport after Shlomi Steinberg, Pradeep Sen and Ling-Qi Yan,
// "Towards Practical Physical-Optics Rendering", ACM Transactions on Graphics 41(4), article 132, SIGGRAPH 2022,
// https://doi.org/10.1145/3528223.3530119 (wave_optics.slang has the formalism and its departures).
//
// Section 4's method: paths are sampled with coherence and polarization ignored, through the maximally incoherent
// envelope of each material (Eqs. (23), (24) and (31)), and only once a complete light-to-sensor path is known is it
// evaluated with the full coherence-aware, polarimetric formalism, from the light towards the sensor. Each pixel
// traces one camera path of up to PO_PATH_VERTICES scattering events from the recorded primary hit. Every vertex
// draws one lobe of its material (one-sample lobe selection, so each lobe's scattered beam carries its own shape
// matrices), connects to one light (next-event estimation) and continues along a direction drawn from that lobe;
// light found by the continuation is combined with the connection by the power heuristic. Distinct beams are
// mutually incoherent and add (Property 3.2).
// - ShadeWave (the color output) evaluates each path with beams: Stokes vectors, shape matrices from Eq. (25) at the
//   light, D and Eq. (20) at every interaction, and an optional linear polarizer in front of the camera.
// - ShadeRadiometric (the radiometric output) traces the same paths and evaluates them classically: unpolarized
//   scalar radiance and D in its coherent limit, the material's power spectral density alone. Gratings are
//   conductors of their own roughness there, as the paper's radiometric comparison renders them as ordinary rough
//   conductors (section 5).
// - MeasureCoherence (the coherence output) writes the characteristic transverse coherence length of the light
//   that the first vertex's connection receives, at PO_COHERENCE_WAVELENGTH_NANOMETERS, divided by
//   PO_COHERENCE_DISPLAY_MICROMETERS; zero where that light is occluded.
// Materials (section 4.1) from the scene's: rough surfaces are Gaussian surfaces (4.1.1) with the material's
// complex index (conductors) or PO_DIELECTRIC_ETA (dielectrics); a dielectric's base is perfectly diffuse (4.1.2);
// metals at or below PO_GRATING_ROUGHNESS_MAXIMUM are sinusoidal gratings (4.1.3).
// Departures: camera paths with next-event estimation replace the paper's bidirectional tracer (section 4); the
// scene's roughness is mapped to the Gaussian surface's parameters (MakeOpticalMaterial); a dielectric's diffuse base
// is weighted by the interface's transmittance, as a layer, where the paper renders single-interface materials;
// birefringent slabs (4.1.4) and transmission are not rendered; four hero wavelengths per path replace the paper's 16
// spectral samples, except that a grating connected to a point, spot or directional light at the first vertex is
// integrated over each order's wavelength window (EstimateEndpoint); point and spot lights are radiators of
// area PO_RADIATOR_AREA_SQUARE_MILLIMETERS for coherence and points for visibility; the environment arrives with the
// envelope's coherence sigma_min, the least coherent light the paper renders.
// At most PO_PATH_VERTICES surface queries (continuations) and as many visibility queries (connections) per invocation.
// Every pass traverses the compute BVH (library.lurender.json): with native Metal ray queries, these paths returned
// non-finite radiance in many pixels, differing between identical runs.

static const float SHADE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;
/// Scene interface budgets: one continuation and one connection per vertex.
static const uint PATH_VERTEX_CAPACITY = 3;
/// Smallest GGX alpha mapped to a Gaussian surface; its correlation length is then about 44 um.
static const float OPTICS_MINIMUM_ALPHA = 1e-3;
/// Largest reflectance converted to a complex index (Gulbrandsen's index diverges at one).
static const float OPTICS_MAXIMUM_CONVERTED_REFLECTANCE = 0.99;
/// Wavelength-window half-width of a grating order, in standard deviations of its coherence Gaussian.
static const float GRATING_WINDOW_DEVIATIONS = 6;
/// Stratified wavelengths per grating wavelength window.
static const uint GRATING_WINDOW_POINTS = 4;
static const float RADIANS_PER_DEGREE = PI / 180;
/// Channels of an RGB optical constant.
static const uint RGB_CHANNEL_COUNT = 3;
/// selectedOrder value that evaluates a grating's every order; any other value selects one order, negative or not.
static const int GRATING_EVERY_ORDER = 0x7fffffff;

/// Lobes of the scene's materials (section 4.1).
static const uint LOBE_NONE = 0;
static const uint LOBE_CONDUCTOR_DIRECT = 1;
static const uint LOBE_CONDUCTOR_SCATTERED = 2;
static const uint LOBE_DIELECTRIC_DIRECT = 3;
static const uint LOBE_DIELECTRIC_SCATTERED = 4;
static const uint LOBE_DIFFUSE = 5;
static const uint LOBE_GRATING = 6;
static const uint LOBE_COUNT = 7;

/// Kinds of light endpoint, by the coherence they emit.
static const uint ENDPOINT_RADIATOR = 0;    ///< Mesh emitters, point and spot lights: Eq. (25) with A and Omega.
static const uint ENDPOINT_SUN = 1;         ///< Directional lights: sunlight's coherence.
static const uint ENDPOINT_ENVIRONMENT = 2; ///< The environment: the envelope's sigma_min.

/// A scene material as the paper's materials, before the wavelength is known.
struct OpticalMaterial
{
    float ConductorWeight;  ///< Metallic: the conductor's share; the dielectric and its base share the rest.
    float3 ConductorEtaRgb; ///< Complex index n + i kappa of the conductor, per uplift band.
    float3 ConductorExtinctionRgb;
    bool HasDielectricInterface; ///< False for the purely diffuse model.
    float DielectricEta;
    float3 DiffuseAlbedoRgb;
    float RmsHeightWorld;         ///< q of the Gaussian surface (section 4.1.1).
    float CorrelationLengthWorld; ///< sigma_s.
    bool IsGrating;
};

/// A scattering event of the camera path, rebuilt from the scene where it is evaluated.
struct ShadingVertex
{
    float3 PositionWorld;
    float3 OutgoingWorld; ///< Unit direction towards the previous vertex or the camera.
    SurfaceFrame Frame;
    OpticalMaterial Material;
    Grating Profile;
    uint Lobe;
    float LobeProbability;
    /// Scalar factor of light that arrives through the continuation: cos / (P_lobe p_lobe), or 1 / P_lobe for a
    /// delta lobe.
    float ContinuationWeight;
};

/// What the camera path keeps of a vertex: its surface point, from which TryRebuildShadingVertex rebuilds the frame
/// and material, and the draws made there. Metal can lose stores to large thread-local state in big kernels (see the
/// render-graph authoring guide), so the path keeps these nine words per vertex rather than whole ShadingVertex values.
struct PathVertex
{
    uint TriangleIndex;
    float2 Barycentric;
    float3 PositionWorld;
    uint Lobe;
    float LobeProbability;
    float ContinuationWeight;
};

/// The camera path in fixed members, which Slang keeps more reliably than local arrays of structures.
struct CameraPath
{
    PathVertex First;
    PathVertex Second;
    PathVertex Third;
    float3 ViewWorld; ///< Unit direction from the first vertex towards the camera.
};

PathVertex GetPathVertex(CameraPath path, uint index)
{
    if (index == 0)
        return path.First;
    if (index == 1)
        return path.Second;
    return path.Third;
}

void SetPathVertex(inout CameraPath path, uint index, PathVertex vertex)
{
    if (index == 0)
        path.First = vertex;
    else if (index == 1)
        path.Second = vertex;
    else
        path.Third = vertex;
}

/// Light that reaches the last vertex of a sub-path, before its wavelength is chosen.
struct LightEndpoint
{
    uint Kind;
    float3 RadianceRgb;      ///< Incident radiance (or a discrete light's incident coefficient), linear RGB.
    float3 PropagationWorld; ///< Unit direction of travel, towards the receiving vertex.
    float DistanceWorld;     ///< Propagation distance from the radiator; ignored for the sun and environment.
    float Weight;            ///< Receiving cosine over the technique's density, times its MIS weight.
    bool IsDiscrete;         ///< A point, spot or directional light.
};

/// Settings that every beam evaluation needs.
struct OpticsSettings
{
    float RadiatorShape; ///< theta of radiators (Eq. (25)).
    float SunShape;
    float LongitudinalCoherenceWorld;
    float EnvelopeDistanceWorld;
    bool HasPolarizer;
};

OpticsSettings GetOpticsSettings()
{
    OpticsSettings result;
    result.RadiatorShape =
        ComputeRadiatorShape(PO_RADIATOR_AREA_SQUARE_MILLIMETERS * SQUARE_WORLD_UNITS_PER_SQUARE_MILLIMETER,
                             PO_BEAM_SOLID_ANGLE_MICROSTERADIANS * STERADIANS_PER_MICROSTERADIAN);
    result.SunShape = ComputeSunShape(PO_SUN_COHERENCE_MICROMETERS * WORLD_UNITS_PER_MICROMETER);
    result.LongitudinalCoherenceWorld = PO_LONGITUDINAL_COHERENCE_MILLIMETERS * WORLD_UNITS_PER_MILLIMETER;
    result.EnvelopeDistanceWorld = PO_ENVELOPE_DISTANCE_CENTIMETERS * WORLD_UNITS_PER_CENTIMETER;
    result.HasPolarizer = PO_CAMERA_POLARIZER;
    return result;
}

float ComputeWavenumberPerWorld(float wavelengthNanometers)
{
    return 2 * PI * NANOMETERS_PER_WORLD_UNIT / wavelengthNanometers;
}

/// The channel of an RGB optical constant that covers a wavelength: the uplift's bands (spectrum.slang).
float SelectBandChannel(float3 rgb, float wavelengthNanometers)
{
    if (wavelengthNanometers >= SPECTRUM_RED_BAND_MINIMUM_NANOMETERS)
        return rgb.x;
    if (wavelengthNanometers >= SPECTRUM_GREEN_BAND_MINIMUM_NANOMETERS)
        return rgb.y;
    return rgb.z;
}

// Thanks to Ole Gulbrandsen, "Artist Friendly Metallic Fresnel", Journal of Computer Graphics Techniques 3(4), 2014,
// https://jcgt.org/published/0003/04/03/ : the complex index whose normal reflectance is r, with edge tint g. The
// metallic-roughness model has no edge tint; we use g = r.
/// Gulbrandsen's (n, kappa) for one reflectance channel, with g = r.
float2 ConvertReflectanceToIndex(float reflectance)
{
    float r = clamp(reflectance, 0.0, OPTICS_MAXIMUM_CONVERTED_REFLECTANCE);
    float rootReflectance = sqrt(r);
    float minimumIndex = (1 - r) / (1 + r);
    float maximumIndex = (1 + rootReflectance) / (1 - rootReflectance);
    float n = r * minimumIndex + (1 - r) * maximumIndex;
    float extinctionSquared = ((n + 1) * (n + 1) * r - (n - 1) * (n - 1)) / (1 - r);
    return float2(n, sqrt(max(extinctionSquared, 0.0)));
}

/// Metals at or below PO_GRATING_ROUGHNESS_MAXIMUM are gratings (as in the steinberg-plt template): the conductor
/// model, or the metallic-roughness model with Metallic of at least one half.
bool IsGratingMaterial(MaterialSample material, float alpha)
{
    bool isMetal = material.Model == LUDUS_MATERIAL_MODEL_CONDUCTOR ||
                   (material.Model == LUDUS_MATERIAL_MODEL_METALLIC_ROUGHNESS && material.Metallic >= 0.5);
    return isMetal == true && alpha <= PO_GRATING_ROUGHNESS_MAXIMUM * PO_GRATING_ROUGHNESS_MAXIMUM;
}

/// The paper's materials for a scene material. The paper parametrizes a Gaussian surface by its rms height q and
/// correlation length sigma_s (section 4.1.1); the scene gives a GGX alpha. We choose sigma_s so that the coherent
/// lobe's half-vector spread, (k sigma_s)^-1 in h~, is 2 alpha at OPTICS_REFERENCE_WAVELENGTH_NANOMETERS, and
/// q = sqrt(alpha) PO_RMS_HEIGHT_AT_FULL_ROUGHNESS_NANOMETERS, so that smooth surfaces keep most of their direct
/// lobe and rough ones scatter it (Eq. (22)).
OpticalMaterial MakeOpticalMaterial(MaterialSample material)
{
    OpticalMaterial result = (OpticalMaterial)0;
    float alpha = ComputeMaterialAlpha(material.Model, material.Roughness, max(material.AlphaU, material.AlphaV));
    result.IsGrating = IsGratingMaterial(material, alpha);
    float clampedAlpha = max(alpha, OPTICS_MINIMUM_ALPHA);
    float referenceWavelengthWorld = OPTICS_REFERENCE_WAVELENGTH_NANOMETERS / NANOMETERS_PER_WORLD_UNIT;
    result.CorrelationLengthWorld = referenceWavelengthWorld / (4 * PI * clampedAlpha);
    result.RmsHeightWorld = sqrt(alpha) * PO_RMS_HEIGHT_AT_FULL_ROUGHNESS_NANOMETERS / NANOMETERS_PER_WORLD_UNIT;
    result.DiffuseAlbedoRgb = material.BaseColor;
    result.DielectricEta = PO_DIELECTRIC_ETA;
    result.HasDielectricInterface = true;
    if (material.Model == LUDUS_MATERIAL_MODEL_CONDUCTOR)
    {
        result.ConductorWeight = 1;
        result.ConductorEtaRgb = material.Eta;
        result.ConductorExtinctionRgb = material.K;
    }
    else if (material.Model == LUDUS_MATERIAL_MODEL_DIFFUSE)
    {
        result.HasDielectricInterface = false;
    }
    else if (material.Model == LUDUS_MATERIAL_MODEL_COATED_DIFFUSE)
    {
        if (material.CoatingEta > 1)
            result.DielectricEta = material.CoatingEta;
    }
    else
    {
        result.ConductorWeight = saturate(material.Metallic);
        float2 red = ConvertReflectanceToIndex(material.BaseColor.x);
        float2 green = ConvertReflectanceToIndex(material.BaseColor.y);
        float2 blue = ConvertReflectanceToIndex(material.BaseColor.z);
        result.ConductorEtaRgb = float3(red.x, green.x, blue.x);
        result.ConductorExtinctionRgb = float3(red.y, green.y, blue.y);
    }
    if (result.IsGrating == true)
        result.ConductorWeight = 1;
    return result;
}

Complex GetConductorEta(OpticalMaterial material, float wavelengthNanometers)
{
    return MakeComplex(SelectBandChannel(material.ConductorEtaRgb, wavelengthNanometers),
                       SelectBandChannel(material.ConductorExtinctionRgb, wavelengthNanometers));
}

/// Mean of the conductor's unpolarized reflectance over its three channels.
float ComputeMeanConductorReflectance(OpticalMaterial material, float cosine)
{
    float sum = 0;
    for (uint channel = 0; channel < RGB_CHANNEL_COUNT; ++channel)
    {
        Complex eta = MakeComplex(material.ConductorEtaRgb[channel], material.ConductorExtinctionRgb[channel]);
        sum += ComputeUnpolarizedReflectance(cosine, eta);
    }
    return sum / float(RGB_CHANNEL_COUNT);
}

float ComputeDielectricReflectance(OpticalMaterial material, float cosine)
{
    if (material.HasDielectricInterface == false)
        return 0;
    return ComputeUnpolarizedReflectance(cosine, MakeComplex(material.DielectricEta, 0));
}

Grating MakeGrating()
{
    Grating result;
    result.AxisSurface = float2(0, 1);
    result.PeriodWorld = PO_GRATING_PERIOD_NANOMETERS / NANOMETERS_PER_WORLD_UNIT;
    result.HeightWorld = PO_GRATING_HEIGHT_NANOMETERS / NANOMETERS_PER_WORLD_UNIT;
    result.MaximumOrder = min(PO_GRATING_ORDER_MAXIMUM, OPTICS_GRATING_MAXIMUM_ORDER);
    return result;
}

/// The grooves follow the surface tangent when the mesh has one, otherwise world +X projected onto the surface, or
/// world +Z where +X is nearly normal to it (as in the steinberg-plt template). The frame's tangent is the groove, so
/// the grating vector is the bitangent: Grating.AxisSurface = (0, 1).
float3 ChooseTangentHintWorld(float3 normalWorld, float4 tangentWorld)
{
    if (abs(tangentWorld.w) > 0.5)
    {
        float3 projectedWorld = tangentWorld.xyz - normalWorld * dot(tangentWorld.xyz, normalWorld);
        if (dot(projectedWorld, projectedWorld) >= OPTICS_MINIMUM_AXIS_LENGTH_SQUARED)
            return projectedWorld;
    }
    float3 xWorld = float3(1, 0, 0) - normalWorld * normalWorld.x;
    if (dot(xWorld, xWorld) >= OPTICS_MINIMUM_AXIS_LENGTH_SQUARED)
        return xWorld;
    return float3(0, 0, 1) - normalWorld * normalWorld.z;
}

// ---------------------------------------------------------------------------------------------------------------
// Lobe selection and sampling, independent of the wavelength and of coherence (section 4).

/// Selection weights of the lobes at a vertex, from the view-side cosine at OPTICS_REFERENCE_WAVELENGTH_NANOMETERS.
/// The radiometric pass renders a grating as a rough conductor (isWave false). Named fields, not a local array:
/// Metal can lose stores to thread arrays (see the render-graph authoring guide).
struct LobeWeights
{
    float ConductorDirect;
    float ConductorScattered;
    float DielectricDirect;
    float DielectricScattered;
    float Diffuse;
    float Grating;
};

LobeWeights ComputeLobeWeights(OpticalMaterial material, float outgoingCosine, bool isWave)
{
    LobeWeights result = (LobeWeights)0;
    if (material.IsGrating == true && isWave == true)
    {
        result.Grating = 1;
        return result;
    }
    float direct = ComputeDirectFraction(ComputeWavenumberPerWorld(OPTICS_REFERENCE_WAVELENGTH_NANOMETERS),
                                         outgoingCosine,
                                         material.RmsHeightWorld);
    float conductor = material.ConductorWeight * ComputeMeanConductorReflectance(material, outgoingCosine);
    float dielectricReflectance = ComputeDielectricReflectance(material, outgoingCosine);
    float dielectric = (1 - material.ConductorWeight) * dielectricReflectance;
    result.ConductorDirect = conductor * direct;
    result.ConductorScattered = conductor * (1 - direct);
    result.DielectricDirect = dielectric * direct;
    result.DielectricScattered = dielectric * (1 - direct);
    result.Diffuse = (1 - material.ConductorWeight) * (1 - dielectricReflectance) *
                     dot(material.DiffuseAlbedoRgb, float3(1.0 / 3.0));
    return result;
}

float GetLobeWeight(LobeWeights weights, uint lobe)
{
    if (lobe == LOBE_CONDUCTOR_DIRECT)
        return weights.ConductorDirect;
    if (lobe == LOBE_CONDUCTOR_SCATTERED)
        return weights.ConductorScattered;
    if (lobe == LOBE_DIELECTRIC_DIRECT)
        return weights.DielectricDirect;
    if (lobe == LOBE_DIELECTRIC_SCATTERED)
        return weights.DielectricScattered;
    if (lobe == LOBE_DIFFUSE)
        return weights.Diffuse;
    if (lobe == LOBE_GRATING)
        return weights.Grating;
    return 0;
}

/// Draw a lobe; outProbability is its selection probability. LOBE_NONE when no lobe reflects.
uint SelectLobe(OpticalMaterial material,
                float outgoingCosine,
                bool isWave,
                float randomValue,
                out float outProbability)
{
    LobeWeights weights = ComputeLobeWeights(material, outgoingCosine, isWave);
    float total = weights.ConductorDirect + weights.ConductorScattered + weights.DielectricDirect +
                  weights.DielectricScattered + weights.Diffuse + weights.Grating;
    outProbability = 0;
    if (total <= 0)
        return LOBE_NONE;
    float target = randomValue * total;
    uint selected = LOBE_NONE;
    [loop] for (uint candidate = 1; candidate < LOBE_COUNT; ++candidate)
    {
        float weight = GetLobeWeight(weights, candidate);
        if (weight <= 0)
            continue;
        selected = candidate;
        target -= weight;
        if (target < 0)
            break;
    }
    outProbability = GetLobeWeight(weights, selected) / total;
    return selected;
}

bool IsDeltaLobe(uint lobe)
{
    return lobe == LOBE_CONDUCTOR_DIRECT || lobe == LOBE_DIELECTRIC_DIRECT;
}

/// Variance of the envelope's half-vector Gaussian at one wavelength: (k^2 sigma_min)^-1 + (k sigma_s)^-2 for rough
/// surfaces (Eq. (31)), (k^2 sigma_min)^-1 alone for a grating's lobes.
float ComputeEnvelopeVariance(ShadingVertex vertex, float wavelengthNanometers, OpticsSettings settings)
{
    float variance =
        1 / ComputeEnvelopeCoherence(wavelengthNanometers, settings.EnvelopeDistanceWorld, settings.RadiatorShape);
    if (vertex.Lobe == LOBE_GRATING)
        return variance;
    float correlation = ComputeWavenumberPerWorld(wavelengthNanometers) * vertex.Material.CorrelationLengthWorld;
    return variance + 1 / (correlation * correlation);
}

/// Solid-angle density with which SampleLobeDirection draws incidentWorld at the sampling wavelength (zero for delta
/// lobes, whose density is not a function).
float EvaluateLobeDensity(ShadingVertex vertex,
                          float3 incidentWorld,
                          float wavelengthNanometers,
                          OpticsSettings settings)
{
    float cosine = dot(incidentWorld, vertex.Frame.NormalWorld);
    if (cosine <= 0 || IsDeltaLobe(vertex.Lobe) == true)
        return 0;
    if (vertex.Lobe == LOBE_DIFFUSE)
        return cosine / PI;
    float2 halfVector = ComputeSurfaceHalfVector(vertex.Frame, incidentWorld, vertex.OutgoingWorld);
    float variance = ComputeEnvelopeVariance(vertex, wavelengthNanometers, settings);
    float3 covariance = float3(variance, 0, variance);
    if (vertex.Lobe != LOBE_GRATING)
        return ConvertHalfVectorDensity(EvaluateBivariateNormal(halfVector, covariance), vertex.Frame, incidentWorld);
    // Section 4.1.3: one of the 2N + 1 lobes uniformly, then its Gaussian; the lobes overlap, so the density sums them.
    float wavelengthOverPeriod = wavelengthNanometers / (vertex.Profile.PeriodWorld * NANOMETERS_PER_WORLD_UNIT);
    int maximumOrder = int(vertex.Profile.MaximumOrder);
    float sum = 0;
    [loop] for (int order = -maximumOrder; order <= maximumOrder; ++order)
    {
        float2 orderHalfVector = halfVector - vertex.Profile.AxisSurface * (float(order) * wavelengthOverPeriod);
        sum += EvaluateBivariateNormal(orderHalfVector, covariance);
    }
    return ConvertHalfVectorDensity(sum / float(2 * maximumOrder + 1), vertex.Frame, incidentWorld);
}

/// Draw the continuation's incident direction from the vertex's lobe. Returns false when the draw leaves the
/// hemisphere, which contributes nothing (the densities above are defined over the whole h~ plane).
bool SampleLobeDirection(ShadingVertex vertex,
                         float wavelengthNanometers,
                         OpticsSettings settings,
                         float3 randomValues,
                         out float3 outIncidentWorld)
{
    outIncidentWorld = float3(0);
    if (IsDeltaLobe(vertex.Lobe) == true)
    {
        outIncidentWorld =
            2 * dot(vertex.OutgoingWorld, vertex.Frame.NormalWorld) * vertex.Frame.NormalWorld - vertex.OutgoingWorld;
        return dot(outIncidentWorld, vertex.Frame.NormalWorld) > 0;
    }
    if (vertex.Lobe == LOBE_DIFFUSE)
    {
        float radius = sqrt(randomValues.x);
        float angle = 2 * PI * randomValues.y;
        float2 disk = radius * float2(cos(angle), sin(angle));
        outIncidentWorld = disk.x * vertex.Frame.TangentWorld + disk.y * vertex.Frame.BitangentWorld +
                           sqrt(max(1 - randomValues.x, 0.0)) * vertex.Frame.NormalWorld;
        return true;
    }
    float2 halfVector =
        SampleEnvelopeHalfVector(ComputeEnvelopeVariance(vertex, wavelengthNanometers, settings), randomValues.xy);
    if (vertex.Lobe == LOBE_GRATING)
    {
        int maximumOrder = int(vertex.Profile.MaximumOrder);
        int order = min(int(randomValues.z * float(2 * maximumOrder + 1)), 2 * maximumOrder) - maximumOrder;
        float wavelengthOverPeriod = wavelengthNanometers / (vertex.Profile.PeriodWorld * NANOMETERS_PER_WORLD_UNIT);
        halfVector += vertex.Profile.AxisSurface * (float(order) * wavelengthOverPeriod);
    }
    return TryMakeDirectionFromHalfVector(vertex.Frame, vertex.OutgoingWorld, halfVector, outIncidentWorld);
}

// ---------------------------------------------------------------------------------------------------------------
// Beam evaluation of a complete path at one wavelength.

/// The beam that a light endpoint sends to its receiving vertex at one wavelength: Eq. (25)'s natural light.
Beam MakeLightBeam(LightEndpoint endpoint, float wavelengthNanometers, OpticsSettings settings)
{
    float radiance = EvaluateUpliftedSpectrum(endpoint.RadianceRgb, wavelengthNanometers);
    if (endpoint.Kind == ENDPOINT_SUN)
        return MakeNaturalBeam(endpoint.PropagationWorld, radiance, settings.SunShape, OPTICS_SUN_DISTANCE_WORLD);
    if (endpoint.Kind == ENDPOINT_ENVIRONMENT)
    {
        // k^2 Theta = k^2 sigma_min: a radiator beam after the envelope's distance at the shortest wavelength.
        float distanceWorld =
            settings.EnvelopeDistanceWorld * SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS / wavelengthNanometers;
        return MakeNaturalBeam(endpoint.PropagationWorld, radiance, settings.RadiatorShape, distanceWorld);
    }
    return MakeNaturalBeam(endpoint.PropagationWorld, radiance, settings.RadiatorShape, endpoint.DistanceWorld);
}

/// Transverse components of a beam that Eq. (16) diffracts separately: x, y and the cross term 1/2.
static const uint BEAM_COMPONENT_COUNT = 3;

/// Eq. (16)'s D for each transverse component of the beam at a rough-surface or grating lobe. isWave false is the
/// radiometric coherent limit (Sigma^-1 = 0). selectedOrder GRATING_EVERY_ORDER sums every grating order; any other
/// value evaluates that order alone. The components share one loop, so the kernel inlines each D once.
[noinline] ComponentDiffraction EvaluateComponentDiffraction(ShadingVertex vertex,
                                                             Beam beam,
                                                             float3 incidentWorld,
                                                             float wavelengthNanometers,
                                                             OpticsSettings settings,
                                                             bool isWave,
                                                             int selectedOrder) {
    float wavenumber = ComputeWavenumberPerWorld(wavelengthNanometers);
    float2 halfVector = ComputeSurfaceHalfVector(vertex.Frame, incidentWorld, vertex.OutgoingWorld);
    Grating grating = vertex.Profile;
    float wavelengthOverPeriod = wavelengthNanometers / (grating.PeriodWorld * NANOMETERS_PER_WORLD_UNIT);
    float phaseAmplitude = ComputeGratingPhaseAmplitude(grating,
                                                        wavenumber,
                                                        dot(incidentWorld, vertex.Frame.NormalWorld),
                                                        dot(vertex.OutgoingWorld, vertex.Frame.NormalWorld));
    int lastOrder = int(min(grating.MaximumOrder, OPTICS_GRATING_MAXIMUM_ORDER));
    int firstOrder = -lastOrder;
    // One order of Eq. (34): EstimateEndpoint integrates each order over its own wavelength window.
    if (selectedOrder != GRATING_EVERY_ORDER)
    {
        firstOrder = selectedOrder;
        lastOrder = selectedOrder;
    }
    float correlation = wavenumber * vertex.Material.CorrelationLengthWorld;
    float inverseCorrelationSquared = 1 / (correlation * correlation);
    ComponentDiffraction result;
    result.X = 0;
    result.Y = 0;
    result.Half = 0;
    [loop] for (uint component = 0; component < BEAM_COMPONENT_COUNT; ++component)
    {
        float3 shape = 0.5 * (beam.ShapeX + beam.ShapeY);
        if (component == 0)
            shape = beam.ShapeX;
        else if (component == 1)
            shape = beam.ShapeY;
        SurfaceCoherence coherence = MakeCoherentLimit();
        if (isWave == true)
            coherence =
                ComputeSurfaceCoherence(beam, shape, vertex.Frame, wavenumber, settings.LongitudinalCoherenceWorld);
        float value = 0;
        if (vertex.Lobe == LOBE_GRATING)
            value = EvaluateGratingDiffraction(grating,
                                               halfVector,
                                               coherence,
                                               wavelengthOverPeriod,
                                               phaseAmplitude,
                                               firstOrder,
                                               lastOrder);
        else
            value = EvaluateGaussianSurfaceDiffraction(halfVector, coherence, inverseCorrelationSquared);
        if (component == 0)
            result.X = value;
        else if (component == 1)
            result.Y = value;
        else
            result.Half = value;
    }
    return result;
}

/// The interaction of a vertex's lobe with one beam at one wavelength (Eqs. (16), (21), (22), (32)).
Interaction MakeInteraction(ShadingVertex vertex,
                            Beam beam,
                            float wavelengthNanometers,
                            OpticsSettings settings,
                            bool isWave,
                            int selectedOrder)
{
    Interaction result;
    result.Diffraction.X = 1;
    result.Diffraction.Y = 1;
    result.Diffraction.Half = 1;
    result.HasFresnel = true;
    result.HasShapeDeformation = false;
    result.Weight = 0;
    float3 incidentWorld = -beam.PropagationWorld;
    float incidentCosine = dot(incidentWorld, vertex.Frame.NormalWorld);
    if (incidentCosine <= 0 || dot(vertex.OutgoingWorld, vertex.Frame.NormalWorld) <= 0)
        return result;
    OpticalMaterial material = vertex.Material;
    float wavenumber = ComputeWavenumberPerWorld(wavelengthNanometers);
    float direct = ComputeDirectFraction(wavenumber, incidentCosine, material.RmsHeightWorld);
    bool isConductorLobe =
        vertex.Lobe == LOBE_CONDUCTOR_DIRECT || vertex.Lobe == LOBE_CONDUCTOR_SCATTERED || vertex.Lobe == LOBE_GRATING;
    result.Eta = isConductorLobe == true ? GetConductorEta(material, wavelengthNanometers)
                                         : MakeComplex(material.DielectricEta, 0);
    float share = isConductorLobe == true ? material.ConductorWeight : 1 - material.ConductorWeight;
    if (vertex.Lobe == LOBE_CONDUCTOR_DIRECT || vertex.Lobe == LOBE_DIELECTRIC_DIRECT)
    {
        result.Weight = share * direct;
        return result;
    }
    if (vertex.Lobe == LOBE_DIFFUSE)
    {
        // Eq. (32): m00 / pi with no polarizing action, under the interface's transmittance at incidence.
        result.HasFresnel = false;
        result.Weight = share * EvaluateUpliftedSpectrum(material.DiffuseAlbedoRgb, wavelengthNanometers) *
                        (1 - ComputeDielectricReflectance(material, incidentCosine)) / PI;
        return result;
    }
    result.Diffraction = EvaluateComponentDiffraction(vertex,
                                                      beam,
                                                      incidentWorld,
                                                      wavelengthNanometers,
                                                      settings,
                                                      isWave,
                                                      selectedOrder);
    if (vertex.Lobe == LOBE_GRATING)
    {
        // Every order of Eq. (34) is in the PSD, the DC order included: no separate direct lobe.
        result.Weight = share;
        return result;
    }
    result.Weight = share * (1 - direct);
    result.HasShapeDeformation = isWave;
    return result;
}

/// The intensity that the camera records from a beam arriving along -cameraBeam.PropagationWorld: S0, or what a
/// linear polarizer with transmission axis polarizerAxisWorld passes.
float ReadSensorIntensity(Beam beam, OpticsSettings settings, float3 polarizerAxisWorld)
{
    if (settings.HasPolarizer == false)
        return beam.Stokes.x;
    float3 yWorld = GetTransverseYWorld(beam);
    float angle = atan2(dot(polarizerAxisWorld, yWorld), dot(polarizerAxisWorld, beam.TransverseXWorld));
    return ApplyLinearPolarizer(beam.Stokes, angle);
}

/// Spectral radiance at the camera from light that reaches path vertex lastVertex from endpoint and is scattered
/// back along the path: light to sensor, as section 4 evaluates complete paths. selectedOrder restricts a grating at
/// the last vertex to one order (EstimateEndpoint); GRATING_EVERY_ORDER keeps every order.
[noinline] float EvaluatePathRadiance(SceneContext scene,
                                      CameraPath path,
                                      uint lastVertex,
                                      LightEndpoint endpoint,
                                      float wavelengthNanometers,
                                      OpticsSettings settings,
                                      bool isWave,
                                      float3 polarizerAxisWorld,
                                      int selectedOrder) {
    Beam beam = MakeLightBeam(endpoint, wavelengthNanometers, settings);
    if (beam.Stokes.x <= 0)
        return 0;
    [loop] for (int index = int(lastVertex); index >= 0; --index)
    {
        ShadingVertex vertex;
        if (TryRebuildShadingVertex(scene, path, uint(index), vertex) == false)
            return 0;
        int order = index == int(lastVertex) ? selectedOrder : GRATING_EVERY_ORDER;
        Interaction interaction = MakeInteraction(vertex, beam, wavelengthNanometers, settings, isWave, order);
        if (interaction.Weight <= 0)
            return 0;
        beam = ScatterBeam(beam, vertex.OutgoingWorld, interaction);
        if (isWave == false)
            beam.Stokes = float4(beam.Stokes.x, 0, 0, 0);
        beam.Stokes *= index == int(lastVertex) ? endpoint.Weight : vertex.ContinuationWeight;
        if (index > 0)
            beam =
                PropagateBeam(beam, length(GetPathVertex(path, uint(index - 1)).PositionWorld - vertex.PositionWorld));
    }
    if (isWave == false)
        return settings.HasPolarizer == true ? 0.5 * beam.Stokes.x : beam.Stokes.x;
    return ReadSensorIntensity(beam, settings, polarizerAxisWorld);
}

/// Wavelength windows of a grating's orders at the first vertex, lit by a discrete light. The half vector does not
/// depend on the wavelength there, so order n's Gaussian is centred on the wavelength lambda_n = (h~ . g) Lambda / n
/// and has no weight more than GRATING_WINDOW_DEVIATIONS of its deviations away. The DC order's direction does not
/// depend on the wavelength, so its window is the whole range.
struct GratingWindows
{
    float Along; ///< h~ . g
    float Reach; ///< GRATING_WINDOW_DEVIATIONS deviations of the orders along g.
    float PeriodNanometers;
    int MaximumOrder;
    bool IsEmpty; ///< The half vector lies too far across the grooves for any order.
};

GratingWindows PlanGratingWindows(ShadingVertex vertex, LightEndpoint endpoint, OpticsSettings settings)
{
    GratingWindows result;
    float2 halfVector = ComputeSurfaceHalfVector(vertex.Frame, -endpoint.PropagationWorld, vertex.OutgoingWorld);
    // The width uses the coherence at the reference wavelength; only the longitudinal term, far larger than the
    // transverse one, depends on the wavelength. An order is the Gaussian N(.; Sigma~^-1), whose variance along a
    // direction is that direction's quadratic form with Sigma~^-1.
    Beam beam = MakeLightBeam(endpoint, OPTICS_REFERENCE_WAVELENGTH_NANOMETERS, settings);
    SurfaceCoherence coherence =
        ComputeSurfaceCoherence(beam,
                                beam.ShapeX,
                                vertex.Frame,
                                ComputeWavenumberPerWorld(OPTICS_REFERENCE_WAVELENGTH_NANOMETERS),
                                settings.LongitudinalCoherenceWorld);
    float2 axis = vertex.Profile.AxisSurface;
    float2 groove = float2(axis.y, -axis.x);
    float2 axisBasis = RotateIntoCoherenceBasis(coherence, axis);
    float2 grooveBasis = RotateIntoCoherenceBasis(coherence, groove);
    float3 covariance = coherence.Inverse;
    float alongVariance = axisBasis.x * axisBasis.x * covariance.x + 2 * axisBasis.x * axisBasis.y * covariance.y +
                          axisBasis.y * axisBasis.y * covariance.z;
    float acrossVariance = grooveBasis.x * grooveBasis.x * covariance.x +
                           2 * grooveBasis.x * grooveBasis.y * covariance.y +
                           grooveBasis.y * grooveBasis.y * covariance.z;
    float across = dot(halfVector, groove);
    result.IsEmpty = across * across > GRATING_WINDOW_DEVIATIONS * GRATING_WINDOW_DEVIATIONS * acrossVariance;
    result.Along = dot(halfVector, axis);
    result.Reach = GRATING_WINDOW_DEVIATIONS * sqrt(alongVariance);
    result.PeriodNanometers = vertex.Profile.PeriodWorld * NANOMETERS_PER_WORLD_UNIT;
    result.MaximumOrder = int(min(vertex.Profile.MaximumOrder, OPTICS_GRATING_MAXIMUM_ORDER));
    return result;
}

/// The wavelength window of one order, within the sampled range; false when it is empty.
bool TryGetGratingWindow(GratingWindows windows, int order, out float outMinimum, out float outMaximum)
{
    outMinimum = SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS;
    outMaximum = SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS;
    if (windows.IsEmpty == true)
        return false;
    if (order == 0)
        return abs(windows.Along) <= windows.Reach;
    float firstBound = (windows.Along - windows.Reach) * windows.PeriodNanometers / float(order);
    float secondBound = (windows.Along + windows.Reach) * windows.PeriodNanometers / float(order);
    outMinimum = max(min(firstBound, secondBound), outMinimum);
    outMaximum = min(max(firstBound, secondBound), outMaximum);
    return outMinimum < outMaximum;
}

/// Linear RGB that light from endpoint sends to the camera through path vertices lastVertex to 0, integrated over the
/// wavelength. Usually the SPECTRUM_HERO_WAVELENGTH_COUNT hero wavelengths rotated by wavelengthRandom. A grating at
/// the first vertex lit by a discrete light is integrated over each order's window (PlanGratingWindows) instead, with
/// GRATING_WINDOW_POINTS stratified wavelengths offset by windowOffset in [0, 1), which is unbiased; uniform hero
/// wavelengths would find a coherent order's window of about a nanometre in few samples. Both share one loop, so the
/// kernel inlines the path evaluation once.
[noinline] float3 EstimateEndpoint(SceneContext scene,
                                   CameraPath path,
                                   uint lastVertex,
                                   ShadingVertex vertex,
                                   LightEndpoint endpoint,
                                   float wavelengthRandom,
                                   float windowOffset,
                                   OpticsSettings settings,
                                   bool isWave,
                                   float3 polarizerAxisWorld) {
    bool isWindowed = isWave == true && lastVertex == 0 && vertex.Lobe == LOBE_GRATING && endpoint.IsDiscrete == true;
    GratingWindows windows = (GratingWindows)0;
    uint sampleCount = SPECTRUM_HERO_WAVELENGTH_COUNT;
    if (isWindowed == true)
    {
        windows = PlanGratingWindows(vertex, endpoint, settings);
        sampleCount = uint(2 * windows.MaximumOrder + 1) * GRATING_WINDOW_POINTS;
    }
    float3 radiance = float3(0);
    [loop] for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        float wavelengthNanometers = SampleWavelengthNanometers(wavelengthRandom, sampleIndex);
        float weight = SPECTRUM_WAVELENGTH_RANGE_NANOMETERS / float(SPECTRUM_HERO_WAVELENGTH_COUNT);
        int order = GRATING_EVERY_ORDER;
        if (isWindowed == true)
        {
            order = int(sampleIndex / GRATING_WINDOW_POINTS) - windows.MaximumOrder;
            float windowMinimum;
            float windowMaximum;
            if (TryGetGratingWindow(windows, order, windowMinimum, windowMaximum) == false)
                continue;
            weight = (windowMaximum - windowMinimum) / float(GRATING_WINDOW_POINTS);
            float stratum = float(sampleIndex % GRATING_WINDOW_POINTS);
            wavelengthNanometers = windowMinimum + (stratum + windowOffset) * weight;
        }
        float spectral = EvaluatePathRadiance(scene,
                                              path,
                                              lastVertex,
                                              endpoint,
                                              wavelengthNanometers,
                                              settings,
                                              isWave,
                                              polarizerAxisWorld,
                                              order);
        radiance += spectral * EvaluateSensorSensitivity(wavelengthNanometers) * weight;
    }
    return radiance;
}

// ---------------------------------------------------------------------------------------------------------------
// Path tracing.

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
};

/// Rebuild this pixel's recorded surface. Returns false when the pixel needs no estimate: a primary miss
/// (outRadiance is the environment) or a failure (queries.HasFailed is set, outRadiance is 0).
bool TryLoadShadingPoint(SceneContext scene,
                         RayLaunch launch,
                         inout RayQueryContext queries,
                         out ShadingPoint outPoint,
                         out float3 outViewWorld,
                         out float3 outRadiance)
{
    outPoint = (ShadingPoint)0;
    outRadiance = float3(0);
    outViewWorld = float3(0);
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries,
                       HasHitRecordCapacity(launch) == false ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY
                                                             : LUDUS_SCENE_FAILURE_STORED_RECORD);
        return false;
    }
    // The primary pass traced this exact ray: same launch, camera and stored jitter.
    Ray cameraRay = MakeCameraRay(launch, record.JitterPixel);
    outViewWorld = -cameraRay.DirectionWorld;
    if (record.IsHit == false)
    {
        outRadiance = EvaluateEnvironment(scene, cameraRay.DirectionWorld);
        return false;
    }
    outPoint.Hit = GetSurfaceHit(scene, record.TriangleIndex, record.Barycentric);
    if (outPoint.Hit.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    // GetSurfaceHit issues no ray; restore what TraceClosest reported, as LoadVisibilitySurface does.
    outPoint.Hit.IncomingRay = cameraRay;
    outPoint.Hit.DistanceWorld = record.DistanceWorld;
    outPoint.Hit.IsFrontFace = record.IsFrontFace;
    outPoint.Material = EvaluateMaterial(scene, outPoint.Hit);
    float normalLengthSquared = dot(outPoint.Material.ShadingNormalWorld, outPoint.Material.ShadingNormalWorld);
    if (outPoint.Material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outPoint.ShadingNormalWorld = outPoint.Material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    outRadiance = EvaluateEmission(scene, outPoint.Hit, outViewWorld);
    return true;
}

/// The camera's linear polarizer axis: PO_CAMERA_POLARIZER_ANGLE_DEGREES from the image's right towards its up.
float3 ComputePolarizerAxisWorld(RayLaunch launch)
{
    float4 rightPositionWorld = mul(launch.CameraClipToWorld, float4(1, 0, 0, 1));
    float4 leftPositionWorld = mul(launch.CameraClipToWorld, float4(-1, 0, 0, 1));
    float4 topPositionWorld = mul(launch.CameraClipToWorld, float4(0, 1, 0, 1));
    float4 bottomPositionWorld = mul(launch.CameraClipToWorld, float4(0, -1, 0, 1));
    float3 rightWorld =
        NormalizeOrZero(rightPositionWorld.xyz / rightPositionWorld.w - leftPositionWorld.xyz / leftPositionWorld.w);
    float3 upWorld =
        NormalizeOrZero(topPositionWorld.xyz / topPositionWorld.w - bottomPositionWorld.xyz / bottomPositionWorld.w);
    float angle = PO_CAMERA_POLARIZER_ANGLE_DEGREES * RADIANS_PER_DEGREE;
    return cos(angle) * rightWorld + sin(angle) * upWorld;
}

/// The shading vertex at a surface, with the frame and material the scene gives it. Returns false when the material
/// does not reflect towards outgoingWorld, which ends the path.
bool TryMakeShadingVertex(SurfaceHit hit,
                          MaterialSample material,
                          float3 shadingNormalWorld,
                          float3 outgoingWorld,
                          out ShadingVertex outVertex)
{
    outVertex = (ShadingVertex)0;
    float outgoingCosine = dot(outgoingWorld, shadingNormalWorld);
    if (outgoingCosine <= 0 || dot(outgoingWorld, hit.GeometricNormalWorld) <= 0)
        return false;
    outVertex.PositionWorld = hit.PositionWorld;
    outVertex.OutgoingWorld = outgoingWorld;
    outVertex.Frame =
        MakeSurfaceFrame(shadingNormalWorld, ChooseTangentHintWorld(shadingNormalWorld, hit.TangentWorld));
    outVertex.Material = MakeOpticalMaterial(material);
    outVertex.Profile = MakeGrating();
    return true;
}

/// Rebuild path vertex index from the scene, without a ray: the same surface, material and frame that the camera path
/// built there. Returns false when the scene no longer gives a valid material.
[noinline] bool TryRebuildShadingVertex(SceneContext scene, CameraPath path, uint index, out ShadingVertex outVertex) {
    outVertex = (ShadingVertex)0;
    PathVertex stored = GetPathVertex(path, index);
    SurfaceHit hit = GetSurfaceHit(scene, stored.TriangleIndex, stored.Barycentric);
    MaterialSample material = EvaluateMaterial(scene, hit);
    float normalLengthSquared = dot(material.ShadingNormalWorld, material.ShadingNormalWorld);
    if (hit.IsValid == false || material.IsValid == false || normalLengthSquared <= 0)
        return false;
    float3 outgoingWorld =
        index == 0 ? path.ViewWorld : normalize(GetPathVertex(path, index - 1).PositionWorld - stored.PositionWorld);
    float3 normalWorld = material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    outVertex.PositionWorld = stored.PositionWorld;
    outVertex.OutgoingWorld = outgoingWorld;
    outVertex.Frame = MakeSurfaceFrame(normalWorld, ChooseTangentHintWorld(normalWorld, hit.TangentWorld));
    outVertex.Material = MakeOpticalMaterial(material);
    outVertex.Profile = MakeGrating();
    outVertex.Lobe = stored.Lobe;
    outVertex.LobeProbability = stored.LobeProbability;
    outVertex.ContinuationWeight = stored.ContinuationWeight;
    return true;
}

float ComputePowerHeuristic(float density, float otherDensity)
{
    float squared = density * density;
    float total = squared + otherDensity * otherDensity;
    return total > 0 ? squared / total : 0;
}

/// Next-event estimation at path vertex index: one light and its shadow ray. Returns false when the connection cannot
/// contribute (a delta lobe, no light, no light sample, an occluded or failed query); otherwise outEndpoint is the
/// light, weighted by the receiving cosine over the light's density and the power heuristic against the lobe's.
bool TryMakeConnection(SceneContext scene,
                       RayLaunch launch,
                       uint index,
                       ShadingVertex vertex,
                       SurfaceHit receiver,
                       float samplingWavelength,
                       OpticsSettings settings,
                       inout RayQueryContext queries,
                       out LightEndpoint outEndpoint)
{
    outEndpoint = (LightEndpoint)0;
    if (IsDeltaLobe(vertex.Lobe) == true)
        return false;
    SelectedLight selected =
        SelectLight(scene, launch, GetShadeVertexDimension(index, SHADE_VERTEX_LIGHT_SELECTION_DIMENSION));
    if (selected.HasSample == false)
        return false;
    uint shapeFirst = GetShadeVertexDimension(index, SHADE_VERTEX_LIGHT_SHAPE_DIMENSION_FIRST);
    float3 shapeSample = float3(SampleSceneRandom(launch, shapeFirst),
                                SampleSceneRandom(launch, shapeFirst + 1),
                                SampleSceneRandom(launch, shapeFirst + 2));
    LightSample light = SampleIncidentLight(scene, selected, receiver, shapeSample);
    float lightDensity = light.SelectionPmf * light.ConditionalPdf;
    float cosine = dot(light.DirectionWorld, vertex.Frame.NormalWorld);
    if (light.HasSample == false || lightDensity <= 0 || cosine <= 0 || any(light.IncidentCoefficient > 0) == false)
        return false;
    outEndpoint.RadianceRgb = light.IncidentCoefficient;
    outEndpoint.PropagationWorld = -light.DirectionWorld;
    outEndpoint.DistanceWorld = light.DistanceWorld;
    outEndpoint.IsDiscrete = light.IsDiscrete;
    outEndpoint.Kind = ENDPOINT_RADIATOR;
    if (light.IsDiscrete == true && light.HasFiniteEndpoint == false)
        outEndpoint.Kind = ENDPOINT_SUN;
    else if (light.IsDiscrete == false && light.TriangleIndex == SCENE_INVALID_INDEX)
        outEndpoint.Kind = ENDPOINT_ENVIRONMENT;
    float weight = cosine / (vertex.LobeProbability * lightDensity);
    if (light.IsDiscrete == false)
        weight *=
            ComputePowerHeuristic(lightDensity,
                                  EvaluateLobeDensity(vertex, light.DirectionWorld, samplingWavelength, settings));
    outEndpoint.Weight = weight;
    // Skip the shadow ray when the connection cannot contribute; an occluded or failed query adds nothing.
    return weight > 0 && TestVisibility(scene, queries, receiver, light) == SCENE_QUERY_MISS;
}

/// The camera path from the recorded hit: emission seen directly, then for every vertex a connection and a
/// continuation, whose light (emission or the environment) is weighted against the connection by the power heuristic.
// Thanks to the Slang authors, core module noinline attribute reference:
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html
// The wave and radiometric passes share one out-of-line path tracer, so the merged ray kernel holds one copy. Slang
// ignores [noinline] on a function with an inout parameter, so the body takes the query context by value and returns
// it through an out parameter behind TracePath, as the salesin-jarosz-lines template does.
[noinline] float3 TracePathOutOfLine(SceneContext scene,
                                     RayLaunch launch,
                                     bool isWave,
                                     RayQueryContext queriesIn,
                                     out RayQueryContext outQueries) {
    RayQueryContext queries = queriesIn;
    float3 radiance = TracePathBody(scene, launch, isWave, queries);
    outQueries = queries;
    return radiance;
}

float3 TracePath(SceneContext scene, RayLaunch launch, bool isWave, inout RayQueryContext queries)
{
    RayQueryContext updated;
    float3 radiance = TracePathOutOfLine(scene, launch, isWave, queries, updated);
    queries = updated;
    return radiance;
}

float3 TracePathBody(SceneContext scene, RayLaunch launch, bool isWave, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 viewWorld;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, viewWorld, radiance) == false)
        return radiance;
    OpticsSettings settings = GetOpticsSettings();
    // Unpolarized light through an ideal linear polarizer keeps half its intensity.
    if (settings.HasPolarizer == true)
        radiance *= 0.5;
    float3 polarizerAxisWorld = ComputePolarizerAxisWorld(launch);
    float wavelengthRandom = SampleSceneRandom(launch, SHADE_WAVELENGTH_DIMENSION);
    float samplingWavelength = SampleWavelengthNanometers(wavelengthRandom, 0);
    uint vertexLimit = min(PO_PATH_VERTICES, PATH_VERTEX_CAPACITY);
    CameraPath path = (CameraPath)0;
    path.ViewWorld = viewWorld;
    SurfaceHit hit = shadingPoint.Hit;
    MaterialSample material = shadingPoint.Material;
    float3 normalWorld = shadingPoint.ShadingNormalWorld;
    float3 outgoingWorld = viewWorld;
    [loop] for (uint index = 0; index < vertexLimit; ++index)
    {
        ShadingVertex vertex;
        if (TryMakeShadingVertex(hit, material, normalWorld, outgoingWorld, vertex) == false)
            break;
        float lobeRandom = SampleSceneRandom(launch, GetShadeVertexDimension(index, SHADE_VERTEX_LOBE_DIMENSION));
        vertex.Lobe =
            SelectLobe(vertex.Material, dot(outgoingWorld, normalWorld), isWave, lobeRandom, vertex.LobeProbability);
        if (vertex.Lobe == LOBE_NONE)
            break;
        PathVertex stored;
        stored.TriangleIndex = hit.TriangleIndex;
        stored.Barycentric = hit.Barycentric;
        stored.PositionWorld = hit.PositionWorld;
        stored.Lobe = vertex.Lobe;
        stored.LobeProbability = vertex.LobeProbability;
        stored.ContinuationWeight = 0;
        SetPathVertex(path, index, stored);
        LightEndpoint connection;
        bool hasConnection =
            TryMakeConnection(scene, launch, index, vertex, hit, samplingWavelength, settings, queries, connection);

        // The continuation: a direction from the lobe, one surface query, and the light it finds.
        LightEndpoint continuation = (LightEndpoint)0;
        bool hasContinuation = false;
        bool canContinue = false;
        SurfaceHit next = (SurfaceHit)0;
        float3 incidentWorld = float3(0);
        uint directionFirst = GetShadeVertexDimension(index, SHADE_VERTEX_DIRECTION_DIMENSION_FIRST);
        // The grating's order is drawn with the lobe selection's number, unused there because a grating has one lobe.
        float3 directionRandom = float3(SampleSceneRandom(launch, directionFirst),
                                        SampleSceneRandom(launch, directionFirst + 1),
                                        lobeRandom);
        bool isDelta = IsDeltaLobe(vertex.Lobe);
        float lobeDensity = 0;
        if (SampleLobeDirection(vertex, samplingWavelength, settings, directionRandom, incidentWorld) == true &&
            dot(incidentWorld, hit.GeometricNormalWorld) > 0)
        {
            if (isDelta == false)
                lobeDensity = EvaluateLobeDensity(vertex, incidentWorld, samplingWavelength, settings);
            if (isDelta == true || lobeDensity > 0)
            {
                stored.ContinuationWeight = isDelta == true ? 1 / vertex.LobeProbability
                                                            : dot(incidentWorld, vertex.Frame.NormalWorld) /
                                                                  (vertex.LobeProbability * lobeDensity);
                SetPathVertex(path, index, stored);
                uint status = TraceClosest(scene,
                                           queries,
                                           SpawnSurfaceRay(hit, incidentWorld, SHADE_MAXIMUM_TRACE_DISTANCE_WORLD),
                                           next);
                if (status != SCENE_QUERY_FAILURE)
                {
                    continuation.PropagationWorld = -incidentWorld;
                    continuation.Weight = stored.ContinuationWeight;
                    float lightDensity = 0;
                    if (status == SCENE_QUERY_MISS)
                    {
                        continuation.Kind = ENDPOINT_ENVIRONMENT;
                        continuation.RadianceRgb = EvaluateEnvironment(scene, incidentWorld);
                        lightDensity = EvaluateEnvironmentPdfSolidAngle(scene, incidentWorld);
                    }
                    else
                    {
                        continuation.Kind = ENDPOINT_RADIATOR;
                        continuation.RadianceRgb = EvaluateEmission(scene, next, -incidentWorld);
                        continuation.DistanceWorld = next.DistanceWorld;
                        lightDensity = EvaluateLightPdfSolidAngle(scene, hit.PositionWorld, next);
                    }
                    if (isDelta == false)
                        continuation.Weight *= ComputePowerHeuristic(lobeDensity, lightDensity);
                    hasContinuation = any(continuation.RadianceRgb > 0);
                    canContinue = status == SCENE_QUERY_SURFACE && index + 1 < vertexLimit;
                }
            }
        }

        // Both lights through one evaluation, so the kernel inlines the beam transport once.
        float windowOffset = SampleSceneRandom(launch, GetShadeVertexDimension(index, SHADE_VERTEX_WINDOW_DIMENSION));
        [loop] for (uint endpointIndex = 0; endpointIndex < 2; ++endpointIndex)
        {
            bool isConnection = endpointIndex == 0;
            if ((isConnection == true ? hasConnection : hasContinuation) == false)
                continue;
            radiance += EstimateEndpoint(scene,
                                         path,
                                         index,
                                         vertex,
                                         isConnection == true ? connection : continuation,
                                         wavelengthRandom,
                                         windowOffset,
                                         settings,
                                         isWave,
                                         polarizerAxisWorld);
        }
        if (canContinue == false)
            break;
        material = EvaluateMaterial(scene, next);
        float normalLengthSquared = dot(material.ShadingNormalWorld, material.ShadingNormalWorld);
        if (material.IsValid == false || normalLengthSquared <= 0)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
            break;
        }
        normalWorld = material.ShadingNormalWorld * rsqrt(normalLengthSquared);
        hit = next;
        outgoingWorld = -incidentWorld;
    }
    return radiance;
}

/// Binary32 exponent bits; all set for an infinity or a NaN.
static const uint FLOAT_EXPONENT_MASK = 0x7f800000u;

/// Reject a nonfinite or negative estimate as a shader failure. Metal compiles with fast math, which may fold
/// isfinite away, so finiteness is tested on the bit pattern.
float3 FinishRadiance(float3 radiance, inout RayQueryContext queries)
{
    uint3 exponents = asuint(radiance) & FLOAT_EXPONENT_MASK;
    if (any(exponents == FLOAT_EXPONENT_MASK) == true || any(radiance < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}

/// Wave optics: paths evaluated with beams, coherence and polarization.
float3 ShadeWave(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return FinishRadiance(TracePath(scene, launch, true, queries), queries);
}

/// The same paths evaluated radiometrically.
float3 ShadeRadiometric(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return FinishRadiance(TracePath(scene, launch, false, queries), queries);
}

/// Characteristic transverse coherence length of the light that the first vertex's connection receives, per
/// PO_COHERENCE_DISPLAY_MICROMETERS.
float3 MeasureCoherence(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 viewWorld;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, viewWorld, radiance) == false)
        return float3(0);
    SelectedLight selected =
        SelectLight(scene, launch, GetShadeVertexDimension(0, SHADE_VERTEX_LIGHT_SELECTION_DIMENSION));
    if (selected.HasSample == false)
        return float3(0);
    uint shapeFirst = GetShadeVertexDimension(0, SHADE_VERTEX_LIGHT_SHAPE_DIMENSION_FIRST);
    float3 shapeSample = float3(SampleSceneRandom(launch, shapeFirst),
                                SampleSceneRandom(launch, shapeFirst + 1),
                                SampleSceneRandom(launch, shapeFirst + 2));
    LightSample light = SampleIncidentLight(scene, selected, shadingPoint.Hit, shapeSample);
    if (light.HasSample == false || any(light.IncidentCoefficient > 0) == false ||
        dot(shadingPoint.ShadingNormalWorld, light.DirectionWorld) <= 0 ||
        TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    LightEndpoint endpoint = (LightEndpoint)0;
    endpoint.RadianceRgb = float3(1);
    endpoint.PropagationWorld = -light.DirectionWorld;
    endpoint.DistanceWorld = light.DistanceWorld;
    endpoint.Kind = ENDPOINT_RADIATOR;
    if (light.IsDiscrete == true && light.HasFiniteEndpoint == false)
        endpoint.Kind = ENDPOINT_SUN;
    else if (light.IsDiscrete == false && light.TriangleIndex == SCENE_INVALID_INDEX)
        endpoint.Kind = ENDPOINT_ENVIRONMENT;
    Beam beam = MakeLightBeam(endpoint, PO_COHERENCE_WAVELENGTH_NANOMETERS, GetOpticsSettings());
    float lengthMicrometers =
        ComputeCoherenceLengthWorld(beam, PO_COHERENCE_WAVELENGTH_NANOMETERS) * MICROMETERS_PER_WORLD_UNIT;
    return FinishRadiance(float3(lengthMicrometers / PO_COHERENCE_DISPLAY_MICROMETERS), queries);
}
