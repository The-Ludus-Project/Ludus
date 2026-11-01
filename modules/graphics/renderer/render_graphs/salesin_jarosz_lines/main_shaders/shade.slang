#include "hit_record.slang"
#include "line_sampling.slang"
#include "multiple_importance.slang"

// Passes 2 to 4 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting
// with the strategies that Salesin and Jarosz combine (point-line design D8b):
// - line U and line V: a chord of an emitter triangle in each direction, a point drawn over its
//   visible parts, with the joint line density (line_sampling.slang);
// - BSDF: SampleStandardBsdf proposes a direction and TraceClosest finds what it reaches;
// - area point: SelectLight and SampleIncidentLight propose a point, and TestVisibility traces it.
// Each pass returns surface emission plus one MIS-weighted estimate (multiple_importance.slang):
// - ShadeMultipleImportance: BSDF, line U and line V, each weighted against the other two
//   strategies' densities for the same direction. The graph's color output.
// - ShadeLineSampling: line U and line V, each weighted against the other (lineSampling output).
// - ShadePointSampling: BSDF and the area point, each weighted against the other, as in Veach and
//   Guibas's two-strategy estimator (pointSampling output).
// All three passes read one random layout (hit_record.slang), so the area point is the point that
// seeds the U chord, and line and BSDF samples are shared between the images. A line strategy
// proposes a directional, point, spot or environment light with the point proposal and its density,
// so the two line strategies each take half of such a sample under either heuristic.
// A primary miss returns the environment. There is no indirect bounce. Per pass at most one surface
// query (the BSDF ray), two visibility queries (a non-mesh shadow ray or a masked confirmation per
// line strategy, or the area point's shadow ray) and six line queries (each line sample's own chord
// and the other direction's density at its point, and both line densities at a BSDF-reached
// emitter). The camera ray is not traced again, because the surface comes from the record.

static const float SHADE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
    float3 ViewDirectionWorld;
};

/// Rebuild this pixel's recorded surface. Returns false when the pixel needs no strategy: a primary
/// miss (outRadiance is the environment) or a failure (queries.HasFailed is set, outRadiance is 0).
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
        FailSceneQuery(queries,
                       HasHitRecordCapacity(launch) == false ? LUDUS_SCENE_FAILURE_STORAGE_CAPACITY
                                                             : LUDUS_SCENE_FAILURE_STORED_RECORD);
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

/// Line strategy in direction LINE_DIRECTION_U or LINE_DIRECTION_V, weighted against the other line
/// direction and, with isBsdfWeighted, against BSDF sampling. The other direction's density at a mesh
/// point costs one line query; for a non-mesh light it equals this strategy's density, because both
/// directions propose such a light with the same point proposal.
float3 SampleLineStrategy(SceneContext scene,
                          RayLaunch launch,
                          ShadingPoint shadingPoint,
                          uint direction,
                          bool isBsdfWeighted,
                          inout RayQueryContext queries)
{
    bool isDirectionU = direction == LINE_DIRECTION_U;
    LineProposal proposal;
    if (ProposeLine(scene,
                    launch,
                    shadingPoint.Hit,
                    shadingPoint.ShadingNormalWorld,
                    true,
                    direction,
                    isDirectionU == true ? SHADE_LINE_U_DIMENSION_FIRST : SHADE_LINE_V_DIMENSION_FIRST,
                    isDirectionU == true ? SHADE_LINE_U_POSITION_DIMENSION : SHADE_LINE_V_POSITION_DIMENSION,
                    queries,
                    proposal) == false)
        return float3(0);
    LineSampleValue value;
    if (EvaluateLineProposal(scene,
                             shadingPoint.Hit,
                             shadingPoint.Material,
                             shadingPoint.ShadingNormalWorld,
                             shadingPoint.ViewDirectionWorld,
                             proposal,
                             queries,
                             value) == false)
        return float3(0);
    float otherLineDensity = value.Density;
    if (value.TriangleIndex != SCENE_INVALID_INDEX)
        otherLineDensity = EvaluateLineDensity(scene,
                                               shadingPoint.Hit,
                                               value.TriangleIndex,
                                               value.Barycentric,
                                               isDirectionU == true ? LINE_DIRECTION_V : LINE_DIRECTION_U,
                                               queries);
    float weight = ComputeMisWeight(value.Density, otherLineDensity, isBsdfWeighted == true ? value.BsdfDensity : 0);
    return value.IntegrandValue * (weight / value.Density);
}

