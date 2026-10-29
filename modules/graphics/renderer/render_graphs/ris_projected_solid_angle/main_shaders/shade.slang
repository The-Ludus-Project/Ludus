#include "hit_record.slang"
#include "resampled_light.slang"

// Passes 2 to 4 (scene compute): rebuild the recorded surface and estimate first-hit direct lighting. The estimator is
// docs/architecture/many-area-light-sampling-reference-review.md ("Proposed estimator", "Decision: BSDF-hit MIS"):
// - light: SelectLight chooses a light category uniformly, as veach-guibas-mis does. Analytic lights and the
//   environment keep SampleIncidentLight. The emissive set, chosen with probability P_E (emitters over lights),
//   resamples one triangle y from RIS_CANDIDATE_COUNT power-CDF candidates (resampled_light.slang). It then takes one
//   diffuse projected-solid-angle sample and, unless the LTC-space clip is empty, one LTC sample of y, combined
//   with Peters' clamped optimal MIS (projected_solid_angle_light.slang). Each sample traces one shadow ray to the
//   point where its direction meets y, so emissive NEE costs up to two shadow rays. The contribution of technique s
//   is W_L w_s F / p_s times W / P_E, with the reservoir's contribution weight W.
// - BSDF: SampleStandardBsdf proposes a direction and TraceClosest finds what it reaches.
// Each pass returns surface emission plus:
// - ShadeMultipleImportance: both strategies. The light group and the BSDF sample are weighted by the power heuristic
//   against each other's density. For an emissive triangle t the light density is the surrogate
//   P-tilde(t) (p_0 + p_1), with P-tilde from an independent normalization estimate (EvaluateSurrogateTriangle-
//   Probability); RIS itself has no tractable density. The graph's color output.
// - ShadeLightSampling: the light strategy alone (W_L = 1), Shah et al.'s estimator (lightSampling output).
// - ShadeBsdfSampling: the BSDF strategy alone (bsdfSampling output). It never reaches a directional, point or spot
//   light, so those lights are absent from this image.
// TryLoadShadingPoint clips a front face's shading normal to the hemisphere of the view direction, as Peters'
// renderer does (Peters 2021, section 6.1, through FoundationMath's shading_normal.slang); every strategy shades with
// that normal. The three passes read the same random dimensions (hit_record.slang), and dimensions 2 to 8 keep their
// veach-guibas-mis meanings. All densities are solid-angle densities. A delta light sample keeps weight one under MIS.
// A primary miss returns the environment. There is no indirect bounce. At most one surface query and two visibility
// queries; the camera ray is not traced again, because the surface comes from the record.
// Departures from the review: like veach-guibas-mis, light samples do not apply the emitter's own alpha cutoff, because
// the scene interface has no alpha-acceptance query; an alpha-cut emissive texel is therefore lit by light sampling
// but transparent to BSDF rays. Emission maps, sidedness and occlusion are evaluated at the sampled point. The section
// 6.1 clip departs from veach-guibas-mis and RT1, which shade with the unclipped normal; its own departures are
// listed in projected_solid_angle_light.slang.
// PROJECTED_SOLID_ANGLE_MIS_VISIBILITY is a Graph setting (main.lushader.json), Peters' v.

static const float SHADE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;
static const uint DIFFUSE_LIGHT_TECHNIQUE = 0;
static const uint SPECULAR_LIGHT_TECHNIQUE = 1;

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
    float3 ViewDirectionWorld;
    ReceiverLightingFrame Frame;
};

/// Per-invocation state of the resampled light strategy shared by its light and BSDF-hit MIS weights.
struct ResampledLightContext
{
    /// P_E: the probability that SelectLight chooses the emissive set.
    float EmissiveCategoryProbability;
    TargetTotalEstimate TargetTotal;
};

ResampledLightContext MakeResampledLightContext(SceneContext scene)
{
    ResampledLightContext result;
    uint lightCount = GetLightCount(scene);
    result.EmissiveCategoryProbability = lightCount > 0 ? float(GetEmitterCount(scene)) / float(lightCount) : 0;
    result.TargetTotal = MakeTargetTotalEstimate();
    return result;
}

