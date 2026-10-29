#include "hit_record.slang"
#include "polygonal_light.slang"

// Passes 1 to 4 (scene compute): first-hit direct lighting from every emissive triangle of the scene, one output per
// estimator of Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4),
// Article 140, 2021, https://doi.org/10.1145/3450626.3459672 (polygonal_light.slang names the sections):
// - ShadeClampedOptimalMis: one projected solid angle and one LTC sample per triangle, combined with section 4.3's
//   clamped optimal MIS; v is the PROJECTED_SOLID_ANGLE_MIS_VISIBILITY Graph setting (main.lushader.json). The graph's
//   color output. This pass also traces the camera ray and records the hit for the other three.
// - ShadeBalanceHeuristic: the same two samples with Veach's balance heuristic (Peters' Fig. 10b).
// - ShadeProjectedSolidAngle: the projected solid angle sample alone (sections 3 and 6.2).
// - ShadeLtc: the LTC sample alone (section 4.1). The LTC can be zero where the BRDF is not, so this output estimates
//   the shading only within the LTC's support; it is not an unbiased image of the scene.
// As in Peters' renderer (section 6.1), every light is shaded, shadows come from one shadow ray per sample, and the
// estimate is direct lighting only. Every pass returns the surface's own emission plus the sum over triangles; a
// primary miss returns the environment. Analytic lights and the environment do not light surfaces here: the paper's
// techniques sample polygons. The emission at the sampled point (including emission maps and sidedness) and its
// occlusion come from the scene; light samples do not apply the emitter's alpha cutoff, as in the other templates.
// Each triangle costs up to two visibility queries in the MIS passes and one in the single-technique passes; a scene
// with more than MAX_POLYGONAL_LIGHT_COUNT emissive triangles fails explicitly (hit_record.slang).

// Thanks to the Slang authors, core module noinline attribute reference:
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html
// TryLoadShadingPoint, ShadePolygonalLights and SamplePolygonalLightTechnique take the estimator as an argument and
// are not inlined, so the four passes share one copy of the per-triangle code; four specialized copies would exceed
// the package's generated Metal source limit. ShadeEstimator passes them a local copy of queries: SPIR-V logical
// addressing admits no forwarded pointer, so Slang may still inline a helper whose inout argument is forwarded.

struct ShadingPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ViewDirectionWorld;
    PolygonalLightReceiver Receiver;
};

