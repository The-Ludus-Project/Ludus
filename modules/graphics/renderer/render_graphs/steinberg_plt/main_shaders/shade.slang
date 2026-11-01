#include "grating.slang"
#include "hit_record.slang"
#include "natural_light.slang"

// Passes 2 to 4 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting under
// Shlomi Steinberg and Ling-Qi Yan, "A Generic Framework for Physical Light Transport", ACM Transactions on
// Graphics 40(4), article 139, SIGGRAPH 2021, https://doi.org/10.1145/3450626.3459791 .
// As in the paper's renderings (section 8.4), only diffracting surfaces need its formalism: smooth metals
// (IsGratingMaterial) are sinusoidal gratings, and every other surface is shaded classically with one light
// sample, the same in every pass. Light from distinct sources is mutually incoherent, so their contributions add
// (assumption (i) of section 9).
// - ShadeWave (the color output) evaluates Eq. (42) for the grating, spectrally. Directional, point and spot
//   lights are natural spherical sources (Definition 5.1) of PLT_DIRECTIONAL_SOURCE_ANGULAR_RADIUS or
//   PLT_POINT_SOURCE_RADIUS_MILLIMETERS; every diffraction order adds its efficiency times the source's angular
//   profile, integrated over the wavelengths at which the order sees the source. Mesh emitters and the environment
//   arrive through one traced ray along a sampled order's incident direction, which sees their actual shape: this is
//   the same convolution for an extended source of any shape (the van Cittert-Zernike theorem).
// - ShadeGeometricOptics (the geometric output) shades the same gratings in the short-wavelength limit
//   (Appendix B): mirror facets of the same sinusoid, with the same sources, wavelengths and Fresnel factors.
//   Its gratings show no dispersion, so comparing both images isolates diffraction, as the paper's Fig. 8 does.
// - MeasureCoherence (the coherence output) writes the spatial coherence radius of Definition 5.1 at the surface
//   for the selected light, divided by PLT_COHERENCE_DISPLAY_MICROMETERS; zero where that light is occluded.
//   Accumulation averages it over light selection. A mesh emitter's natural source is the bounding sphere of its
//   triangles; the environment counts as fully incoherent (zero).
// Departures: delta lights stay points for shadow rays and for classical surfaces; the camera traces gathers
// only, so light that reaches a grating through other surfaces is not diffracted, which the paper's
// bidirectional tracer handles (section 8.4); polarization is ignored (grating.slang).
// At most one surface query (the traced estimate) and one visibility query per invocation.

static const float SHADE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
    float3 ViewDirectionWorld;
};

/// A grating surface point with its groove frame and profile.
struct GratingPoint
{
    ShadingPoint Surface;
    GratingFrame Frame;
    GratingProfile Profile;
};

/// A directional, point or spot light shaded as a natural spherical source.
struct NaturalDeltaSource
{
    LightSample Light;
    float AngularRadius;
    bool HasSource;
};

/// Rebuild this pixel's recorded surface. Returns false when the pixel needs no estimate: a primary miss
/// (outRadiance is the environment) or a failure (queries.HasFailed is set, outRadiance is 0).
bool TryLoadShadingPoint(SceneContext scene,
                         RayLaunch launch,
                         inout RayQueryContext queries,
                         out ShadingPoint outPoint,
                         out float3 outRadiance)
{
    outPoint = (ShadingPoint)0;
    outRadiance = float3(0);
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries, HasHitRecordCapacity(launch) == false ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY : LUDUS_SCENE_FAILURE_STORED_RECORD);
        return false;
    }
    // The primary pass traced this exact ray: same launch, camera and stored jitter.
    Ray cameraRay = MakeCameraRay(launch, record.JitterPixel);
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
    outPoint.ViewDirectionWorld = -cameraRay.DirectionWorld;
    outPoint.Material = EvaluateMaterial(scene, outPoint.Hit);
    float normalLengthSquared = dot(outPoint.Material.ShadingNormalWorld, outPoint.Material.ShadingNormalWorld);
    if (outPoint.Material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outPoint.ShadingNormalWorld = outPoint.Material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    outRadiance = EvaluateEmission(scene, outPoint.Hit, outPoint.ViewDirectionWorld);
    return true;
}

