// Public logical scene shader interface v1. Packed buffers and traversal remain Renderer-owned.
// Thanks to Pharr, Jakob and Humphreys, PBRT 4e (2023), sections 12.1/12.6/13.4:
// https://pbr-book.org/4ed/Light_Sources/Light_Interface
// separate discrete source choice from incident-light proposals and solid-angle PDFs.
// No integrator, MIS weighting or bounce decision is performed in this module.
#include "scene_data.h"
#include "scene_shader_layout.h"
static const uint SCENE_INTERFACE_VERSION = LUDUS_SCENE_SHADER_INTERFACE_VERSION;
static const uint SCENE_INVALID_INDEX = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
static const uint SCENE_QUERY_MISS = 0;
static const uint SCENE_QUERY_SURFACE = 1;
static const uint SCENE_QUERY_FAILURE = 2;
static const uint SCENE_LIGHT_DIRECTIONAL = 0;
static const uint SCENE_LIGHT_POINT = 1;
static const uint SCENE_LIGHT_SPOT = 2;
static const uint SCENE_LIGHT_MESH = 3;
static const uint SCENE_LIGHT_ENVIRONMENT = 4;
static const float SCENE_UNIT_TOLERANCE = 0.00001;
struct Ray
{
    float3 OriginWorld;
    float MinDistanceWorld;
    float3 DirectionWorld;
    float MaxDistanceWorld;
};
struct RayLaunch
{
    uint2 PositionPixel;
    uint2 ExtentPixel;
    Ray CameraRay;
    float4x4 CameraClipToWorld;
    float3 CameraPositionWorld;
    bool IsOrthographic;
    uint SampleIndex;
    uint2 Seed;
};
struct SceneContext
{
    uint Version;
    uint2 Generation;
};
struct RayQueryContext
{
    uint SurfaceQueries;
    uint VisibilityQueries;
    uint MaximumSurfaceQueries;
    uint MaximumVisibilityQueries;
    bool HasFailed;
};
struct SurfaceHit
{
    Ray IncomingRay;
    float3 PositionWorld;
    float DistanceWorld;
    float3 PositionErrorWorld;
    float3 GeometricNormalWorld;
    float3 ShadingNormalWorld;
    float4 TangentWorld;
    uint TriangleIndex;
    uint Primitive;
    uint InstanceIndex;
    uint MaterialIndex;
    uint SourceMaterialIndex;
    uint MaterialSlot;
    uint MaterialKind; // assigned=0, default=1, error=2
    uint EmitterIndex;
    uint2 SourceId;
    float2 Barycentric;
    float2 TexcoordUv;
    bool IsFrontFace;
    bool IsValid;
};
struct MaterialSample
{
    float3 BaseColor;
    float Metallic;
    float Roughness;
    float3 Emission;
    float3 ShadingNormalWorld;
    bool IsEmissionTwoSided;
    float4 TexcoordTransformUv;
    float NormalScaleTangent;
    bool HasColorMap;
    bool HasSurfaceMap;
    bool HasEmissionMap;
    bool IsValid;
};
struct LightInfo
{
    uint Kind;
    uint Index;
    uint MaterialSlot;
    uint2 SourceId;
    float3 Intensity;
    float3 PositionWorld;
    float3 DirectionWorld;
    float RangeWorld;
    float InnerCos;
    float OuterCos;
    bool IsValid;
};
struct SelectedLight
{
    uint LightIndex;
    float SelectionPmf;
    bool HasSample;
};
struct LightSample
{
    uint LightIndex;
    uint TriangleIndex;
    float2 Barycentric;
    float3 DirectionWorld;
    float3 IncidentCoefficient;
    float SelectionPmf;
    float ConditionalPdf;
    float3 PositionWorld;
    float DistanceWorld;
    bool IsDiscrete;
    bool HasFiniteEndpoint;
    bool HasSample;
};
bool HasSceneData(SceneContext scene, uint groups)
{
    return scene.Version == SCENE_INTERFACE_VERSION && (asuint(Options.w) & groups) == groups;
}
uint GetEmitterCount(SceneContext scene)
{
    return scene.Version == SCENE_INTERFACE_VERSION && (asuint(Options.w) & SCENE_DATA_LIGHTS) != 0
               ? asuint(Environment.w)
               : 0;
}
uint GetMaterialCount(SceneContext scene)
{
    return scene.Version == SCENE_INTERFACE_VERSION &&
                   (asuint(Options.w) & (SCENE_DATA_MATERIALS | SCENE_DATA_LIGHTS)) != 0
               ? asuint(Options.z)
               : 0;
}
uint GetLightCount(SceneContext scene)
{
    return (asuint(Options.w) & SCENE_DATA_LIGHTS) != 0
               ? Counts.w + GetEmitterCount(scene) +
                     ((SurfaceLayout.w != 0 || HasPositiveComponent(Environment.xyz)) ? 1u : 0u)
               : 0;
}
float4 GetEmitter(SceneContext scene, uint emitterIndex)
{
    return emitterIndex < GetEmitterCount(scene) ? SceneValue(Offsets.z + Counts.z + emitterIndex) : float4(0);
}
LightInfo GetLightInfo(SceneContext scene, uint lightIndex)
{
    LightInfo result = (LightInfo)0;
    result.Index = lightIndex;
    result.IsValid = lightIndex < GetLightCount(scene);
    if (result.IsValid == false)
        return result;
    if (lightIndex < Counts.w)
    {
        uint offset = SurfaceLayout.y + lightIndex * LUDUS_GPU_PREVIEW_LIGHT_WORDS;
        float4 intensityAndKind = SceneValue(offset);
        float4 positionAndRangeWorld = SceneValue(offset + 1);
        float4 directionAndConeWorld = SceneValue(offset + 2);
        result.Kind = uint(intensityAndKind.w);
        result.Intensity = intensityAndKind.xyz;
        result.PositionWorld = positionAndRangeWorld.xyz;
        result.RangeWorld = positionAndRangeWorld.w;
        result.DirectionWorld = directionAndConeWorld.xyz;
        result.InnerCos = SceneValue(offset + 3).x;
        result.OuterCos = directionAndConeWorld.w;
        result.SourceId = asuint(SceneValue(offset + 3).yz);
    }
    else if (lightIndex - Counts.w < GetEmitterCount(scene))
    {
        result.Kind = SCENE_LIGHT_MESH;
        float4 emitter = GetEmitter(scene, lightIndex - Counts.w);
        uint triangle = asuint(SceneValue(Offsets.z + uint(emitter.x)).x);
        result.SourceId = asuint(SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 7).zw);
        result.MaterialSlot = asuint(SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).w);
    }
    else
    {
        result.Kind = SCENE_LIGHT_ENVIRONMENT;
        result.Intensity = Environment.xyz;
    }
    return result;
}
MaterialSample GetMaterial(SceneContext scene, uint materialIndex)
{
    MaterialSample result = (MaterialSample)0;
    result.IsValid = materialIndex < GetMaterialCount(scene);
    if (result.IsValid == false)
        return result;
    uint base = Offsets.w + materialIndex * SurfaceLayout.x;
    result.BaseColor = SceneValue(base).xyz;
    result.Metallic = SceneValue(base).w;
    result.Roughness = SceneValue(base + 1).x;
    result.Emission = SceneValue(base + 4).xyz;
    result.IsEmissionTwoSided = asuint(SceneValue(base + 5).w) != 0;
    result.TexcoordTransformUv = SceneValue(base + 3);
    result.NormalScaleTangent = SceneValue(base + 6).x;
    result.HasColorMap = asuint(SceneValue(base + 4).w) != 0;
    result.HasSurfaceMap = asuint(SceneValue(base + 6).w) != 0;
    result.HasEmissionMap = asuint(SceneValue(base + 8).z) != 0;
    return result;
}
MaterialSample EvaluateMaterial(SceneContext scene, SurfaceHit hit)
{
    MaterialSample result = GetMaterial(scene, hit.MaterialIndex);
    if (hit.IsValid == false || hit.TriangleIndex >= Counts.x || result.IsValid == false)
    {
        result.IsValid = false;
        return result;
    }
    if (hit.MaterialKind == 2)
    {
        result.ShadingNormalWorld = hit.GeometricNormalWorld;
        return result;
    }
    uint triangle = hit.TriangleIndex;
    result.BaseColor *= Texture(triangle, hit.TexcoordUv).xyz * InterpolateTriangle(triangle, 13, hit.Barycentric).xyz;
    result.Emission *= Texture(triangle, hit.TexcoordUv, 2).xyz;
    result.ShadingNormalWorld = hit.ShadingNormalWorld;
    if (asuint(Surface(triangle, 6).w) != 0)
    {
        SurfaceParameters parameters = DecodeSurfaceParameters(Texture(triangle, hit.TexcoordUv, 1),
                                                               result.Metallic,
                                                               result.Roughness,
                                                               Surface(triangle, 6).x);
        result.Metallic = parameters.Metallic;
        result.Roughness = parameters.Roughness;
        uint base = triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
        float2 scale = Surface(triangle, 3).xy;
        if (abs(hit.TangentWorld.w) > 0.5)
        {
            float3 tangentWorld = NormalizeOrZero(
                hit.TangentWorld.xyz - hit.ShadingNormalWorld * dot(hit.ShadingNormalWorld, hit.TangentWorld.xyz));
            result.ShadingNormalWorld =
                NormalizeOrZero(tangentWorld * parameters.NormalTangent.x +
                                cross(hit.ShadingNormalWorld, tangentWorld) * (hit.TangentWorld.w < 0 ? -1 : 1) *
                                    parameters.NormalTangent.y +
                                hit.ShadingNormalWorld * parameters.NormalTangent.z);
        }
        else
        {
            result.ShadingNormalWorld = ApplyNormalWorld(hit.ShadingNormalWorld,
                                                         parameters.NormalTangent,
                                                         SceneValue(base + 1).xyz - SceneValue(base).xyz,
                                                         SceneValue(base + 2).xyz - SceneValue(base).xyz,
                                                         (SceneValue(base + 6).zw - SceneValue(base + 6).xy) * scale,
                                                         (SceneValue(base + 7).xy - SceneValue(base + 6).xy) * scale);
        }
    }
    return result;
}
float3 EvaluateEmission(SceneContext scene, SurfaceHit hit, float3 directionAwayWorld)
{
    MaterialSample material = EvaluateMaterial(scene, hit);
    if (material.IsValid == false ||
        (material.IsEmissionTwoSided == false && dot(hit.GeometricNormalWorld, directionAwayWorld) <= 0))
        return float3(0);
    return material.Emission;
}
float3 EvaluateEnvironment(SceneContext scene, float3 directionWorld)
{
    return EvaluateEnvironmentWorld(directionWorld).xyz;
}
bool IsSceneRayValid(Ray ray)
{
    return all(isfinite(ray.OriginWorld)) && all(isfinite(ray.DirectionWorld)) && isfinite(ray.MinDistanceWorld) &&
           isfinite(ray.MaxDistanceWorld) && ray.MinDistanceWorld >= 0 &&
           ray.MaxDistanceWorld >= ray.MinDistanceWorld &&
           abs(dot(ray.DirectionWorld, ray.DirectionWorld) - 1) <= SCENE_UNIT_TOLERANCE;
}
uint TraceClosest(SceneContext scene, inout RayQueryContext queries, Ray ray, out SurfaceHit outHit)
{
    outHit = (SurfaceHit)0;
    if (queries.HasFailed == true || scene.Version != SCENE_INTERFACE_VERSION ||
        queries.SurfaceQueries >= queries.MaximumSurfaceQueries || IsSceneRayValid(ray) == false)
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    queries.SurfaceQueries += 1;
    Hit hit = Trace(ray.OriginWorld, ray.DirectionWorld, ray.MinDistanceWorld, ray.MaxDistanceWorld);
    if (hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    if (hit.Triangle == SCENE_INVALID_INDEX)
        return SCENE_QUERY_MISS;
    uint base = hit.Triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    outHit.IncomingRay = ray;
    outHit.TriangleIndex = hit.Triangle;
    outHit.Primitive = asuint(SceneValue(base + 2).w);
    outHit.InstanceIndex = asuint(SceneValue(base + 3).w);
    outHit.MaterialIndex = asuint(SceneValue(base).w);
    outHit.MaterialSlot = asuint(SceneValue(base + 8).w);
    outHit.SourceMaterialIndex = asuint(SceneValue(base + 4).w);
    outHit.MaterialKind = asuint(SceneValue(base + 5).w);
    outHit.SourceId = asuint(SceneValue(base + 7).zw);
    outHit.EmitterIndex = asuint(SceneValue(base + 9).w);
    outHit.PositionWorld = PositionWorld(hit.Triangle, hit.Barycentric);
    outHit.DistanceWorld = hit.DistanceWorld;
    outHit.PositionErrorWorld = SceneValue(base + 9).xyz;
    outHit.GeometricNormalWorld = SceneValue(base + 8).xyz;
    outHit.ShadingNormalWorld =
        NormalizeOrZero(SceneValue(base + 3).xyz * (1 - hit.Barycentric.x - hit.Barycentric.y) +
                        SceneValue(base + 4).xyz * hit.Barycentric.x + SceneValue(base + 5).xyz * hit.Barycentric.y);
    outHit.TangentWorld = InterpolateTriangle(hit.Triangle, 10, hit.Barycentric);
    outHit.Barycentric = hit.Barycentric;
    outHit.TexcoordUv = TexcoordUv(hit.Triangle, hit.Barycentric);
    outHit.IsFrontFace = dot(outHit.GeometricNormalWorld, -ray.DirectionWorld) > 0;
    outHit.IsValid = true;
    return SCENE_QUERY_SURFACE;
}
Ray SpawnSurfaceRay(SurfaceHit hit, float3 directionWorld, float maximumDistanceWorld)
{
    Ray ray;
    ray.OriginWorld = hit.TriangleIndex < Counts.x
                          ? OffsetPositionWorld(hit.PositionWorld, hit.TriangleIndex, directionWorld)
                          : hit.PositionWorld;
    ray.MinDistanceWorld = 0;
    ray.DirectionWorld = directionWorld;
    ray.MaxDistanceWorld = maximumDistanceWorld;
    return ray;
}
float EvaluateLightSelectionPmf(SceneContext scene, uint lightIndex)
{
    uint count = GetLightCount(scene);
    return lightIndex < count ? 1.0 / float(count) : 0;
}
SelectedLight SelectLight(SceneContext scene, RayLaunch launch, uint dimension)
{
    SelectedLight result = (SelectedLight)0;
    uint count = GetLightCount(scene);
    if (count == 0)
        return result;
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    result.HasSample = TrySampleRandomBounded(launch.Seed,
                                              PixelRandomAddress(pixel, launch.SampleIndex, dimension),
                                              count,
                                              result.LightIndex);
    result.SelectionPmf = result.HasSample == true ? 1.0 / float(count) : 0;
    return result;
}
float EvaluateLightPdfSolidAngle(SceneContext scene, float3 receiverPositionWorld, SurfaceHit endpoint)
{
    if (endpoint.EmitterIndex >= GetEmitterCount(scene))
        return 0;
    float4 emitter = GetEmitter(scene, endpoint.EmitterIndex);
    float3 offsetWorld = endpoint.PositionWorld - receiverPositionWorld;
    float cosine = abs(dot(endpoint.GeometricNormalWorld, -NormalizeOrZero(offsetWorld)));
    return cosine > 0 && emitter.z > 0
               ? dot(offsetWorld, offsetWorld) / (float(GetLightCount(scene)) * emitter.z * cosine)
               : 0;
}
LightSample SampleIncidentLight(SceneContext scene, SelectedLight selected, SurfaceHit receiver, float3 shapeSample)
{
    LightSample result = (LightSample)0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    if (selected.HasSample == false || selected.LightIndex >= GetLightCount(scene) || any(shapeSample < 0) ||
        any(shapeSample >= 1) || any(isfinite(shapeSample) == false))
        return result;
    result.LightIndex = selected.LightIndex;
    result.SelectionPmf = EvaluateLightSelectionPmf(scene, selected.LightIndex);
    LightInfo light = GetLightInfo(scene, selected.LightIndex);
    result.ConditionalPdf = 1;
    if (light.Kind == SCENE_LIGHT_MESH)
    {
        float4 emitter = GetEmitter(scene, selected.LightIndex - Counts.w);
        float targetArea = shapeSample.x * emitter.z;
        uint triangle = SCENE_INVALID_INDEX;
        for (uint i = 0; i < uint(emitter.y); ++i)
        {
            triangle = asuint(SceneValue(Offsets.z + uint(emitter.x) + i).x);
            targetArea -= SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 1).w;
            if (targetArea <= 0)
                break;
        }
        if (triangle == SCENE_INVALID_INDEX)
            return result;
        float root = sqrt(shapeSample.y);
        result.Barycentric = float2(root * (1 - shapeSample.z), root * shapeSample.z);
        result.TriangleIndex = triangle;
        result.PositionWorld = PositionWorld(triangle, result.Barycentric);
        float3 offsetWorld = result.PositionWorld - receiver.PositionWorld;
        result.DistanceWorld = length(offsetWorld);
        if (result.DistanceWorld <= 0)
            return result;
        result.DirectionWorld = offsetWorld / result.DistanceWorld;
        float3 normalWorld = SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).xyz;
        float cosine = abs(dot(normalWorld, -result.DirectionWorld));
        if (cosine <= 0)
            return result;
        result.ConditionalPdf = dot(offsetWorld, offsetWorld) / (emitter.z * cosine);
        result.IncidentCoefficient =
            Surface(triangle, 4).xyz * Texture(triangle, TexcoordUv(triangle, result.Barycentric), 2).xyz;
        if (asuint(Surface(triangle, 5).w) == 0 && dot(normalWorld, -result.DirectionWorld) <= 0)
            result.IncidentCoefficient = 0;
        result.HasFiniteEndpoint = true;
    }
    else if (light.Kind == SCENE_LIGHT_ENVIRONMENT)
    {
        float cosine = 1 - 2 * shapeSample.y;
        float radius = sqrt(max(0, 1 - cosine * cosine));
        float longitude = 2 * PI * shapeSample.z;
        result.DirectionWorld = float3(radius * cos(longitude), cosine, radius * sin(longitude));
        result.IncidentCoefficient = EvaluateEnvironmentWorld(result.DirectionWorld).xyz;
        result.ConditionalPdf = 1 / (4 * PI);
    }
    else
    {
        result.IsDiscrete = true;
        result.DirectionWorld = -light.DirectionWorld;
        result.IncidentCoefficient = light.Intensity;
        if (light.Kind != SCENE_LIGHT_DIRECTIONAL)
        {
            result.PositionWorld = light.PositionWorld;
            float3 offsetWorld = light.PositionWorld - receiver.PositionWorld;
            float distanceSquared = dot(offsetWorld, offsetWorld);
            if (distanceSquared <= 0)
                return result;
            result.DistanceWorld = sqrt(distanceSquared);
            result.DirectionWorld = offsetWorld / result.DistanceWorld;
            result.HasFiniteEndpoint = true;
            float ratio = result.DistanceWorld / light.RangeWorld;
            float window = max(1 - ratio * ratio * ratio * ratio, 0);
            result.IncidentCoefficient *= window * window / max(distanceSquared, 0.0001);
            if (light.Kind == SCENE_LIGHT_SPOT)
            {
                float cone = saturate((dot(-result.DirectionWorld, light.DirectionWorld) - light.OuterCos) /
                                      (light.InnerCos - light.OuterCos));
                result.IncidentCoefficient *= cone * cone;
            }
        }
    }
    result.HasSample = true;
    return result;
}
uint TestVisibility(SceneContext scene, inout RayQueryContext queries, SurfaceHit receiver, LightSample light)
{
    if (queries.HasFailed == true || light.HasSample == false ||
        queries.VisibilityQueries >= queries.MaximumVisibilityQueries)
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    queries.VisibilityQueries += 1;
    Ray shadow = SpawnSurfaceRay(receiver, light.DirectionWorld, 0);
    if (light.HasFiniteEndpoint == true)
    {
        float3 endpointWorld = light.PositionWorld;
        if (light.TriangleIndex != SCENE_INVALID_INDEX)
            endpointWorld = OffsetPositionWorld(endpointWorld, light.TriangleIndex, -light.DirectionWorld);
        float3 offsetWorld = endpointWorld - shadow.OriginWorld;
        shadow.DirectionWorld = NormalizeOrZero(offsetWorld);
        shadow.MaxDistanceWorld = length(offsetWorld);
        if (shadow.MaxDistanceWorld > 0)
            shadow.MaxDistanceWorld = asfloat(asuint(shadow.MaxDistanceWorld) - 1);
    }
    else
    {
        for (uint corner = 0; corner < 8; ++corner)
        {
            float3 cornerWorld = float3((corner & 1) != 0 ? BoundsMaximumWorld.x : BoundsMinimumWorld.x,
                                        (corner & 2) != 0 ? BoundsMaximumWorld.y : BoundsMinimumWorld.y,
                                        (corner & 4) != 0 ? BoundsMaximumWorld.z : BoundsMinimumWorld.z);
            shadow.MaxDistanceWorld = max(shadow.MaxDistanceWorld, length(cornerWorld - shadow.OriginWorld));
        }
        shadow.MaxDistanceWorld = shadow.MaxDistanceWorld * 1.000001 + 1;
    }
    if (IsSceneRayValid(shadow) == false)
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    Hit hit = Trace(shadow.OriginWorld, shadow.DirectionWorld, 0, shadow.MaxDistanceWorld, true);
    if (hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    return hit.Triangle == SCENE_INVALID_INDEX ? SCENE_QUERY_MISS : SCENE_QUERY_SURFACE;
}
// Raw geometry access for authors implementing a different light proposal.
struct TriangleData
{
    uint TriangleIndex;
    float3 Position0World;
    float3 Position1World;
    float3 Position2World;
    float3 GeometricNormalWorld;
    float AreaWorld;
    float2 Texcoord0Uv;
    float2 Texcoord1Uv;
    float2 Texcoord2Uv;
    uint MaterialIndex;
    uint Primitive;
    bool IsValid;
};
TriangleData GetTriangle(SceneContext scene, uint triangleIndex)
{
    TriangleData result = (TriangleData)0;
    result.IsValid = scene.Version == SCENE_INTERFACE_VERSION && triangleIndex < Counts.x;
    if (result.IsValid == false)
        return result;
    result.TriangleIndex = triangleIndex;
    uint base = triangleIndex * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    result.Position0World = SceneValue(base).xyz;
    result.Position1World = SceneValue(base + 1).xyz;
    result.Position2World = SceneValue(base + 2).xyz;
    result.GeometricNormalWorld = SceneValue(base + 8).xyz;
    result.AreaWorld = SceneValue(base + 1).w;
    result.Texcoord0Uv = SceneValue(base + 6).xy;
    result.Texcoord1Uv = SceneValue(base + 6).zw;
    result.Texcoord2Uv = SceneValue(base + 7).xy;
    result.MaterialIndex = asuint(SceneValue(base).w);
    result.Primitive = asuint(SceneValue(base + 2).w);
    return result;
}
TriangleData GetEmitterTriangle(SceneContext scene, uint emitterIndex, uint primitiveInEmitter)
{
    float4 emitter = GetEmitter(scene, emitterIndex);
    if (emitterIndex >= GetEmitterCount(scene) || primitiveInEmitter >= uint(emitter.y))
        return (TriangleData)0;
    uint triangle = asuint(SceneValue(Offsets.z + uint(emitter.x) + primitiveInEmitter).x);
    return GetTriangle(scene, triangle);
}
float EvaluateEnvironmentPdfSolidAngle(SceneContext scene, float3 directionWorld)
{
    uint index = Counts.w + GetEmitterCount(scene);
    return EvaluateLightSelectionPmf(scene, index) / (4 * PI);
}
float SampleSceneRandom(RayLaunch launch, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    return SampleRandomFloat01(launch.Seed, PixelRandomAddress(pixel, launch.SampleIndex, dimension));
}
void EvaluateStandardBsdf(MaterialSample material,
                          float3 viewDirectionWorld,
                          float3 lightDirectionWorld,
                          out float3 outValue,
                          out float outPdf)
{
    Evaluate(material.BaseColor,
             material.Metallic,
             material.Roughness,
             material.ShadingNormalWorld,
             viewDirectionWorld,
             lightDirectionWorld,
             outValue,
             outPdf);
}
float3 SampleStandardBsdf(MaterialSample material, float3 viewDirectionWorld, float3 samples)
{
    return SampleBsdf(material.ShadingNormalWorld,
                      viewDirectionWorld,
                      material.Roughness,
                      material.Metallic == 1 ? 1 : 0.5,
                      samples);
}

// Construct a surface endpoint without issuing a ray. IncomingRay/Distance are zero.
SurfaceHit GetSurfaceHit(SceneContext scene, uint triangleIndex, float2 barycentric)
{
    SurfaceHit result = (SurfaceHit)0;
    if (scene.Version != SCENE_INTERFACE_VERSION || triangleIndex >= Counts.x || any(isfinite(barycentric) == false) ||
        any(barycentric < 0) || barycentric.x + barycentric.y > 1)
        return result;
    uint base = triangleIndex * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    result.TriangleIndex = triangleIndex;
    result.Primitive = asuint(SceneValue(base + 2).w);
    result.InstanceIndex = asuint(SceneValue(base + 3).w);
    result.MaterialIndex = asuint(SceneValue(base).w);
    result.MaterialSlot = asuint(SceneValue(base + 8).w);
    result.SourceMaterialIndex = asuint(SceneValue(base + 4).w);
    result.MaterialKind = asuint(SceneValue(base + 5).w);
    result.SourceId = asuint(SceneValue(base + 7).zw);
    result.EmitterIndex = asuint(SceneValue(base + 9).w);
    result.PositionWorld = PositionWorld(triangleIndex, barycentric);
    result.PositionErrorWorld = SceneValue(base + 9).xyz;
    result.GeometricNormalWorld = SceneValue(base + 8).xyz;
    result.ShadingNormalWorld =
        NormalizeOrZero(SceneValue(base + 3).xyz * (1 - barycentric.x - barycentric.y) +
                        SceneValue(base + 4).xyz * barycentric.x + SceneValue(base + 5).xyz * barycentric.y);
    result.TangentWorld = InterpolateTriangle(triangleIndex, 10, barycentric);
    result.Barycentric = barycentric;
    result.TexcoordUv = TexcoordUv(triangleIndex, barycentric);
    result.IsValid = true;
    return result;
}

struct SceneInfo
{
    uint2 Generation;
    uint Version;
    uint TriangleCount;
    uint MaterialCount;
    uint LightCount;
    uint EmitterCount;
};
SceneInfo GetSceneInfo(SceneContext scene)
{
    SceneInfo result;
    result.Generation = scene.Generation;
    result.Version = scene.Version;
    result.TriangleCount = Counts.x;
    result.MaterialCount = GetMaterialCount(scene);
    result.LightCount = GetLightCount(scene);
    result.EmitterCount = GetEmitterCount(scene);
    return result;
}
Ray MakeCameraRay(RayLaunch launch, float2 samplePixel)
{
    float2 positionUv = (float2(launch.PositionPixel) + samplePixel) / float2(launch.ExtentPixel);
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

float4 SampleMaterialTexture(SceneContext scene, SurfaceHit hit, float2 texcoordUv, uint mapIndex)
{
    if (scene.Version != SCENE_INTERFACE_VERSION || hit.IsValid == false || hit.TriangleIndex >= Counts.x ||
        mapIndex > 2)
        return float4(-1);
    return Texture(hit.TriangleIndex, texcoordUv, mapIndex);
}