/// Area point strategy: one light proposal from the line U selection and shape dimensions and its
/// shadow ray, weighted against BSDF sampling. A delta light keeps weight one.
float3
SamplePointStrategy(SceneContext scene, RayLaunch launch, ShadingPoint shadingPoint, inout RayQueryContext queries)
{
    SelectedLight selected = SelectLight(scene, launch, SHADE_POINT_DIMENSION_FIRST);
    if (selected.HasSample == false)
        return float3(0);
    float3 shapeSample = float3(SampleSceneRandom(launch, SHADE_POINT_DIMENSION_FIRST + 1),
                                SampleSceneRandom(launch, SHADE_POINT_DIMENSION_FIRST + 2),
                                SampleSceneRandom(launch, SHADE_POINT_DIMENSION_FIRST + 3));
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
    float weight = ComputeMisWeight(lightDensity, light.IsDiscrete == true ? 0 : bsdfDensity, 0);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (weight <= 0 || TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface * weight / lightDensity);
}

/// BSDF strategy: one BSDF-sampled ray and the emitter or environment it reaches. With isLineWeighted
/// it is weighted against both line strategies (two line queries at an emitter); otherwise against
/// the area point strategy.
float3 SampleBsdfStrategy(SceneContext scene,
                          RayLaunch launch,
                          ShadingPoint shadingPoint,
                          bool isLineWeighted,
                          inout RayQueryContext queries)
{
    float3 directionSample = float3(SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST),
                                    SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST + 1),
                                    SampleSceneRandom(launch, SHADE_BSDF_DIMENSION_FIRST + 2));
    float3 sampledDirectionWorld =
        SampleStandardBsdf(shadingPoint.Material, shadingPoint.ViewDirectionWorld, directionSample);
    float sampledLengthSquared = dot(sampledDirectionWorld, sampledDirectionWorld);
    // A zero direction is the interface's ordinary rejection.
    if (all(isfinite(sampledDirectionWorld)) == false || sampledLengthSquared <= 0)
        return float3(0);
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
        return float3(0);
    SurfaceHit endpoint;
    uint status = TraceClosest(scene,
                               queries,
                               SpawnSurfaceRay(shadingPoint.Hit, directionWorld, SHADE_MAXIMUM_TRACE_DISTANCE_WORLD),
                               endpoint);
    float3 incidentRadiance = float3(0);
    float otherDensity0 = 0;
    float otherDensity1 = 0;
    if (status == SCENE_QUERY_MISS)
    {
        incidentRadiance = EvaluateEnvironment(scene, directionWorld);
        // Each line strategy proposes the environment with the point proposal's density.
        otherDensity0 = EvaluateEnvironmentPdfSolidAngle(scene, directionWorld);
        otherDensity1 = isLineWeighted == true ? otherDensity0 : 0;
    }
    else if (status == SCENE_QUERY_SURFACE)
    {
        incidentRadiance = EvaluateEmission(scene, endpoint, -directionWorld);
        if (any(incidentRadiance > 0) == true && isLineWeighted == true)
        {
            otherDensity0 = EvaluateLineDensity(scene,
                                                shadingPoint.Hit,
                                                endpoint.TriangleIndex,
                                                endpoint.Barycentric,
                                                LINE_DIRECTION_U,
                                                queries);
            otherDensity1 = EvaluateLineDensity(scene,
                                                shadingPoint.Hit,
                                                endpoint.TriangleIndex,
                                                endpoint.Barycentric,
                                                LINE_DIRECTION_V,
                                                queries);
        }
        else if (any(incidentRadiance > 0) == true)
        {
            // Measured from the receiver itself, as SampleIncidentLight measures its proposals.
            otherDensity0 = EvaluateLightPdfSolidAngle(scene, shadingPoint.Hit.PositionWorld, endpoint);
        }
    }
    // A failed query has latched queries.HasFailed; zero radiance adds nothing.
    if (any(incidentRadiance > 0) == false)
        return float3(0);
    float weight = ComputeMisWeight(bsdfDensity, otherDensity0, otherDensity1);
    return bsdfValue * incidentRadiance * (cosSurface * weight / bsdfDensity);
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

/// Emission plus BSDF, line U and line V samples, MIS-weighted. Weighted estimates are summed.
float3 ShadeMultipleImportance(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    radiance += SampleLineStrategy(scene, launch, shadingPoint, LINE_DIRECTION_U, true, queries);
    radiance += SampleLineStrategy(scene, launch, shadingPoint, LINE_DIRECTION_V, true, queries);
    radiance += SampleBsdfStrategy(scene, launch, shadingPoint, true, queries);
    return FinishRadiance(radiance, queries);
}

/// Emission plus line U and line V samples, each weighted against the other.
float3 ShadeLineSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    radiance += SampleLineStrategy(scene, launch, shadingPoint, LINE_DIRECTION_U, false, queries);
    radiance += SampleLineStrategy(scene, launch, shadingPoint, LINE_DIRECTION_V, false, queries);
    return FinishRadiance(radiance, queries);
}

/// Emission plus BSDF and area point samples, each weighted against the other.
float3 ShadePointSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, queries, shadingPoint, radiance) == false)
        return radiance;
    radiance += SamplePointStrategy(scene, launch, shadingPoint, queries);
    radiance += SampleBsdfStrategy(scene, launch, shadingPoint, false, queries);
    return FinishRadiance(radiance, queries);
}