float3 LoadLightShapeSample(RayLaunch launch)
{
    return float3(SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST),
                  SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 1),
                  SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 2));
}

/// Classical surfaces: one light sample through the standard BSDF and its shadow ray.
float3
SampleClassicalLight(SceneContext scene, RayLaunch launch, ShadingPoint shadingPoint, inout RayQueryContext queries)
{
    SelectedLight selected = SelectLight(scene, launch, SHADE_LIGHT_SELECTION_DIMENSION);
    if (selected.HasSample == false)
        return float3(0);
    LightSample light = SampleIncidentLight(scene, selected, shadingPoint.Hit, LoadLightShapeSample(launch));
    float lightDensity = light.SelectionPmf * light.ConditionalPdf;
    if (light.HasSample == false || lightDensity <= 0 || any(light.IncidentCoefficient > 0) == false)
        return float3(0);
    float cosSurface = dot(shadingPoint.ShadingNormalWorld, light.DirectionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(shadingPoint.Material,
                         shadingPoint.ViewDirectionWorld,
                         light.DirectionWorld,
                         bsdfValue,
                         bsdfDensity);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (cosSurface <= 0 || any(bsdfValue > 0) == false ||
        TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface / lightDensity);
}

/// The selected light when it is a directional, point or spot light. Mesh emitters and the environment reach a
/// grating through the traced estimate instead, so they return HasSource = false here.
NaturalDeltaSource SelectNaturalDeltaSource(SceneContext scene, RayLaunch launch, SurfaceHit receiver)
{
    NaturalDeltaSource result = (NaturalDeltaSource)0;
    SelectedLight selected = SelectLight(scene, launch, SHADE_LIGHT_SELECTION_DIMENSION);
    if (selected.HasSample == false)
        return result;
    result.Light = SampleIncidentLight(scene, selected, receiver, LoadLightShapeSample(launch));
    if (result.Light.HasSample == false || result.Light.IsDiscrete == false || result.Light.SelectionPmf <= 0 ||
        any(result.Light.IncidentCoefficient > 0) == false)
        return result;
    result.AngularRadius =
        result.Light.HasFiniteEndpoint == true
            ? ComputeNaturalSourceAngularRadius(PLT_POINT_SOURCE_RADIUS_MILLIMETERS * WORLD_UNITS_PER_MILLIMETER,
                                                result.Light.DistanceWorld)
            : min(PLT_DIRECTIONAL_SOURCE_ANGULAR_RADIUS, NATURAL_SOURCE_MAXIMUM_ANGULAR_RADIUS);
    result.HasSource = result.AngularRadius > 0;
    return result;
}

/// Trace from a grating point along an incident direction: the emission of the surface it reaches, or the
/// environment on a miss. A failed query latches queries.HasFailed and returns zero.
float3
TraceIncidentRadiance(SceneContext scene, SurfaceHit receiver, float3 incidentWorld, inout RayQueryContext queries)
{
    SurfaceHit endpoint;
    uint status = TraceClosest(scene,
                               queries,
                               SpawnSurfaceRay(receiver, incidentWorld, SHADE_MAXIMUM_TRACE_DISTANCE_WORLD),
                               endpoint);
    if (status == SCENE_QUERY_MISS)
        return EvaluateEnvironment(scene, incidentWorld);
    if (status == SCENE_QUERY_SURFACE)
        return EvaluateEmission(scene, endpoint, -incidentWorld);
    return float3(0);
}

/// Linear RGB estimate of a reflection whose direction does not depend on the wavelength, at the
/// SPECTRUM_HERO_WAVELENGTH_COUNT hero wavelengths rotated by wavelengthRandom: the uplifted reflectance times the
/// uplifted incident radiance, read by the sensor and averaged.
float3 ConvertHeroReflection(float3 reflectance, float3 incidentRadiance, float wavelengthRandom)
{
    float3 radiance = float3(0);
    for (uint hero = 0; hero < SPECTRUM_HERO_WAVELENGTH_COUNT; ++hero)
    {
        float wavelengthNanometers = SampleWavelengthNanometers(wavelengthRandom, hero);
        float spectral = EvaluateUpliftedSpectrum(reflectance, wavelengthNanometers) *
                         EvaluateUpliftedSpectrum(incidentRadiance, wavelengthNanometers);
        radiance += ConvertSpectralRadiance(spectral, wavelengthNanometers);
    }
    return radiance / float(SPECTRUM_HERO_WAVELENGTH_COUNT);
}

/// Divide a delta source's estimate by its selection probability once its centre is visible.
float3 FinishDeltaSourceEstimate(SceneContext scene,
                                 SurfaceHit receiver,
                                 NaturalDeltaSource source,
                                 float3 radiance,
                                 inout RayQueryContext queries)
{
    // Skip the shadow ray for a zero estimate; an occluded or failed query adds nothing.
    if (any(radiance > 0) == false || TestVisibility(scene, queries, receiver, source.Light) != SCENE_QUERY_MISS)
        return float3(0);
    return radiance / source.Light.SelectionPmf;
}

/// Spectral radiance at one wavelength that a natural delta source sends to the viewer through one order:
/// J_m(A_m)^2 times the Fresnel factor times the source's angular profile at the order's incident direction
/// (one peak of Eq. (42)); zero for an evanescent order or a direction off the source's disk.
float EvaluateWaveOrderRadiance(GratingPoint grating, NaturalDeltaSource source, int order, float wavelengthNanometers)
{
    float sourceSpectrum = EvaluateUpliftedSpectrum(source.Light.IncidentCoefficient, wavelengthNanometers);
    if (sourceSpectrum <= 0)
        return 0;
    GratingFrame frame = grating.Frame;
    float3 outgoingWorld = grating.Surface.ViewDirectionWorld;
    float3 incidentWorld;
    if (TryGetOrderIncidentDirection(frame,
                                     outgoingWorld,
                                     order,
                                     wavelengthNanometers / grating.Profile.PeriodNanometers,
                                     incidentWorld) == false)
        return 0;
    float profile = EvaluateNaturalSourceProfile(ComputeAngleBetween(incidentWorld, source.Light.DirectionWorld),
                                                 source.AngularRadius);
    if (profile <= 0)
        return 0;
    float wavenumberAmplitude = 2 * PI * grating.Profile.AmplitudeNanometers / wavelengthNanometers;
    float efficiency = EvaluateOrderEfficiency(frame, incidentWorld, outgoingWorld, order, wavenumberAmplitude);
    float3 fresnel =
        EvaluateGratingFresnel(grating.Surface.Material, dot(outgoingWorld, normalize(incidentWorld + outgoingWorld)));
    return efficiency * EvaluateUpliftedSpectrum(fresnel, wavelengthNanometers) * profile * sourceSpectrum;
}

/// Linear RGB radiance that a natural delta source sends to the viewer through every order (Eq. (42)). An order
/// sees the source's disk only within a window of wavelengths, where the grating equation maps its incident
/// direction onto the disk: about a nanometre for a millimetre source. The spectral integral over each window uses
/// SPECTRUM_HERO_WAVELENGTH_COUNT stratified wavelengths offset by wavelengthOffset in [0, 1), which is unbiased;
/// drawing wavelengths over the whole range instead would find a small source's window in few samples.
float3 IntegrateWaveSourceRadiance(GratingPoint grating, NaturalDeltaSource source, float wavelengthOffset)
{
    GratingFrame frame = grating.Frame;
    // Order m's incident direction has parallel component -w_o,par + m (wavelength / period) g. It is within the
    // angular radius of the source centre only if its parallel component is within that radius of the centre's:
    // projection and chords both shorten angles. So m wavelength / period must lie in [along - width,
    // along + width] around the offset between the centre's and the mirror direction's parallel components.
    float3 offsetWorld =
        ProjectOntoPlane(source.Light.DirectionWorld + grating.Surface.ViewDirectionWorld, frame.NormalWorld);
    float across = dot(offsetWorld, frame.GrooveWorld);
    float reach = source.AngularRadius;
    if (across * across >= reach * reach)
        return float3(0);
    float halfWidth = sqrt(reach * reach - across * across);
    float along = dot(offsetWorld, frame.GratingVectorWorld);
    float lowestProduct = (along - halfWidth) * grating.Profile.PeriodNanometers;
    float highestProduct = (along + halfWidth) * grating.Profile.PeriodNanometers;
    int firstOrder = int(ceil(min(lowestProduct / SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS,
                                  lowestProduct / SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS)));
    int lastOrder = int(floor(max(highestProduct / SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS,
                                  highestProduct / SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS)));
    lastOrder = min(lastOrder, firstOrder + GRATING_MAXIMUM_ORDER_SPAN - 1);
    float3 radiance = float3(0);
    for (int order = firstOrder; order <= lastOrder; ++order)
    {
        float windowMinimum = SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS;
        float windowMaximum = SPECTRUM_WAVELENGTH_MAXIMUM_NANOMETERS;
        if (order == 0)
        {
            // The mirror order's direction does not depend on the wavelength: all or nothing.
            if (lowestProduct > 0 || highestProduct < 0)
                continue;
        }
        else
        {
            float firstBound = lowestProduct / float(order);
            float secondBound = highestProduct / float(order);
            windowMinimum = max(min(firstBound, secondBound), windowMinimum);
            windowMaximum = min(max(firstBound, secondBound), windowMaximum);
        }
        if (windowMinimum >= windowMaximum)
            continue;
        float windowStep = (windowMaximum - windowMinimum) / float(SPECTRUM_HERO_WAVELENGTH_COUNT);
        for (uint stratum = 0; stratum < SPECTRUM_HERO_WAVELENGTH_COUNT; ++stratum)
        {
            float wavelengthNanometers = windowMinimum + (float(stratum) + wavelengthOffset) * windowStep;
            radiance += EvaluateWaveOrderRadiance(grating, source, order, wavelengthNanometers) *
                        EvaluateSensorSensitivity(wavelengthNanometers) * windowStep;
        }
    }
    return radiance;
}

/// Wave optics, delta sources: every order over its wavelength window, then one shadow ray.
float3
EstimateWaveDeltaSource(SceneContext scene, RayLaunch launch, GratingPoint grating, inout RayQueryContext queries)
{
    NaturalDeltaSource source = SelectNaturalDeltaSource(scene, launch, grating.Surface.Hit);
    if (source.HasSource == false)
        return float3(0);
    float3 radiance =
        IntegrateWaveSourceRadiance(grating, source, SampleSceneRandom(launch, SHADE_WAVELENGTH_DIMENSION));
    return FinishDeltaSourceEstimate(scene, grating.Surface.Hit, source, radiance, queries);
}

/// Wave optics, extended sources: one order drawn from the proposal (grating.slang) at the first hero wavelength,
/// and one ray along its incident direction. Other orders' directions depend on the wavelength, so only a flat
/// grating, whose single mirror order does not, shares its ray among the hero wavelengths.
float3 EstimateWaveTraced(SceneContext scene, RayLaunch launch, GratingPoint grating, inout RayQueryContext queries)
{
    GratingFrame frame = grating.Frame;
    float3 outgoingWorld = grating.Surface.ViewDirectionWorld;
    float wavelengthRandom = SampleSceneRandom(launch, SHADE_WAVELENGTH_DIMENSION);
    if (grating.Profile.AmplitudeNanometers <= 0)
    {
        // J_0(0) = 1: the mirror order carries all the light. Any wavelength gives its direction.
        float3 mirrorWorld;
        if (TryGetOrderIncidentDirection(frame, outgoingWorld, 0, 0, mirrorWorld) == false)
            return float3(0);
        float3 reflectance = EvaluateGratingFresnel(grating.Surface.Material,
                                                    dot(outgoingWorld, normalize(mirrorWorld + outgoingWorld)));
        float3 incidentRadiance = TraceIncidentRadiance(scene, grating.Surface.Hit, mirrorWorld, queries);
        return ConvertHeroReflection(reflectance, incidentRadiance, wavelengthRandom);
    }
    float wavelengthNanometers = SampleWavelengthNanometers(wavelengthRandom, 0);
    float wavelengthOverPeriod = wavelengthNanometers / grating.Profile.PeriodNanometers;
    int firstOrder;
    int lastOrder;
    if (TryGetPropagatingOrders(frame, outgoingWorld, wavelengthOverPeriod, firstOrder, lastOrder) == false)
        return float3(0);
    float wavenumberAmplitude = 2 * PI * grating.Profile.AmplitudeNanometers / wavelengthNanometers;
    OrderProposal proposal =
        MakeOrderProposal(firstOrder, lastOrder, 2 * wavenumberAmplitude * dot(outgoingWorld, frame.NormalWorld));
    int order = SampleOrder(proposal, SampleSceneRandom(launch, SHADE_TRACED_DIRECTION_DIMENSION));
    float probability = EvaluateOrderProbability(proposal, order);
    float3 incidentWorld;
    if (probability <= 0 ||
        TryGetOrderIncidentDirection(frame, outgoingWorld, order, wavelengthOverPeriod, incidentWorld) == false)
        return float3(0);
    float efficiency = EvaluateOrderEfficiency(frame, incidentWorld, outgoingWorld, order, wavenumberAmplitude);
    float3 fresnel =
        EvaluateGratingFresnel(grating.Surface.Material, dot(outgoingWorld, normalize(incidentWorld + outgoingWorld)));
    float reflection = efficiency / probability * EvaluateUpliftedSpectrum(fresnel, wavelengthNanometers);
    if (reflection <= 0)
        return float3(0);
    float3 incidentRadiance = TraceIncidentRadiance(scene, grating.Surface.Hit, incidentWorld, queries);
    return ConvertSpectralRadiance(reflection * EvaluateUpliftedSpectrum(incidentRadiance, wavelengthNanometers),
                                   wavelengthNanometers);
}

/// Linear RGB fraction of a natural delta source's integrated radiance that the facet ensemble reflects toward the
/// viewer: the integral over the slope density of Fresnel times facet weight times the source profile. Facets turn
/// about the grooves, so the reflected direction sweeps a circle; only a window of slopes around the facet that
/// best reflects the viewer onto the source centre can contribute. The window is integrated with
/// GRATING_FACET_QUADRATURE_POINTS stratified points offset by quadratureOffset in [0, 1), which is unbiased.
float3 IntegrateFacetSourceReflection(GratingPoint grating, NaturalDeltaSource source, float quadratureOffset)
{
    GratingFrame frame = grating.Frame;
    float3 outgoingWorld = grating.Surface.ViewDirectionWorld;
    float3 sourceWorld = source.Light.DirectionWorld;
    float angularRadius = source.AngularRadius;
    float maximumSlope = ComputeMaximumFacetSlope(grating.Profile);
    if (maximumSlope <= 0)
    {
        float3 mirrorWorld = ReflectAbout(outgoingWorld, frame.NormalWorld);
        return EvaluateGratingFresnel(grating.Surface.Material, dot(outgoingWorld, frame.NormalWorld)) *
               EvaluateNaturalSourceProfile(ComputeAngleBetween(mirrorWorld, sourceWorld), angularRadius);
    }
    // Rotating a facet by an angle about the grooves turns the reflected direction by
    // 2 sqrt(1 - (w_o . groove)^2) times that angle.
    float grooveCosine = dot(outgoingWorld, frame.GrooveWorld);
    float speed = 2 * sqrt(max(1 - grooveCosine * grooveCosine, 0));
    float steepestAngle = atan(maximumSlope);
    float firstAngle = -steepestAngle;
    float lastAngle = steepestAngle;
    if (speed > GRATING_MINIMUM_REFLECTION_SPEED)
    {
        float3 halfWorld = outgoingWorld + sourceWorld;
        float centerAngle = atan2(-dot(halfWorld, frame.GratingVectorWorld), dot(halfWorld, frame.NormalWorld));
        float halfWidth = GRATING_FACET_WINDOW_SCALE * angularRadius / speed;
        firstAngle = max(firstAngle, centerAngle - halfWidth);
        lastAngle = min(lastAngle, centerAngle + halfWidth);
    }
    if (firstAngle >= lastAngle)
        return float3(0);
    // Slopes s = S sin(theta) with theta uniform in [-pi/2, pi/2]: the slope density is dtheta / pi.
    float firstPhase = asin(clamp(tan(firstAngle) / maximumSlope, -1.0, 1.0));
    float lastPhase = asin(clamp(tan(lastAngle) / maximumSlope, -1.0, 1.0));
    float phaseStep = (lastPhase - firstPhase) / float(GRATING_FACET_QUADRATURE_POINTS);
    float3 sum = float3(0);
    for (uint quadraturePoint = 0; quadraturePoint < GRATING_FACET_QUADRATURE_POINTS; ++quadraturePoint)
    {
        float slope = maximumSlope * sin(firstPhase + (float(quadraturePoint) + quadratureOffset) * phaseStep);
        float3 facetWorld = MakeFacetNormal(frame, slope);
        float3 incidentWorld = ReflectAbout(outgoingWorld, facetWorld);
        if (dot(incidentWorld, frame.NormalWorld) <= 0)
            continue;
        float profile = EvaluateNaturalSourceProfile(ComputeAngleBetween(incidentWorld, sourceWorld), angularRadius);
        if (profile <= 0)
            continue;
        sum += EvaluateGratingFresnel(grating.Surface.Material, dot(outgoingWorld, facetWorld)) *
               (ComputeFacetWeight(frame, facetWorld, outgoingWorld) * profile);
    }
    return sum * (phaseStep / PI);
}

/// Geometric optics, delta sources: the facet integral, the same hero wavelengths as the wave pass, one shadow ray.
float3
EstimateGeometricDeltaSource(SceneContext scene, RayLaunch launch, GratingPoint grating, inout RayQueryContext queries)
{
    NaturalDeltaSource source = SelectNaturalDeltaSource(scene, launch, grating.Surface.Hit);
    if (source.HasSource == false)
        return float3(0);
    float3 reflection =
        IntegrateFacetSourceReflection(grating, source, SampleSceneRandom(launch, SHADE_FACET_QUADRATURE_DIMENSION));
    if (any(reflection > 0) == false)
        return float3(0);
    float3 radiance = ConvertHeroReflection(reflection,
                                            source.Light.IncidentCoefficient,
                                            SampleSceneRandom(launch, SHADE_WAVELENGTH_DIMENSION));
    return FinishDeltaSourceEstimate(scene, grating.Surface.Hit, source, radiance, queries);
}

/// Geometric optics, extended sources: one facet slope from the slope density and one ray along its reflection,
/// which serves every hero wavelength.
float3
EstimateGeometricTraced(SceneContext scene, RayLaunch launch, GratingPoint grating, inout RayQueryContext queries)
{
    GratingFrame frame = grating.Frame;
    float3 outgoingWorld = grating.Surface.ViewDirectionWorld;
    float phase = PI * (SampleSceneRandom(launch, SHADE_TRACED_DIRECTION_DIMENSION) - 0.5);
    float3 facetWorld = MakeFacetNormal(frame, ComputeMaximumFacetSlope(grating.Profile) * sin(phase));
    float3 incidentWorld = ReflectAbout(outgoingWorld, facetWorld);
    float weight = ComputeFacetWeight(frame, facetWorld, outgoingWorld);
    if (dot(incidentWorld, frame.NormalWorld) <= 0 || weight <= 0)
        return float3(0);
    float3 reflectance = EvaluateGratingFresnel(grating.Surface.Material, dot(outgoingWorld, facetWorld)) * weight;
    if (any(reflectance > 0) == false)
        return float3(0);
    float3 incidentRadiance = TraceIncidentRadiance(scene, grating.Surface.Hit, incidentWorld, queries);
    return ConvertHeroReflection(reflectance, incidentRadiance, SampleSceneRandom(launch, SHADE_WAVELENGTH_DIMENSION));
}

/// Build the grating at a smooth metal; returns false for every other material, which is shaded classically.
bool TryMakeGratingPoint(ShadingPoint shadingPoint, out GratingPoint outGrating)
{
    outGrating = (GratingPoint)0;
    if (IsGratingMaterial(shadingPoint.Material) == false)
        return false;
    outGrating.Surface = shadingPoint;
    outGrating.Frame = MakeGratingFrame(shadingPoint.ShadingNormalWorld, shadingPoint.Hit.TangentWorld);
    outGrating.Profile = GetGratingProfile();
    return true;
}

/// Reject a nonfinite or negative estimate as a shader failure.
float3 FinishRadiance(float3 radiance, inout RayQueryContext queries)
{
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return radiance;
}

/// Wave optics: emission plus the grating's diffraction of delta and extended sources, or a classical light sample.
float3 ShadeWave(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    GratingPoint grating;
    if (TryMakeGratingPoint(shadingPoint, grating) == false)
        return FinishRadiance(radiance + SampleClassicalLight(scene, launch, shadingPoint, queries), queries);
    // A grating seen from behind its mean plane reflects nothing toward the viewer.
    if (dot(shadingPoint.ViewDirectionWorld, grating.Frame.NormalWorld) <= 0)
        return FinishRadiance(radiance, queries);
    radiance += EstimateWaveDeltaSource(scene, launch, grating, queries);
    radiance += EstimateWaveTraced(scene, launch, grating, queries);
    return FinishRadiance(radiance, queries);
}

/// Geometric optics: the same, with the grating's mirror facets instead of its diffraction orders.
float3 ShadeGeometricOptics(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    GratingPoint grating;
    if (TryMakeGratingPoint(shadingPoint, grating) == false)
        return FinishRadiance(radiance + SampleClassicalLight(scene, launch, shadingPoint, queries), queries);
    if (dot(shadingPoint.ViewDirectionWorld, grating.Frame.NormalWorld) <= 0)
        return FinishRadiance(radiance, queries);
    radiance += EstimateGeometricDeltaSource(scene, launch, grating, queries);
    radiance += EstimateGeometricTraced(scene, launch, grating, queries);
    return FinishRadiance(radiance, queries);
}

/// Coherence radius of the selected light's natural source at the surface, per PLT_COHERENCE_DISPLAY_MICROMETERS.
float3 MeasureCoherence(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return float3(0);
    SelectedLight selected = SelectLight(scene, launch, SHADE_LIGHT_SELECTION_DIMENSION);
    if (selected.HasSample == false)
        return float3(0);
    LightSample light = SampleIncidentLight(scene, selected, shadingPoint.Hit, LoadLightShapeSample(launch));
    if (light.HasSample == false || any(light.IncidentCoefficient > 0) == false ||
        dot(shadingPoint.ShadingNormalWorld, light.DirectionWorld) <= 0)
        return float3(0);
    float angularRadius = 0;
    if (light.IsDiscrete == true)
    {
        angularRadius =
            light.HasFiniteEndpoint == true
                ? ComputeNaturalSourceAngularRadius(PLT_POINT_SOURCE_RADIUS_MILLIMETERS * WORLD_UNITS_PER_MILLIMETER,
                                                    light.DistanceWorld)
                : min(PLT_DIRECTIONAL_SOURCE_ANGULAR_RADIUS, NATURAL_SOURCE_MAXIMUM_ANGULAR_RADIUS);
    }
    else if (light.TriangleIndex != SCENE_INVALID_INDEX)
    {
        EmitterBounds bounds =
            GetEmitterBounds(scene, GetSurfaceHit(scene, light.TriangleIndex, light.Barycentric).EmitterIndex);
        if (bounds.IsValid == true)
            angularRadius =
                ComputeNaturalSourceAngularRadius(bounds.RadiusWorld,
                                                  length(bounds.CenterWorld - shadingPoint.Hit.PositionWorld));
    }
    // The environment, and a source without extent, have no finite coherence radius to report.
    if (angularRadius <= 0 || TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    float radiusMicrometers =
        ComputeCoherenceRadiusWorld(angularRadius, PLT_COHERENCE_WAVELENGTH_NANOMETERS) * MICROMETERS_PER_WORLD_UNIT;
    return FinishRadiance(float3(radiusMicrometers / PLT_COHERENCE_DISPLAY_MICROMETERS), queries);
}