/// Rebuild this pixel's recorded surface, after tracing and recording it when isPrimary. Returns false when the pixel
/// needs no light: a primary miss (outRadiance is the environment) or a failure (queries.HasFailed is set, outRadiance
/// is 0). On success outRadiance is the surface's emission.
[noinline] bool TryLoadShadingPoint(SceneContext scene,
                                    RayLaunch launch,
                                    bool isPrimary,
                                    inout RayQueryContext queries,
                                    out ShadingPoint outPoint,
                                    out float3 outRadiance) {
    outPoint = (ShadingPoint)0;
    outRadiance = float3(0);
    if (HasHitRecordCapacity(launch) == false)
    {
        queries.HasFailed = true;
        return false;
    }
    if (isPrimary == true && TraceAndStoreHitRecord(scene, launch, queries) == SCENE_QUERY_FAILURE)
        return false;
    HitRecord record;
    if (TryLoadHitRecord(scene, launch, record) == false)
    {
        queries.HasFailed = true;
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
    outPoint.Material.ShadingNormalWorld *= rsqrt(normalLengthSquared);
    outPoint.Receiver = MakePolygonalLightReceiver(outPoint.Hit.PositionWorld,
                                                   outPoint.Material.ShadingNormalWorld,
                                                   outPoint.ViewDirectionWorld,
                                                   outPoint.Material.BaseColor,
                                                   outPoint.Material.Metallic,
                                                   outPoint.Material.Roughness);
    outRadiance = EvaluateEmission(scene, outPoint.Hit, outPoint.ViewDirectionWorld);
    return true;
}

    /// One sample of one technique of a prepared triangle, with its emission and shadow ray; zero when the technique
    /// cannot sample, the estimator gives the sample no weight or the sample is occluded.
    [noinline] float3 SamplePolygonalLightTechnique(SceneContext scene,
                                                    ShadingPoint shadingPoint,
                                                    TriangleData triangle,
                                                    PolygonalLight light,
                                                    uint estimator,
                                                    uint technique,
                                                    float2 randomSample,
                                                    inout RayQueryContext queries)
{
    PolygonalLightSample lightSample;
    if (UsesPolygonalLightTechnique(estimator, technique) == false ||
        TrySamplePolygonalLight(shadingPoint.Receiver, light, technique, randomSample, lightSample) == false)
        return float3(0);
    float3 throughput = EstimatePolygonalLightSample(estimator,
                                                     technique,
                                                     shadingPoint.Material,
                                                     shadingPoint.ViewDirectionWorld,
                                                     light,
                                                     lightSample,
                                                     PROJECTED_SOLID_ANGLE_MIS_VISIBILITY);
    if (any(throughput > 0) == false)
        return float3(0);
    SurfaceHit emitterHit = GetSurfaceHit(scene, triangle.TriangleIndex, lightSample.Barycentric);
    if (emitterHit.IsValid == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    float3 emission = EvaluateEmission(scene, emitterHit, -lightSample.DirectionWorld);
    if (any(emission > 0) == false)
        return float3(0);
    LightSample shadowRay = (LightSample)0;
    shadowRay.TriangleIndex = triangle.TriangleIndex;
    shadowRay.Barycentric = lightSample.Barycentric;
    shadowRay.PositionWorld = emitterHit.PositionWorld;
    shadowRay.DirectionWorld = lightSample.DirectionWorld;
    shadowRay.DistanceWorld = lightSample.DistanceWorld;
    shadowRay.HasFiniteEndpoint = true;
    shadowRay.HasSample = true;
    // An occluded or failed query adds nothing; a failure has latched queries.HasFailed.
    if (TestVisibility(scene, queries, shadingPoint.Hit, shadowRay) != SCENE_QUERY_MISS)
        return float3(0);
    return throughput * emission;
}

/// The estimator's direct lighting from every emissive triangle. Triangle k of the GetEmitter/GetEmitterTriangle order
/// reads its own random dimensions (hit_record.slang). Fails explicitly for more than MAX_POLYGONAL_LIGHT_COUNT
/// triangles or an invalid triangle, material or polygon.
[noinline] float3 ShadePolygonalLights(SceneContext scene,
                                       RayLaunch launch,
                                       ShadingPoint shadingPoint,
                                       uint estimator,
                                       inout RayQueryContext queries) {
    uint emitterCount = GetEmitterCount(scene);
    uint lightCount = 0;
    for (uint emitter = 0; emitter < emitterCount; ++emitter)
        lightCount += uint(GetEmitter(scene, emitter).y);
    if (lightCount > MAX_POLYGONAL_LIGHT_COUNT)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    float3 radiance = float3(0);
    uint light = 0;
    for (uint emitter = 0; emitter < emitterCount; ++emitter)
    {
        uint primitiveCount = uint(GetEmitter(scene, emitter).y);
        for (uint primitive = 0; primitive < primitiveCount; ++primitive, ++light)
        {
            TriangleData triangle = GetEmitterTriangle(scene, emitter, primitive);
            MaterialSample emitterMaterial = GetMaterial(scene, triangle.MaterialIndex);
            PolygonalLight polygonalLight;
            if (triangle.IsValid == false || emitterMaterial.IsValid == false ||
                TryPreparePolygonalLight(shadingPoint.Receiver,
                                         triangle.Position0World,
                                         triangle.Position1World,
                                         triangle.Position2World,
                                         triangle.GeometricNormalWorld,
                                         emitterMaterial.Emission,
                                         emitterMaterial.IsEmissionTwoSided,
                                         polygonalLight) == false)
            {
                queries.HasFailed = true;
                return float3(0);
            }
            // A triangle below the horizon or a one-sided emitter seen from behind needs no shadow ray.
            if (any(polygonalLight.DiffuseCoefficient > 0) == false)
                continue;
            uint dimension = POLYGONAL_LIGHT_DIMENSION_FIRST + light * POLYGONAL_LIGHT_DIMENSIONS_PER_LIGHT;
            for (uint technique = 0; technique < POLYGONAL_LIGHT_TECHNIQUE_COUNT; ++technique)
            {
                uint sampleDimension = dimension + (technique == PROJECTED_SOLID_ANGLE_TECHNIQUE
                                                        ? PROJECTED_SOLID_ANGLE_SAMPLE_DIMENSION_OFFSET
                                                        : LTC_SAMPLE_DIMENSION_OFFSET);
                float2 randomSample =
                    float2(SampleSceneRandom(launch, sampleDimension), SampleSceneRandom(launch, sampleDimension + 1));
                radiance += SamplePolygonalLightTechnique(scene,
                                                          shadingPoint,
                                                          triangle,
                                                          polygonalLight,
                                                          estimator,
                                                          technique,
                                                          randomSample,
                                                          queries);
            }
        }
    }
    return radiance;
}

/// Emission plus the estimator's lighting; a nonfinite or negative estimate is a shader failure. Each pass works on a
/// copy of queries and writes it back, as the other templates' passes do.
float3 ShadeEstimator(SceneContext scene, RayLaunch launch, uint estimator, inout RayQueryContext queries)
{
    RayQueryContext passQueries = queries;
    ShadingPoint shadingPoint;
    float3 radiance;
    bool isPrimary = estimator == CLAMPED_OPTIMAL_MIS_ESTIMATOR;
    if (TryLoadShadingPoint(scene, launch, isPrimary, passQueries, shadingPoint, radiance) == true)
    {
        radiance += ShadePolygonalLights(scene, launch, shadingPoint, estimator, passQueries);
        if (IsFiniteBits(radiance) == false || any(radiance < 0) == true)
        {
            passQueries.HasFailed = true;
            radiance = float3(0);
        }
    }
    queries = passQueries;
    return radiance;
}

float3 ShadeClampedOptimalMis(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return ShadeEstimator(scene, launch, CLAMPED_OPTIMAL_MIS_ESTIMATOR, queries);
}

float3 ShadeBalanceHeuristic(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return ShadeEstimator(scene, launch, BALANCE_HEURISTIC_ESTIMATOR, queries);
}

float3 ShadeProjectedSolidAngle(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return ShadeEstimator(scene, launch, PROJECTED_SOLID_ANGLE_ESTIMATOR, queries);
}

float3 ShadeLtc(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return ShadeEstimator(scene, launch, LTC_ESTIMATOR, queries);
}
