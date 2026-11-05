#pragma once
#include "core.slang"

struct FgPoint
{
    SurfaceHit Hit;
    MaterialSample Material;
    float3 ViewDirectionWorld;
    float3 Throughput;
    uint Depth;
    bool IsValid;
};
bool IsFgDiffuse(MaterialSample material)
{
    return (material.Model != MaterialModel::Conductor && material.Metallic < 1) ||
           sqrt(ComputeMaterialAlpha(material.Model, material.Roughness, material.AlphaU)) >= FG_SPECULAR_ROUGHNESS;
}
[noinline] bool TryMakeFgPoint(SceneContext scene,
                               SurfaceHit hit,
                               float3 viewDirectionWorld,
                               inout RayQueryContext queries,
                               out FgPoint outPoint)
{
    outPoint = (FgPoint)0;
    outPoint.Hit = hit;
    outPoint.Material = EvaluateMaterial(scene, hit);
    outPoint.ViewDirectionWorld = viewDirectionWorld;
    outPoint.Throughput = float3(1);
    if (outPoint.Material.IsValid == false || outPoint.Material.Model == MaterialModel::CoatedDiffuse)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_PRIMARY_MATERIAL);
        return false;
    }
    outPoint.IsValid = true;
    return true;
}
[noinline] float3 EvaluateFgBsdf(FgPoint point, float3 incomingWorld)
{
    float3 value;
    float density;
    EvaluateStandardBsdf(point.Material, point.ViewDirectionWorld, incomingWorld, value, density);
    return value;
}
[noinline] bool TrySampleFgBsdf(FgPoint point, float3 samples, out float3 outDirectionWorld, out float3 outWeight)
{
    outDirectionWorld = SampleStandardBsdf(point.Material, point.ViewDirectionWorld, samples);
    float3 value;
    float density;
    EvaluateStandardBsdf(point.Material, point.ViewDirectionWorld, outDirectionWorld, value, density);
    float cosine = dot(point.Material.ShadingNormalWorld, outDirectionWorld);
    outWeight = density > 0 && cosine > 0 ? value * (cosine / density) : float3(0);
    return any(outWeight > 0) == true && all(isfinite(outWeight)) == true;
}
Ray MakeFgCameraRay(RayLaunch launch)
{
    float2 positionUv = (float2(launch.PositionPixel) + 0.5) / float2(launch.ExtentPixel);
    float4 nearH = mul(launch.CameraClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 0, 1));
    float4 farH = mul(launch.CameraClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 1, 1));
    float3 nearWorld = nearH.xyz / nearH.w;
    float3 farWorld = farH.xyz / farH.w;
    Ray ray = (Ray)0;
    ray.OriginWorld = launch.IsOrthographic == true ? nearWorld : launch.CameraPositionWorld;
    ray.DirectionWorld = normalize(farWorld - ray.OriginWorld);
    ray.MaxDistanceWorld = FG_MAXIMUM_DISTANCE_WORLD;
    return ray;
}
// Replay only the glossy camera prefix; diffuse reconnection happens in resample.slang.
// Scene/camera edits reset the estimator, so previous-geometry photon replay is unnecessary here.
// Inlined at each caller: out of line in the merged Metal kernel, this tracer corrupted its
// callers' live state on an Apple M5 (garbage failure counters and accumulated sample counts).
FgPoint TraceFgCamera(SceneContext scene,
                      RayLaunch launch,
                      uint4 randomKey,
                      inout RayQueryContext queries,
                      out float3 outEmission)
{
    outEmission = float3(0);
    float3 throughput = float3(1);
    Ray ray = MakeFgCameraRay(launch);
    FgPoint result = (FgPoint)0;
    [loop] for (uint depth = 0; depth <= FG_CAMERA_BOUNCES; ++depth)
    {
        SurfaceHit hit;
        SceneQuery status = TraceClosest(scene, queries, ray, hit);
        if (status == SceneQuery::Miss)
        {
            outEmission += throughput * EvaluateEnvironment(scene, ray.DirectionWorld);
            return result;
        }
        if (status != SceneQuery::Surface)
            return result;
        outEmission += throughput * EvaluateEmission(scene, hit, -ray.DirectionWorld);
        FgPoint point;
        if (TryMakeFgPoint(scene, hit, -ray.DirectionWorld, queries, point) == false)
            return result;
        if (IsFgDiffuse(point.Material) == true)
        {
            point.Depth = depth;
            point.Throughput = throughput;
            return point;
        }
        if (depth == FG_CAMERA_BOUNCES)
            return result;
        float3 directionWorld;
        float3 weight;
        if (TrySampleFgBsdf(point,
                            SampleFgRandomTriple(randomKey.xy,
                                                 randomKey.w,
                                                 randomKey.z,
                                                 FG_CAMERA_FIRST_DIMENSION + depth * FG_VERTEX_DIMENSIONS),
                            directionWorld,
                            weight) == false)
            return result;
        throughput *= weight;
        ray = SpawnSurfaceRay(hit, directionWorld, FG_MAXIMUM_DISTANCE_WORLD);
    }
    return result;
}

