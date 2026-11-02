#pragma once

// A bounded BSDF-only path integral over a uniform primary-sample cube. Thanks to
// Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based Rendering", 4e,
// section 13.3, https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/Path_Tracing .
// No NEE: mesh emission and environment are reached by BSDF continuations. Delta
// analytic lights and stochastic layered BSDFs are explicitly outside this profile.
static const uint MH_MAXIMUM_SCATTERING_DEPTH = LUDUS_MH_MAXIMUM_SCATTERING_DEPTH;
static const uint MH_CAMERA_DIMENSIONS = LUDUS_MH_CAMERA_DIMENSIONS;
static const uint MH_BSDF_DIMENSIONS = LUDUS_MH_BSDF_DIMENSIONS;
static const uint MH_PRIMARY_DIMENSIONS = MH_CAMERA_DIMENSIONS + MH_MAXIMUM_SCATTERING_DEPTH * MH_BSDF_DIMENSIONS;
static const float MH_MAXIMUM_DISTANCE_WORLD = 3.402823466e38f;
static const float3 MH_LUMINANCE_WEIGHTS = float3(0.2126f, 0.7152f, 0.0722f);

struct PrimarySamples
{
    float Values[MH_PRIMARY_DIMENSIONS];
};
struct MetropolisPathAovs
{
    float3 Emission;
    float3 Direct;
    float3 Indirect;
};
void AddMetropolisPathAov(float3 contribution, uint depth, inout MetropolisPathAovs inoutAovs)
{
    if (depth == 0)
        inoutAovs.Emission += contribution;
    else if (depth == 1)
        inoutAovs.Direct += contribution;
    else
        inoutAovs.Indirect += contribution;
}

Ray GenerateMetropolisCameraRay(RayLaunch launch, PrimarySamples primary)
{
    float2 positionUv =
        (float2(launch.PositionPixel) + float2(primary.Values[0], primary.Values[1])) / float2(launch.ExtentPixel);
    float4 nearPositionWorld = mul(launch.CameraClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 0, 1));
    float4 farPositionWorld = mul(launch.CameraClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 1, 1));
    nearPositionWorld /= nearPositionWorld.w;
    farPositionWorld /= farPositionWorld.w;
    Ray ray;
    ray.OriginWorld = launch.IsOrthographic == true ? nearPositionWorld.xyz : launch.CameraPositionWorld;
    ray.DirectionWorld = NormalizeOrZero(farPositionWorld.xyz - ray.OriginWorld);
    ray.MinDistanceWorld =
        launch.IsOrthographic == true ? 0 : dot(nearPositionWorld.xyz - ray.OriginWorld, ray.DirectionWorld);
    ray.MaxDistanceWorld = dot(farPositionWorld.xyz - ray.OriginWorld, ray.DirectionWorld);
    return ray;
}

float3 EvaluateMetropolisPath(SceneContext scene,
                              Ray ray,
                              PrimarySamples primary,
                              inout RayQueryContext queries,
                              out MetropolisPathAovs outAovs)
{
    outAovs = (MetropolisPathAovs)0;
    if (MH_SCATTERING_DEPTH == 0 || MH_SCATTERING_DEPTH > MH_MAXIMUM_SCATTERING_DEPTH)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
        return float3(0);
    }
    float3 radiance = float3(0);
    float3 throughput = float3(1);
    [loop] for (uint depth = 0; depth <= MH_SCATTERING_DEPTH; ++depth)
    {
        SurfaceHit hit;
        uint status = TraceClosest(scene, queries, ray, hit);
        if (status == SCENE_QUERY_MISS)
        {
            float3 contribution = throughput * EvaluateEnvironment(scene, ray.DirectionWorld);
            radiance += contribution;
            AddMetropolisPathAov(contribution, depth, outAovs);
            break;
        }
        if (status != SCENE_QUERY_SURFACE)
            break;
        float3 viewDirectionWorld = -ray.DirectionWorld;
        float3 contribution = throughput * EvaluateEmission(scene, hit, viewDirectionWorld);
        radiance += contribution;
        AddMetropolisPathAov(contribution, depth, outAovs);
        if (depth == MH_SCATTERING_DEPTH || dot(hit.GeometricNormalWorld, viewDirectionWorld) <= 0)
            break;
        MaterialSample material = EvaluateMaterial(scene, hit);
        if (material.IsValid == false || material.Model == LUDUS_MATERIAL_MODEL_COATED_DIFFUSE)
        {
            FailSceneQuery(queries,
                           material.IsValid == false ? LUDUS_SCENE_FAILURE_SCENE_DATA
                                                     : LUDUS_SCENE_FAILURE_PRIMARY_MATERIAL);
            break;
        }
        uint dimension = MH_CAMERA_DIMENSIONS + depth * MH_BSDF_DIMENSIONS;
        float3 sample = float3(primary.Values[dimension], primary.Values[dimension + 1], primary.Values[dimension + 2]);
        float3 directionWorld = SampleStandardBsdf(material, viewDirectionWorld, sample);
        if (all(isfinite(directionWorld)) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
            break;
        }
        if (dot(directionWorld, directionWorld) <= 0 || dot(hit.GeometricNormalWorld, directionWorld) <= 0)
            break;
        float3 value;
        float density;
        EvaluateStandardBsdf(material, viewDirectionWorld, directionWorld, value, density);
        if (density <= 0)
            break;
        throughput *= value * (max(0, dot(material.ShadingNormalWorld, directionWorld)) / density);
        ray = SpawnSurfaceRay(hit, directionWorld, MH_MAXIMUM_DISTANCE_WORLD);
    }
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    return radiance;
}

float3 EvaluateMetropolisPath(SceneContext scene, Ray ray, PrimarySamples primary, inout RayQueryContext queries)
{
    MetropolisPathAovs aovs;
    return EvaluateMetropolisPath(scene, ray, primary, queries, aovs);
}

float ComputeMetropolisTarget(float3 radiance)
{
    // Positive support includes black paths. The self-normalized numerator and
    // denominator estimate E_pi[L/p] / E_pi[1/p], avoiding a bootstrap brightness estimate.
    return MH_TARGET_FLOOR + dot(radiance, MH_LUMINANCE_WEIGHTS);
}