/// Rebuild this pixel's recorded surface. Returns false when the pixel needs no strategy: a primary
/// miss (outRadiance is the environment) or a failure (queries.HasFailed is set, outRadiance is 0).
/// hasLightFrame also builds the receiver's frame and LTC, which only the light strategy and MIS need.
/// TryLoadShadingPoint, SampleLightStrategy and SampleBsdfStrategy are not inlined, so the passes that share them
/// share one copy in the generated artifact.
[noinline] bool TryLoadShadingPoint(SceneContext scene,
                                    RayLaunch launch,
                                    bool hasLightFrame,
                                    inout RayQueryContext queries,
                                    out ShadingPoint outPoint,
                                    out float3 outRadiance) {
    outPoint = (ShadingPoint)0;
    outRadiance = float3(0);
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        queries.HasFailed = true;
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
        queries.HasFailed = true;
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
        queries.HasFailed = true;
        return false;
    }
    outPoint.ShadingNormalWorld = outPoint.Material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    // Peters 2021, section 6.1: the BSDF, BSDF sampling and both light techniques use the shading normal clipped to the
    // view's hemisphere, in all three passes. Only a front face is clipped: a one-sided surface seen from behind keeps
    // a zero BSDF, as in the other templates, because a normal clipped towards the viewer would light it from the far
    // side of the surface.
    if (outPoint.Hit.IsFrontFace == true)
        outPoint.ShadingNormalWorld =
            ClipShadingNormalToOutgoingHemisphere(outPoint.ShadingNormalWorld, outPoint.ViewDirectionWorld);
    outPoint.Material.ShadingNormalWorld = outPoint.ShadingNormalWorld;
    if (hasLightFrame == true)
        outPoint.Frame = MakeReceiverLightingFrame(outPoint.Hit.PositionWorld,
                                                   outPoint.ShadingNormalWorld,
                                                   outPoint.ViewDirectionWorld,
                                                   outPoint.Material.BaseColor,
                                                   outPoint.Material.Metallic,
                                                   outPoint.Material.Roughness);
    outRadiance = EvaluateEmission(scene, outPoint.Hit, outPoint.ViewDirectionWorld);
    return true;
}

/// P-tilde(t) for an evaluated emissive triangle, computing Z-hat on first use. A triangle that the resampled strategy
/// cannot propose (zero target or source probability, or no emissive category) has zero probability. Returns false
/// when Z-hat fails.
bool TryEvaluateSurrogateProbability(SceneContext scene,
                                     RayLaunch launch,
                                     ShadingPoint shadingPoint,
                                     EmissiveTriangleTarget target,
                                     inout ResampledLightContext inoutContext,
                                     out float outProbability)
{
    outProbability = 0;
    float sourcePmf = EvaluateEmissiveTriangleSelectionPmf(scene, target.Triangle.TriangleIndex);
    if ((sourcePmf > 0) == false || (target.Target > 0) == false ||
        (inoutContext.EmissiveCategoryProbability > 0) == false)
        return true;
    if (TryEnsureTargetTotalEstimate(scene, launch, shadingPoint.Frame, inoutContext.TargetTotal) == false)
        return false;
    outProbability = EvaluateSurrogateTriangleProbability(inoutContext.EmissiveCategoryProbability,
                                                          target.Target,
                                                          sourcePmf,
                                                          inoutContext.TargetTotal.Value,
                                                          RIS_ACTIVE_CANDIDATE_COUNT);
    return true;
}

