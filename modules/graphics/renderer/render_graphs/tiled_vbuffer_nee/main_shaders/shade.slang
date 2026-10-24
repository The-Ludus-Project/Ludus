#include "tile_table.slang"

// Pass 3 of 3: direct mesh-emitter lighting at the visibility-buffer surface. Two MIS
// strategies, combined with the power heuristic and summed with the surface emission:
// - light: an emitter from this pixel's tile table, a triangle by area, a uniform point,
//   one occlusion query (TestVisibility);
// - BSDF: one closest-hit query (TraceClosest); an emitter hit is weighted with the light
//   PDF read from the same tile table.
// No closest-hit or miss callback runs, and no indirect bounce is traced.
// Thanks to Pharr, Jakob and Humphreys, PBRT 4e, "A Better Path Tracer":
// https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer

static const float TILED_NEE_MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;

/// Power heuristic for the strategy that drew the sample, robust to large PDF ratios.
float ComputePowerHeuristic(float sampledPdf, float otherPdf)
{
    if (sampledPdf <= 0.0f)
    {
        return 0.0f;
    }
    if (otherPdf <= 0.0f)
    {
        return 1.0f;
    }
    const float maximumPdf = max(sampledPdf, otherPdf);
    const float sampled = sampledPdf / maximumPdf;
    const float other = otherPdf / maximumPdf;
    return (sampled * sampled) / (sampled * sampled + other * other);
}

/// Convert both densities to one measure, scaled by a factor of at most one, then weight.
float ComputeMisWeight(float lightPdfArea,
                       float bsdfPdfSolidAngle,
                       float distanceSquaredWorld,
                       float cosEmitter,
                       bool isLightSample)
{
    float lightPdf = lightPdfArea;
    float bsdfPdf = bsdfPdfSolidAngle * (cosEmitter / distanceSquaredWorld);
    if (distanceSquaredWorld < cosEmitter)
    {
        lightPdf = lightPdfArea * (distanceSquaredWorld / cosEmitter);
        bsdfPdf = bsdfPdfSolidAngle;
    }
    return isLightSample == true ? ComputePowerHeuristic(lightPdf, bsdfPdf) : ComputePowerHeuristic(bsdfPdf, lightPdf);
}

/// Choose a triangle of the emitter in proportion to its area, consuming one stream dimension.
bool TrySampleEmitterTriangle(SceneContext scene,
                              uint emitterIndex,
                              inout RayQueryContext queries,
                              out TriangleData outTriangle)
{
    outTriangle = (TriangleData)0;
    const float4 emitter = GetEmitter(scene, emitterIndex);
    const uint triangleCount = uint(emitter.y);
    if (triangleCount == 0 || isfinite(emitter.z) == false || emitter.z <= 0.0f)
    {
        return false;
    }
    const float targetAreaWorld = SampleSceneRandom(queries) * emitter.z;
    float cumulativeAreaWorld = 0.0f;
    [loop] for (uint i = 0; i < triangleCount; ++i)
    {
        const TriangleData triangle = GetEmitterTriangle(scene, emitterIndex, i);
        if (triangle.IsValid == false)
        {
            queries.HasFailed = true;
            return false;
        }
        if (triangle.AreaWorld <= 0.0f)
        {
            continue;
        }
        // The last positive-area triangle is the cumulative-rounding fallback.
        outTriangle = triangle;
        cumulativeAreaWorld += triangle.AreaWorld;
        if (targetAreaWorld < cumulativeAreaWorld)
        {
            break;
        }
    }
    return queries.HasFailed == false && outTriangle.IsValid == true && outTriangle.AreaWorld > 0.0f;
}

