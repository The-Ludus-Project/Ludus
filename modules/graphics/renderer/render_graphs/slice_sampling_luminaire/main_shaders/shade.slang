#include "hit_record.slang"
#include "slice_sampling.slang"

// Passes 1 to 3 (scene compute): first-hit direct lighting from the directional luminaire of luminaire.slang, one
// output per proposal of Shlomi Steinberg, "Slice Sampling: Importance sampling with an auxiliary map" (draft,
// University of Waterloo), section 6 (slice_sampling.slang names the equations):
// - ShadeDefensiveSliceSampling: the defensive mixture of Eq. (15), slice sampling with probability
//   1 - SLICE_DEFENSIVE_FRACTION and uniform panel area otherwise. The graph's color output. This pass also traces the
//   camera ray and records the hit for the other two.
// - ShadeSliceSampling: uniform-seed slice sampling alone (Algorithm 1). Receivers whose seeds are unattainable get
//   no light, which is the support failure that section 6.4 reports for far Gaussian tails.
// - ShadeUniformArea: uniform panel-area sampling alone, the baseline of section 6.4.
// Each pass returns the surface's own emission plus one luminaire sample with one shadow ray; a primary miss returns
// the environment. Scene lights, mesh emitters and the environment do not light surfaces: the template evaluates the
// direct-light estimator of Eq. (19) for its analytic luminaire only. There is no indirect bounce. At most one surface
// and one visibility query per pass.

LuminaireProfile MakeLuminaireProfile()
{
    LuminaireProfile profile;
    profile.RadianceScale = LUMINAIRE_RADIANCE_SCALE;
    profile.BeamWidth = LUMINAIRE_BEAM_WIDTH;
    profile.BeamSteering = LUMINAIRE_BEAM_STEERING;
    profile.BeamSpread = LUMINAIRE_BEAM_SPREAD;
    profile.BeamTwist = LUMINAIRE_BEAM_TWIST;
    return profile;
}

/// True when no channel is infinite or NaN, tested on the bits: Metal compiles with fast math, which may fold isfinite.
bool IsRadianceFinite(float3 radiance)
{
    static const uint EXPONENT_BITS = 0x7f800000u;
    return all((asuint(radiance) & EXPONENT_BITS) != EXPONENT_BITS);
}

static const uint DEFENSIVE_SLICE_SAMPLING_ESTIMATOR = 0;
static const uint SLICE_SAMPLING_ESTIMATOR = 1;
static const uint UNIFORM_AREA_ESTIMATOR = 2;

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ShadingNormalWorld;
    float3 ViewDirectionWorld;
};

/// Rebuild this pixel's recorded surface, after tracing and recording it when isPrimary. Returns false when the pixel
/// needs no light: a primary miss (outRadiance is the environment) or a failure (queries.HasFailed is set, outRadiance
/// is 0). On success outRadiance is the surface's emission.
bool TryLoadShadingPoint(SceneContext scene,
                         RayLaunch launch,
                         bool isPrimary,
                         inout RayQueryContext queries,
                         out ShadingPoint outPoint,
                         out float3 outRadiance)
{
    outPoint = (ShadingPoint)0;
    outRadiance = float3(0);
    if (HasHitRecordCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return false;
    }
    if (isPrimary == true && TraceAndStoreHitRecord(scene, launch, queries) == SCENE_QUERY_FAILURE)
        return false;
    HitRecord record;
    if (TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return false;
    }
    // The first pass traced this exact ray: same launch, camera and stored jitter.
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

/// One luminaire proposal of the estimator, with its BSDF, receiver cosine and shadow ray; zero for a null outcome,
/// a direction below the surface or an occluded panel point.
float3 SampleLuminaire(SceneContext scene,
                       RayLaunch launch,
                       ShadingPoint shadingPoint,
                       uint estimator,
                       inout RayQueryContext queries)
{
    LuminaireProfile profile = MakeLuminaireProfile();
    LuminaireReceiver receiver = MakeLuminaireReceiver(shadingPoint.Hit.PositionWorld);
    float2 seedSample = float2(SampleSceneRandom(launch, SLICE_SEED_DIMENSION_FIRST),
                               SampleSceneRandom(launch, SLICE_SEED_DIMENSION_FIRST + 1));
    float branchSample = SampleSceneRandom(launch, SLICE_BRANCH_DIMENSION);
    float2 areaSample = float2(SampleSceneRandom(launch, PANEL_AREA_DIMENSION_FIRST),
                               SampleSceneRandom(launch, PANEL_AREA_DIMENSION_FIRST + 1));
    LuminaireLightSample light;
    if (estimator == DEFENSIVE_SLICE_SAMPLING_ESTIMATOR)
    {
        light = SampleLuminaireDefensive(profile,
                                         receiver,
                                         SLICE_DEFENSIVE_FRACTION,
                                         SampleSceneRandom(launch, MIXTURE_COMPONENT_DIMENSION),
                                         seedSample,
                                         branchSample,
                                         areaSample);
    }
    else if (estimator == SLICE_SAMPLING_ESTIMATOR)
    {
        float seedWeight;
        float guideDensity;
        light = SampleLuminaireSlice(profile, receiver, seedSample, branchSample, seedWeight, guideDensity);
    }
    else
    {
        light = SampleLuminaireArea(profile, receiver, areaSample);
    }
    if (light.HasSample == false || any(light.RadianceOverDensity > 0) == false)
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
    // The panel is not scene geometry: trace to the exact panel point, which TestVisibility does not offset.
    LightSample shadowRay = (LightSample)0;
    shadowRay.TriangleIndex = SCENE_INVALID_INDEX;
    shadowRay.PositionWorld = light.PositionWorld;
    shadowRay.DirectionWorld = light.DirectionWorld;
    shadowRay.DistanceWorld = length(light.PositionWorld - shadingPoint.Hit.PositionWorld);
    shadowRay.HasFiniteEndpoint = true;
    shadowRay.HasSample = true;
    // An occluded or failed query adds nothing; a failure has latched queries.HasFailed.
    if (TestVisibility(scene, queries, shadingPoint.Hit, shadowRay) != SCENE_QUERY_MISS)
        return float3(0);
    return bsdfValue * light.RadianceOverDensity * cosSurface;
}

/// Emission plus the estimator's luminaire sample; a nonfinite or negative estimate is a shader failure. Each pass
/// works on a copy of queries and writes it back, as the other templates' passes do.
float3 ShadeEstimator(SceneContext scene, RayLaunch launch, uint estimator, inout RayQueryContext queries)
{
    RayQueryContext passQueries = queries;
    ShadingPoint shadingPoint;
    float3 radiance;
    bool isPrimary = estimator == DEFENSIVE_SLICE_SAMPLING_ESTIMATOR;
    if (TryLoadShadingPoint(scene, launch, isPrimary, passQueries, shadingPoint, radiance) == true)
    {
        radiance += SampleLuminaire(scene, launch, shadingPoint, estimator, passQueries);
        if (IsRadianceFinite(radiance) == false || any(radiance < 0) == true)
        {
            FailSceneQuery(passQueries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
            radiance = float3(0);
        }
    }
    queries = passQueries;
    return radiance;
}

float3 ShadeDefensiveSliceSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return ShadeEstimator(scene, launch, DEFENSIVE_SLICE_SAMPLING_ESTIMATOR, queries);
}

float3 ShadeSliceSampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return ShadeEstimator(scene, launch, SLICE_SAMPLING_ESTIMATOR, queries);
}

float3 ShadeUniformArea(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return ShadeEstimator(scene, launch, UNIFORM_AREA_ESTIMATOR, queries);
}
