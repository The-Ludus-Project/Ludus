#pragma once
// Light transport shared by the ReSTIR GI passes: the surface a camera ray sees, direct lighting there with light and
// BSDF sampling, and the radiance a sample point scatters back toward the visible point that generated it.
//
// Thanks to Yaobin Ouyang, Shiqiu Liu, Markus Kettunen, Matt Pharr and Jacopo Pantaleoni, "ReSTIR GI: Path
// Resampling for Real-Time Path Tracing", Computer Graphics Forum 40(8) (High-Performance Graphics 2021), pp. 17-29,
// https://doi.org/10.1111/cgf.14378 . Section 4.1 estimates the outgoing radiance at a sample point "using Monte Carlo
// path tracing using next event estimation (NEE) and multiple importance sampling at each vertex"; n path-traced
// bounces after the sample point give (n + 1)-bounce global illumination (their Figure 5). The sample point's own
// emission is excluded here, because the shade pass already estimates direct lighting at the visible point.
// Thanks to Eric Veach and Leonidas J. Guibas, "Optimally Combining Sampling Techniques for Monte Carlo Rendering",
// SIGGRAPH 1995, pp. 419-428, https://doi.org/10.1145/218380.218498 : light and BSDF sampling are combined with
// their power heuristic (exponent 2), as in the veach-guibas-mis template.

static const float TRANSPORT_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;
static const float MIS_POWER_EXPONENT = 2;
// Rec. 709 luminance, the scalar that the target function of section 4.2 reduces RGB radiance to.
static const float3 LUMINANCE_WEIGHTS = float3(0.2126f, 0.7152f, 0.0722f);

// Random dimensions of one path vertex: light selection, the three SampleIncidentLight shape coordinates,
// then the three SampleStandardBsdf coordinates. Each pass addresses its own scope (SampleGiRandom).
static const uint VERTEX_LIGHT_SELECTION_DIMENSION = 0;
static const uint VERTEX_LIGHT_SHAPE_DIMENSION_FIRST = VERTEX_LIGHT_SELECTION_DIMENSION + 1;
static const uint VERTEX_BSDF_DIMENSION_FIRST = VERTEX_LIGHT_SHAPE_DIMENSION_FIRST + 3;
static const uint VERTEX_DIMENSION_COUNT = VERTEX_BSDF_DIMENSION_FIRST + 3;

/// A surface with its evaluated material, ready for BSDF and light evaluation.
struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld; ///< Normalized material normal, including normal maps.
    float3 ViewDirectionWorld; ///< Unit direction toward whoever looks at Hit.
};

/// One continuation proposed by BSDF sampling: the MIS-weighted emission it reached and where the path goes next.
struct BsdfStep
{
    float3 Radiance;   ///< Weighted emission or environment reached, already multiplied by f cos / pdf.
    float3 Throughput; ///< f cos / pdf of the continuation.
    SurfaceHit Vertex; ///< The surface reached; valid only with HasVertex.
    bool HasVertex;
};

/// One addressed random number of this pixel, frame and pass. Every pass of the sequence is seeded identically, so
/// the scope includes GetGraphPassIndex() and no two passes repeat each other's values.
float SampleGiRandom(RayLaunch launch, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    RandomAddress address = MakeRandomAddress(uint2(pixel, GetGraphPassIndex()), launch.SampleIndex, dimension);
    return SampleRandomFloat01(launch.Seed, address);
}

float ComputeLuminance(float3 value)
{
    return dot(value, LUMINANCE_WEIGHTS);
}