/// One sample of the selected triangle by light technique `technique` from randomSample, with its shadow ray; zero
/// when the technique is disabled or cannot sample the direction, the direction misses the triangle in FP32 or the
/// sample is occluded. An empty LTC-space clip disables the specular technique (Peters 2021, section 4.1). Densities
/// are evaluated at the normalized world direction, as the BSDF-hit weights evaluate them, so W_L is one function of
/// the direction at both sites. surrogateProbability is P-tilde(y); isMisWeighted applies W_L against BSDF sampling.
/// Not inlined, like TryEvaluateEmissiveTriangleTarget, so both techniques share one copy.
[noinline] float3 SampleResampledLightTechnique(SceneContext scene,
                                                ShadingPoint shadingPoint,
                                                EmissiveTriangleTarget target,
                                                uint technique,
                                                float2 randomSample,
                                                float surrogateProbability,
                                                bool isMisWeighted,
                                                inout RayQueryContext queries) {
    float3 directionTangent = float3(0);
    bool isSampled = false;
    if (technique == DIFFUSE_LIGHT_TECHNIQUE)
        isSampled = TrySampleProjectedSolidAnglePolygon(target.Polygons.Diffuse, randomSample, directionTangent);
    else if (target.Polygons.SpecularIntegral > 0)
        isSampled = TrySampleLinearlyTransformedCosinePolygon(target.Polygons.Specular, randomSample, directionTangent);
    if (isSampled == false)
        return float3(0);
    float3 directionWorld = NormalizeOrZero(TransformTangentToWorld(shadingPoint.Frame, directionTangent));
    float2 densities =
        EvaluateLightTechniqueDensities(target.Polygons, TransformWorldToTangent(shadingPoint.Frame, directionWorld));
    float techniqueDensity = technique == DIFFUSE_LIGHT_TECHNIQUE ? densities.x : densities.y;
    if ((techniqueDensity > 0) == false)
        return float3(0);
    float cosSurface = dot(shadingPoint.ShadingNormalWorld, directionWorld);
    float3 bsdfValue;
    float bsdfDensity;
    EvaluateStandardBsdf(shadingPoint.Material,
                         shadingPoint.ViewDirectionWorld,
                         directionWorld,
                         bsdfValue,
                         bsdfDensity);
    if (cosSurface <= 0 || any(bsdfValue > 0) == false)
        return float3(0);
    float distanceWorld;
    float2 barycentric;
    if (TryIntersectTrianglePlane(shadingPoint.Frame.PositionWorld,
                                  directionWorld,
                                  target.Triangle.Position0World,
                                  target.Triangle.Position1World,
                                  target.Triangle.Position2World,
                                  distanceWorld,
                                  barycentric) == false)
        return float3(0);
    SurfaceHit emitterHit = GetSurfaceHit(scene, target.Triangle.TriangleIndex, barycentric);
    if (emitterHit.IsValid == false)
        return float3(0);
    float3 emission = EvaluateEmission(scene, emitterHit, -directionWorld);
    if (any(emission > 0) == false)
        return float3(0);
    float2 techniqueWeights =
        ComputeClampedOptimalMisWeights(target.Coefficients, densities, PROJECTED_SOLID_ANGLE_MIS_VISIBILITY);
    float weight = technique == DIFFUSE_LIGHT_TECHNIQUE ? techniqueWeights.x : techniqueWeights.y;
    if (isMisWeighted == true)
        weight *= ComputePowerHeuristicWeight(surrogateProbability * (densities.x + densities.y), bsdfDensity);
    if ((weight > 0) == false)
        return float3(0);
    LightSample light = (LightSample)0;
    light.TriangleIndex = target.Triangle.TriangleIndex;
    light.Barycentric = barycentric;
    light.PositionWorld = emitterHit.PositionWorld;
    light.DirectionWorld = directionWorld;
    light.DistanceWorld = distanceWorld;
    light.HasFiniteEndpoint = true;
    light.HasSample = true;
    // An occluded or failed query adds nothing; a failure has latched queries.HasFailed.
    if (TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * emission * (cosSurface * weight / techniqueDensity);
}

/// The emissive branch of the light strategy: resample y, then sample it once per technique. Returns zero without a
/// selection, and zero with queries.HasFailed set when an evaluation fails.
float3 SampleResampledEmissiveLight(SceneContext scene,
                                    RayLaunch launch,
                                    ShadingPoint shadingPoint,
                                    bool isMisWeighted,
                                    inout ResampledLightContext inoutContext,
                                    inout RayQueryContext queries)
{
    ResampledTriangle selection;
    if (TryResampleEmissiveTriangle(scene, launch, shadingPoint.Frame, selection) == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    if (selection.HasSelection == false || (selection.ContributionWeight > 0) == false)
        return float3(0);
    EmissiveTriangleTarget target;
    float surrogateProbability = 0;
    if (TryEvaluateEmissiveTriangleTarget(scene, shadingPoint.Frame, selection.TriangleIndex, target) == false ||
        (isMisWeighted == true &&
         TryEvaluateSurrogateProbability(scene, launch, shadingPoint, target, inoutContext, surrogateProbability) ==
             false))
    {
        queries.HasFailed = true;
        return float3(0);
    }
    // One diffuse sample omega_0 and, unless the LTC-space clip is empty, one LTC sample omega_1.
    float2 diffuseSample = float2(SampleSceneRandom(launch, SHADE_DIFFUSE_TECHNIQUE_DIMENSION_FIRST),
                                  SampleSceneRandom(launch, SHADE_DIFFUSE_TECHNIQUE_DIMENSION_FIRST + 1));
    float2 specularSample = float2(SampleSceneRandom(launch, SHADE_SPECULAR_TECHNIQUE_DIMENSION_FIRST),
                                   SampleSceneRandom(launch, SHADE_SPECULAR_TECHNIQUE_DIMENSION_FIRST + 1));
    float3 radiance = float3(0);
    for (uint technique = DIFFUSE_LIGHT_TECHNIQUE; technique <= SPECULAR_LIGHT_TECHNIQUE; ++technique)
        radiance += SampleResampledLightTechnique(scene,
                                                  shadingPoint,
                                                  target,
                                                  technique,
                                                  technique == DIFFUSE_LIGHT_TECHNIQUE ? diffuseSample : specularSample,
                                                  surrogateProbability,
                                                  isMisWeighted,
                                                  queries);
    return radiance * (selection.ContributionWeight / inoutContext.EmissiveCategoryProbability);
}

/// Light strategy: one category draw. The emissive set is resampled; any other light keeps one SampleIncidentLight
/// proposal and its shadow ray. With isMisWeighted it is weighted against BSDF sampling; otherwise weight one.
[noinline] float3 SampleLightStrategy(SceneContext scene,
                                      RayLaunch launch,
                                      ShadingPoint shadingPoint,
                                      bool isMisWeighted,
                                      inout ResampledLightContext inoutContext,
                                      inout RayQueryContext queries) {
    SelectedLight selected = SelectLight(scene, launch, SHADE_LIGHT_SELECTION_DIMENSION);
    if (selected.HasSample == false)
        return float3(0);
    if (GetLightInfo(scene, selected.LightIndex).Kind == SCENE_LIGHT_MESH)
        return SampleResampledEmissiveLight(scene, launch, shadingPoint, isMisWeighted, inoutContext, queries);
    float3 shapeSample = float3(SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST),
                                SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 1),
                                SampleSceneRandom(launch, SHADE_LIGHT_SHAPE_DIMENSION_FIRST + 2));
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
    float weight = 1;
    if (isMisWeighted == true && light.IsDiscrete == false)
        weight = ComputePowerHeuristicWeight(lightDensity, bsdfDensity);
    // Skip the shadow ray when the sample cannot contribute; an occluded or failed query adds nothing.
    if (weight <= 0 || TestVisibility(scene, queries, shadingPoint.Hit, light) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.IncidentCoefficient * (cosSurface * weight / lightDensity);
}

/// The light strategy's density for a BSDF direction that reached an emissive triangle: P-tilde(t) (p_0 + p_1),
/// measured from the receiver itself. Returns false when an evaluation fails.
bool TryEvaluateResampledLightDensity(SceneContext scene,
                                      RayLaunch launch,
                                      ShadingPoint shadingPoint,
                                      uint triangleIndex,
                                      float3 directionWorld,
                                      inout ResampledLightContext inoutContext,
                                      out float outDensity)
{
    outDensity = 0;
    if ((EvaluateEmissiveTriangleSelectionPmf(scene, triangleIndex) > 0) == false)
        return true;
    EmissiveTriangleTarget target;
    float surrogateProbability;
    if (TryEvaluateEmissiveTriangleTarget(scene, shadingPoint.Frame, triangleIndex, target) == false ||
        TryEvaluateSurrogateProbability(scene, launch, shadingPoint, target, inoutContext, surrogateProbability) ==
            false)
        return false;
    float2 densities =
        EvaluateLightTechniqueDensities(target.Polygons, TransformWorldToTangent(shadingPoint.Frame, directionWorld));
    outDensity = surrogateProbability * (densities.x + densities.y);
    return true;
}

/// BSDF strategy: one BSDF-sampled ray and the emitter or environment it reaches. With isMisWeighted
/// it is weighted against light sampling; otherwise it is the plain BSDF-sampling estimator.
[noinline] float3 SampleBsdfStrategy(SceneContext scene,
                                     RayLaunch launch,
                                     ShadingPoint shadingPoint,
                                     bool isMisWeighted,
                                     inout ResampledLightContext inoutContext,
                                     inout RayQueryContext queries) {
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
    float lightDensity = 0;
    if (status == SCENE_QUERY_MISS)
    {
        incidentRadiance = EvaluateEnvironment(scene, directionWorld);
        if (isMisWeighted == true)
            lightDensity = EvaluateEnvironmentPdfSolidAngle(scene, directionWorld);
    }
    else if (status == SCENE_QUERY_SURFACE)
    {
        incidentRadiance = EvaluateEmission(scene, endpoint, -directionWorld);
        if (isMisWeighted == true && any(incidentRadiance > 0) == true &&
            TryEvaluateResampledLightDensity(scene,
                                             launch,
                                             shadingPoint,
                                             endpoint.TriangleIndex,
                                             directionWorld,
                                             inoutContext,
                                             lightDensity) == false)
        {
            queries.HasFailed = true;
            return float3(0);
        }
    }
    // A failed query has latched queries.HasFailed; zero radiance adds nothing.
    if (any(incidentRadiance > 0) == false)
        return float3(0);
    float weight = isMisWeighted == true ? ComputePowerHeuristicWeight(bsdfDensity, lightDensity) : 1;
    return bsdfValue * incidentRadiance * (cosSurface * weight / bsdfDensity);
}

/// Reject a nonfinite or negative estimate as a shader failure.
float3 FinishRadiance(float3 radiance, inout RayQueryContext queries)
{
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    return radiance;
}

// The non-inlined helpers above need a local variable as their inout argument: SPIR-V logical addressing admits no
// forwarded pointer, so Slang would otherwise inline them. Each pass therefore works on a copy of queries and writes
// it back.

/// Emission plus both strategies, MIS-weighted. Complementary MIS estimates are summed, not averaged.
float3 ShadeMultipleImportance(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    RayQueryContext passQueries = queries;
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, true, passQueries, shadingPoint, radiance) == true)
    {
        ResampledLightContext context = MakeResampledLightContext(scene);
        radiance += SampleLightStrategy(scene, launch, shadingPoint, true, context, passQueries);
        radiance += SampleBsdfStrategy(scene, launch, shadingPoint, true, context, passQueries);
        radiance = FinishRadiance(radiance, passQueries);
    }
    queries = passQueries;
    return radiance;
}

/// Emission plus the light strategy alone: RIS and projected-solid-angle/LTC sampling for emissive triangles.
float3 ShadeLightSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    RayQueryContext passQueries = queries;
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, true, passQueries, shadingPoint, radiance) == true)
    {
        ResampledLightContext context = MakeResampledLightContext(scene);
        radiance += SampleLightStrategy(scene, launch, shadingPoint, false, context, passQueries);
        radiance = FinishRadiance(radiance, passQueries);
    }
    queries = passQueries;
    return radiance;
}

/// Emission plus the BSDF strategy alone.
float3 ShadeBsdfSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    RayQueryContext passQueries = queries;
    ShadingPoint shadingPoint;
    float3 radiance;
    if (TryLoadShadingPoint(scene, launch, false, passQueries, shadingPoint, radiance) == true)
    {
        ResampledLightContext context = MakeResampledLightContext(scene);
        radiance += SampleBsdfStrategy(scene, launch, shadingPoint, false, context, passQueries);
        radiance = FinishRadiance(radiance, passQueries);
    }
    queries = passQueries;
    return radiance;
}
