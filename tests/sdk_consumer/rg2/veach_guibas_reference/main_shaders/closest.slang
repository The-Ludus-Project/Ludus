// Emission plus one light sample and one BSDF sample at the traced hit, combined with multiple
// importance sampling, or either strategy alone. An independent single-pass statement of the Veach
// and Guibas MIS template's three shade passes; it does not include the template's sources.
// Thanks to Eric Veach and Leonidas J. Guibas, "Optimally Combining Sampling Techniques for Monte
// Carlo Rendering", SIGGRAPH 1995, pp. 419-428, https://doi.org/10.1145/218380.218498 , for the
// multi-sample estimator and its balance, cutoff, power and maximum heuristics.
// REFERENCE_STRATEGY selects the estimator: MIS of both strategies, or the light or BSDF strategy
// alone with weight one. Every variant draws all seven shading coordinates, so each sees the random
// values that the matching template pass uses.

static const float REFERENCE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;
static const uint REFERENCE_STRATEGY_MIS = 0;
static const uint REFERENCE_STRATEGY_LIGHT = 1;
static const uint REFERENCE_STRATEGY_BSDF = 2;

/// MIS weight of the strategy that drew a sample (MIS_HEURISTIC: 0 balance, 1 power, 2 cutoff, 3 maximum).
/// A strategy used alone has weight one.
float WeighStrategy(float sampledDensity, float otherDensity)
{
    if (REFERENCE_STRATEGY != REFERENCE_STRATEGY_MIS)
        return 1;
    if (isfinite(sampledDensity) == false || sampledDensity <= 0)
        return 0;
    if (otherDensity <= 0)
        return 1;
    if (isfinite(otherDensity) == false)
        return 0;
    float largerDensity = max(sampledDensity, otherDensity);
    float sampled = sampledDensity / largerDensity;
    float other = otherDensity / largerDensity;
    if (MIS_HEURISTIC == 1)
    {
        sampled = pow(sampled, MIS_POWER_EXPONENT);
        other = pow(other, MIS_POWER_EXPONENT);
    }
    else if (MIS_HEURISTIC == 2)
    {
        if (sampled < MIS_CUTOFF_THRESHOLD)
            return 0;
        if (other < MIS_CUTOFF_THRESHOLD)
            return 1;
    }
    else if (MIS_HEURISTIC == 3)
    {
        if (sampled == other)
            return 0.5;
        return sampled > other ? 1 : 0;
    }
    return sampled / (sampled + other);
}

void ShadeDirectLighting(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    float3 viewDirectionWorld = -hit.IncomingRay.DirectionWorld;
    payload.Radiance = EvaluateEmission(scene, hit, viewDirectionWorld);
    MaterialSample material = EvaluateMaterial(scene, hit);
    if (material.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return;
    }
    // Draw in the template's order: light selection, three light-shape and three BSDF coordinates.
    SelectedLight selected = SelectLight(scene, queries);
    float3 shapeSample;
    shapeSample.x = SampleSceneRandom(queries);
    shapeSample.y = SampleSceneRandom(queries);
    shapeSample.z = SampleSceneRandom(queries);
    float3 directionSample;
    directionSample.x = SampleSceneRandom(queries);
    directionSample.y = SampleSceneRandom(queries);
    directionSample.z = SampleSceneRandom(queries);
    float3 normalWorld = normalize(material.ShadingNormalWorld);

    if (REFERENCE_STRATEGY != REFERENCE_STRATEGY_BSDF)
    {
        LightSample light = SampleIncidentLight(scene, selected, hit, shapeSample);
        float lightDensity = light.SelectionPmf * light.ConditionalPdf;
        if (light.HasSample == true && lightDensity > 0)
        {
            float cosine = dot(normalWorld, light.DirectionWorld);
            float3 reflectance;
            float bsdfDensity;
            EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, reflectance, bsdfDensity);
            float weight = light.IsDiscrete == true ? 1 : WeighStrategy(lightDensity, bsdfDensity);
            if (cosine > 0 && weight > 0 && any(reflectance * light.IncidentCoefficient > 0) == true &&
                TestVisibility(scene, queries, hit, light) == SCENE_QUERY_MISS)
                payload.Radiance += reflectance * light.IncidentCoefficient * (cosine * weight / lightDensity);
        }
    }
    if (REFERENCE_STRATEGY == REFERENCE_STRATEGY_LIGHT)
        return;

    float3 directionWorld = SampleStandardBsdf(material, viewDirectionWorld, directionSample);
    if (all(isfinite(directionWorld)) == false || dot(directionWorld, directionWorld) <= 0)
        return;
    directionWorld = normalize(directionWorld);
    float cosine = dot(normalWorld, directionWorld);
    float3 reflectance;
    float bsdfDensity;
    EvaluateStandardBsdf(material, viewDirectionWorld, directionWorld, reflectance, bsdfDensity);
    if (cosine <= 0 || any(reflectance > 0) == false || isfinite(bsdfDensity) == false || bsdfDensity <= 0)
        return;
    SurfaceHit endpoint;
    Ray ray = SpawnSurfaceRay(hit, directionWorld, REFERENCE_MAXIMUM_TRACE_DISTANCE_WORLD);
    uint status = TraceClosest(scene, queries, ray, endpoint);
    float3 incidentRadiance = float3(0);
    float otherDensity = 0;
    if (status == SCENE_QUERY_MISS)
    {
        incidentRadiance = EvaluateEnvironment(scene, directionWorld);
        otherDensity = EvaluateEnvironmentPdfSolidAngle(scene, directionWorld);
    }
    else if (status == SCENE_QUERY_SURFACE)
    {
        incidentRadiance = EvaluateEmission(scene, endpoint, -directionWorld);
        otherDensity = EvaluateLightPdfSolidAngle(scene, hit.PositionWorld, endpoint);
    }
    float weight = WeighStrategy(bsdfDensity, otherDensity);
    if (any(incidentRadiance > 0) == true)
        payload.Radiance += reflectance * incidentRadiance * (cosine * weight / bsdfDensity);
}