/// Evaluate the material at hit as seen along -viewDirectionWorld. A material without a usable normal latches a
/// scene-data failure and returns false.
bool TryMakeShadingPoint(SceneContext scene,
                         SurfaceHit hit,
                         float3 viewDirectionWorld,
                         inout RayQueryContext queries,
                         out ShadingPoint outPoint)
{
    outPoint = (ShadingPoint)0;
    outPoint.Hit = hit;
    outPoint.ViewDirectionWorld = viewDirectionWorld;
    outPoint.Material = EvaluateMaterial(scene, hit);
    float normalLengthSquared = dot(outPoint.Material.ShadingNormalWorld, outPoint.Material.ShadingNormalWorld);
    if (outPoint.Material.IsValid == false || normalLengthSquared <= 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    outPoint.ShadingNormalWorld = outPoint.Material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    return true;
}

/// Power-heuristic weight of the strategy that drew a sample against the other strategy's density for it. A zero
/// other density (a delta light, or a direction the other strategy never proposes) returns 1; a nonfinite or
/// nonpositive sampled density returns 0.
float ComputePowerHeuristic(float sampledDensity, float otherDensity)
{
    if (isfinite(sampledDensity) == false || sampledDensity <= 0)
        return 0;
    if (otherDensity <= 0)
        return 1;
    if (isfinite(otherDensity) == false)
        return 0;
    // Scale both by the larger density, so the powers neither overflow nor flush both terms to zero.
    float largerDensity = max(sampledDensity, otherDensity);
    float sampled = pow(sampledDensity / largerDensity, MIS_POWER_EXPONENT);
    float other = pow(otherDensity / largerDensity, MIS_POWER_EXPONENT);
    return sampled / (sampled + other);
}

/// Light strategy at one vertex: a uniformly chosen light, a point or direction on it and its shadow ray. With
/// isMisWeighted the estimate is weighted against BSDF sampling of the same direction.
float3 SampleLightStrategy(SceneContext scene,
                           RayLaunch launch,
                           ShadingPoint shadingPoint,
                           uint dimensionFirst,
                           bool isMisWeighted,
                           inout RayQueryContext queries)
{
    uint lightCount = GetLightCount(scene);
    if (lightCount == 0)
        return float3(0);
    SelectedLight selected;
    selected.LightIndex =
        min(uint(SampleGiRandom(launch, dimensionFirst + VERTEX_LIGHT_SELECTION_DIMENSION) * float(lightCount)),
            lightCount - 1);
    selected.SelectionPmf = EvaluateLightSelectionPmf(scene, selected.LightIndex);
    selected.HasSample = true;
    float3 shapeSample = float3(SampleGiRandom(launch, dimensionFirst + VERTEX_LIGHT_SHAPE_DIMENSION_FIRST),
                                SampleGiRandom(launch, dimensionFirst + VERTEX_LIGHT_SHAPE_DIMENSION_FIRST + 1),
                                SampleGiRandom(launch, dimensionFirst + VERTEX_LIGHT_SHAPE_DIMENSION_FIRST + 2));
    LightSample light = SampleIncidentLight(scene, selected, shadingPoint.Hit, shapeSample);
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
    if (cosSurface <= 0 || any(bsdfValue > 0) == false)
        return float3(0);
    float weight =
        isMisWeighted == true && light.IsDiscrete == false ? ComputePowerHeuristic(lightDensity, bsdfDensity) : 1;
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (weight <= 0 || TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface * weight / lightDensity);
}

/// BSDF strategy at one vertex: a BSDF-sampled ray, the emission or environment it reaches weighted against light
/// sampling, and the surface it reaches for a further bounce.
BsdfStep SampleBsdfStrategy(SceneContext scene,
                            RayLaunch launch,
                            ShadingPoint shadingPoint,
                            uint dimensionFirst,
                            inout RayQueryContext queries)
{
    BsdfStep result = (BsdfStep)0;
    float3 directionSample = float3(SampleGiRandom(launch, dimensionFirst + VERTEX_BSDF_DIMENSION_FIRST),
                                    SampleGiRandom(launch, dimensionFirst + VERTEX_BSDF_DIMENSION_FIRST + 1),
                                    SampleGiRandom(launch, dimensionFirst + VERTEX_BSDF_DIMENSION_FIRST + 2));
    float3 sampledDirectionWorld =
        SampleStandardBsdf(shadingPoint.Material, shadingPoint.ViewDirectionWorld, directionSample);
    float sampledLengthSquared = dot(sampledDirectionWorld, sampledDirectionWorld);
    // A zero direction is the interface's ordinary rejection.
    if (all(isfinite(sampledDirectionWorld)) == false || sampledLengthSquared <= 0)
        return result;
    float3 directionWorld = sampledDirectionWorld * rsqrt(sampledLengthSquared);
    float cosSurface = dot(shadingPoint.ShadingNormalWorld, directionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(shadingPoint.Material,
                         shadingPoint.ViewDirectionWorld,
                         directionWorld,
                         bsdfValue,
                         bsdfDensity);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false || isfinite(bsdfDensity) == false || bsdfDensity <= 0)
        return result;
    result.Throughput = bsdfValue * (cosSurface / bsdfDensity);
    SurfaceHit endpoint;
    uint status =
        TraceClosest(scene,
                     queries,
                     SpawnSurfaceRay(shadingPoint.Hit, directionWorld, TRANSPORT_MAXIMUM_TRACE_DISTANCE_WORLD),
                     endpoint);
    float3 incidentRadiance = float3(0);
    float lightDensity = 0;
    if (status == SCENE_QUERY_MISS)
    {
        incidentRadiance = EvaluateEnvironment(scene, directionWorld);
        lightDensity = EvaluateEnvironmentPdfSolidAngle(scene, directionWorld);
    }
    else if (status == SCENE_QUERY_SURFACE)
    {
        incidentRadiance = EvaluateEmission(scene, endpoint, -directionWorld);
        // Measured from the receiver itself, as SampleIncidentLight measures its proposals.
        lightDensity = EvaluateLightPdfSolidAngle(scene, shadingPoint.Hit.PositionWorld, endpoint);
        result.Vertex = endpoint;
        result.HasVertex = true;
    }
    // A failed query has latched queries.HasFailed; zero radiance adds nothing.
    if (any(incidentRadiance > 0) == true)
        result.Radiance = result.Throughput * incidentRadiance * ComputePowerHeuristic(bsdfDensity, lightDensity);
    return result;
}

/// Direct lighting at a visible point: emission is added by the caller; both strategies are MIS-weighted and summed.
float3 EstimateDirectLighting(SceneContext scene,
                              RayLaunch launch,
                              ShadingPoint shadingPoint,
                              uint dimensionFirst,
                              inout RayQueryContext queries)
{
    float3 radiance = SampleLightStrategy(scene, launch, shadingPoint, dimensionFirst, true, queries);
    radiance += SampleBsdfStrategy(scene, launch, shadingPoint, dimensionFirst, queries).Radiance;
    return radiance;
}

/// Section 4.1's outgoing radiance L_o(x_s, w_o) at a sample point toward the visible point that traced it: reflected
/// light only, never the sample point's own emission. RESTIR_GI_PATH_BOUNCES further path vertices follow BSDF
/// sampling, each with NEE and MIS; the last vertex uses light sampling alone, whose weight is then one.
float3 EstimateRadianceOut(SceneContext scene,
                           RayLaunch launch,
                           SurfaceHit samplePoint,
                           uint dimensionFirst,
                           inout RayQueryContext queries)
{
    float3 radiance = float3(0);
    float3 throughput = float3(1);
    SurfaceHit vertex = samplePoint;
    [loop] for (uint depth = 0; depth <= RESTIR_GI_PATH_BOUNCES; ++depth)
    {
        ShadingPoint shadingPoint;
        if (TryMakeShadingPoint(scene, vertex, -vertex.IncomingRay.DirectionWorld, queries, shadingPoint) == false)
            break;
        const uint vertexDimension = dimensionFirst + depth * VERTEX_DIMENSION_COUNT;
        const bool hasContinuation = depth < RESTIR_GI_PATH_BOUNCES;
        radiance +=
            throughput * SampleLightStrategy(scene, launch, shadingPoint, vertexDimension, hasContinuation, queries);
        if (hasContinuation == false)
            break;
        BsdfStep step = SampleBsdfStrategy(scene, launch, shadingPoint, vertexDimension, queries);
        radiance += throughput * step.Radiance;
        if (step.HasVertex == false || any(step.Throughput > 0) == false)
            break;
        throughput *= step.Throughput;
        vertex = step.Vertex;
    }
    return radiance;
}