float3 SampleLightDirectLighting(SceneContext scene,
                                 SurfaceHit receiver,
                                 MaterialSample material,
                                 float3 shadingNormalWorld,
                                 float3 viewDirectionWorld,
                                 uint tileIndex,
                                 inout RayQueryContext queries)
{
    const TileEmitterSelection selection = SelectTileEmitter(scene, tileIndex, queries);
    TriangleData triangle;
    if (selection.HasSample == false ||
        TrySampleEmitterTriangle(scene, selection.EmitterIndex, queries, triangle) == false)
    {
        return float3(0);
    }
    // Area-proportional triangle choice and a uniform point give density 1 / emitter area.
    const float lightPdfArea = selection.SelectionPmf / GetEmitter(scene, selection.EmitterIndex).z;
    const float u = SampleSceneRandom(queries);
    const float v = SampleSceneRandom(queries);
    if (queries.HasFailed == true || isfinite(lightPdfArea) == false || lightPdfArea <= 0.0f)
    {
        return float3(0);
    }
    const float root = sqrt(u);
    const float2 barycentric = float2(root * (1.0f - v), root * v);
    const SurfaceHit endpoint = GetSurfaceHit(scene, triangle.TriangleIndex, barycentric);
    if (endpoint.IsValid == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    const float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
    const float distanceSquaredWorld = dot(offsetWorld, offsetWorld);
    if (isfinite(distanceSquaredWorld) == false || distanceSquaredWorld <= 0.0f)
    {
        return float3(0);
    }
    const float distanceWorld = sqrt(distanceSquaredWorld);
    const float3 directionWorld = offsetWorld / distanceWorld;
    const float cosSurface = max(dot(shadingNormalWorld, directionWorld), 0.0f);
    const float cosEmitter = abs(dot(normalize(endpoint.GeometricNormalWorld), -directionWorld));
    if (cosSurface <= 0.0f || cosEmitter <= 0.0f)
    {
        return float3(0);
    }
    const float3 emission = EvaluateEmission(scene, endpoint, -directionWorld);
    if (any(emission > 0.0f) == false)
    {
        return float3(0);
    }
    float3 bsdfValue;
    float bsdfPdfSolidAngle;
    EvaluateStandardBsdf(material, viewDirectionWorld, directionWorld, bsdfValue, bsdfPdfSolidAngle);
    if (any(bsdfValue > 0.0f) == false)
    {
        return float3(0);
    }
    const float misWeight = ComputeMisWeight(lightPdfArea, bsdfPdfSolidAngle, distanceSquaredWorld, cosEmitter, true);
    if (misWeight <= 0.0f)
    {
        return float3(0);
    }
    LightSample lightSample = (LightSample)0;
    lightSample.TriangleIndex = triangle.TriangleIndex;
    lightSample.Barycentric = barycentric;
    lightSample.PositionWorld = endpoint.PositionWorld;
    lightSample.DirectionWorld = directionWorld;
    lightSample.DistanceWorld = distanceWorld;
    lightSample.HasFiniteEndpoint = true;
    lightSample.HasSample = true;
    if (TestVisibility(scene, queries, receiver, lightSample) != SCENE_QUERY_MISS)
    {
        return float3(0);
    }
    // Area measure: f * Le * cosSurface * cosEmitter / (r^2 * pA).
    return emission * bsdfValue * ((cosSurface * cosEmitter * misWeight / distanceSquaredWorld) / lightPdfArea);
}

float3 SampleBsdfDirectLighting(SceneContext scene,
                                SurfaceHit receiver,
                                MaterialSample material,
                                float3 shadingNormalWorld,
                                float3 viewDirectionWorld,
                                uint tileIndex,
                                inout RayQueryContext queries)
{
    const float sampleLobe = SampleSceneRandom(queries);
    const float sampleRadius = SampleSceneRandom(queries);
    const float sampleAngle = SampleSceneRandom(queries);
    if (queries.HasFailed == true)
    {
        return float3(0);
    }
    const float3 sampledDirection =
        SampleStandardBsdf(material, viewDirectionWorld, float3(sampleLobe, sampleRadius, sampleAngle));
    const float sampledLengthSquared = dot(sampledDirection, sampledDirection);
    // A zero direction is the interface's ordinary rejection.
    if (all(isfinite(sampledDirection)) == false || sampledLengthSquared <= 0.0f)
    {
        return float3(0);
    }
    const float3 directionWorld = sampledDirection * rsqrt(sampledLengthSquared);
    const float cosSurface = max(dot(shadingNormalWorld, directionWorld), 0.0f);
    if (cosSurface <= 0.0f)
    {
        return float3(0);
    }
    float3 bsdfValue;
    float bsdfPdfSolidAngle;
    EvaluateStandardBsdf(material, viewDirectionWorld, directionWorld, bsdfValue, bsdfPdfSolidAngle);
    if (any(bsdfValue > 0.0f) == false || bsdfPdfSolidAngle <= 0.0f)
    {
        return float3(0);
    }
    const Ray ray = SpawnSurfaceRay(receiver, directionWorld, TILED_NEE_MAXIMUM_TRACE_DISTANCE_WORLD);
    SurfaceHit endpoint;
    if (TraceClosest(scene, queries, ray, endpoint) != SCENE_QUERY_SURFACE ||
        endpoint.EmitterIndex >= GetEmitterCount(scene))
    {
        return float3(0);
    }
    const float3 emission = EvaluateEmission(scene, endpoint, -directionWorld);
    if (any(emission > 0.0f) == false)
    {
        return float3(0);
    }
    const float emitterAreaWorld = GetEmitter(scene, endpoint.EmitterIndex).z;
    const float lightPdfArea = emitterAreaWorld > 0.0f
                                   ? EvaluateTileEmitterPmf(scene, tileIndex, endpoint.EmitterIndex) / emitterAreaWorld
                                   : 0.0f;
    float misWeight = 1.0f;
    if (lightPdfArea > 0.0f)
    {
        // The light proposal's density is measured from the receiver itself, not the offset ray origin.
        const float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
        const float distanceSquaredWorld = dot(offsetWorld, offsetWorld);
        const float cosEmitter =
            abs(dot(normalize(endpoint.GeometricNormalWorld), -offsetWorld * rsqrt(distanceSquaredWorld)));
        if (isfinite(distanceSquaredWorld) == false || distanceSquaredWorld <= 0.0f || cosEmitter <= 0.0f)
        {
            return float3(0);
        }
        misWeight = ComputeMisWeight(lightPdfArea, bsdfPdfSolidAngle, distanceSquaredWorld, cosEmitter, false);
    }
    return emission * bsdfValue * (cosSurface * misWeight / bsdfPdfSolidAngle);
}

float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    const uint status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    if (status == SCENE_QUERY_MISS)
    {
        return EvaluateEnvironment(scene, launch.CameraRay.DirectionWorld);
    }
    if (status != SCENE_QUERY_SURFACE)
    {
        return float3(0);
    }
    const float3 viewDirectionWorld = -normalize(launch.CameraRay.DirectionWorld);
    const float3 surfaceEmission = EvaluateEmission(scene, hit, viewDirectionWorld);
    const MaterialSample material = EvaluateMaterial(scene, hit);
    const float normalLengthSquared = dot(material.ShadingNormalWorld, material.ShadingNormalWorld);
    if (GetEmitterCount(scene) == 0 || material.IsValid == false || normalLengthSquared <= 0.0f)
    {
        return surfaceEmission;
    }
    const float3 shadingNormalWorld = material.ShadingNormalWorld * rsqrt(normalLengthSquared);
    const uint tileIndex = GetTileLocation(launch).Index;
    float3 radiance = surfaceEmission;
    radiance += SampleLightDirectLighting(scene,
                                          hit,
                                          material,
                                          shadingNormalWorld,
                                          viewDirectionWorld,
                                          tileIndex,
                                          inoutQueries);
    // Runs regardless of the light sample's visibility or contribution.
    radiance +=
        SampleBsdfDirectLighting(scene, hit, material, shadingNormalWorld, viewDirectionWorld, tileIndex, inoutQueries);
    if (all(isfinite(radiance)) == false || any(radiance < 0.0f) == true)
    {
        inoutQueries.HasFailed = true;
        return float3(0);
    }
    // Complementary MIS estimates are summed, not averaged.
    return radiance;
}