[noinline] float3 EstimateFgDirect(SceneContext scene, RayLaunch launch, FgPoint point, inout RayQueryContext queries)
{
    float3 radiance = float3(0);
    uint count = GetLightCount(scene);
    if (count > 0)
    {
        SelectedLight selected;
        selected.LightIndex = min(uint(SampleFgRandom(launch, FG_DIRECT_FIRST_DIMENSION) * float(count)), count - 1);
        selected.SelectionPmf = EvaluateLightSelectionPmf(scene, selected.LightIndex);
        selected.HasSample = true;
        RandomAddress address =
            MakeFgRandomAddress(GetFgPixel(launch), launch.SampleIndex, FG_DIRECT_FIRST_DIMENSION + 1);
        IncidentLightSample light =
            SampleIncidentLight(scene, selected.LightIndex, point.Hit, launch.Seed, address, queries);
        float3 value;
        float density;
        EvaluateStandardBsdf(point.Material, point.ViewDirectionWorld, light.DirectionWorld, value, density);
        float lightDensity = selected.SelectionPmf * light.ConditionalProbability;
        float cosine = dot(point.Material.ShadingNormalWorld, light.DirectionWorld);
        if (light.HasSample == true && lightDensity > 0 && cosine > 0 && any(value > 0) == true &&
            TestVisibility(scene, queries, point.Hit, light) == SceneQuery::Miss)
        {
            float weight =
                light.Measure == IncidentLightMeasure::Discrete ? 1 : ComputeFgPowerWeight(lightDensity, density);
            radiance += value * light.IncidentCoefficient * (cosine * weight / lightDensity);
        }
    }
    float3 directionWorld;
    float3 bsdfWeight;
    if (TrySampleFgBsdf(point,
                        SampleFgRandomTriple(launch.Seed,
                                             GetFgPixel(launch),
                                             launch.SampleIndex,
                                             FG_DIRECT_FIRST_DIMENSION + FG_VERTEX_DIMENSIONS + 1),
                        directionWorld,
                        bsdfWeight) == true)
    {
        SurfaceHit hit;
        SceneQuery status =
            TraceClosest(scene, queries, SpawnSurfaceRay(point.Hit, directionWorld, FG_MAXIMUM_DISTANCE_WORLD), hit);
        RayLightEvaluation light = EvaluateLightAlongRay(scene,
                                                        point.Hit.PositionWorld,
                                                        directionWorld,
                                                        status,
                                                        hit,
                                                        LightSelectionPolicy::Uniform);
        float3 emission = light.Radiance;
        float lightDensity = light.SelectionPmf * light.ConditionalPdfSolidAngle;
        float3 value;
        float density;
        EvaluateStandardBsdf(point.Material, point.ViewDirectionWorld, directionWorld, value, density);
        radiance += bsdfWeight * emission * ComputeFgPowerWeight(density, lightDensity);
    }
    return point.Throughput * radiance;
}
