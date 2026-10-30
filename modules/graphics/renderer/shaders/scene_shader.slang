#include "coated_diffuse.slang"
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
/// Maximum visible intervals in one FindVisibleLineIntervals result (scene_shader_layout.h).
static const uint SCENE_LINE_INTERVAL_CAPACITY = LUDUS_SCENE_LINE_INTERVAL_CAPACITY;
/// Line interval merge and drop tolerance, a unitless fraction of the segment (scene_shader_layout.h).
static const float SCENE_LINE_INTERVAL_MERGE_TOLERANCE = LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE;
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
/// Renderer-initialized invocation state; forward the same value through all callbacks.
/// Query budgets and automatic sampling advance together; do not reset or copy this state.
struct RayQueryContext
{
    uint SurfaceQueries;
    uint VisibilityQueries;
    uint LineQueries; ///< FindVisibleLineIntervals calls made by this invocation.
    uint MaximumSurfaceQueries;
    uint MaximumVisibilityQueries;
    uint MaximumLineQueries; ///< The scene interface's line_queries budget; zero when absent.
    bool HasFailed;
    uint2 RandomSeed;         ///< Renderer-initialized sampling key; forward unchanged between callbacks.
    uint RandomPixel;         ///< Renderer-initialized linear pixel identity.
    uint RandomSampleIndex;   ///< Renderer-initialized progressive sample identity.
    uint NextRandomDimension; ///< Next automatic draw; shared across callbacks for this invocation.
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
    uint Model;
    float3 Eta;
    float3 K;
    float AlphaU;
    float AlphaV;
    float CoatingEta;
    float CoatingThickness;
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
/// World bounds of one mesh emitter's positive-area triangles, precomputed with the scene.
/// One importance weight per emitter can use them instead of iterating GetEmitterTriangle.
struct EmitterBounds
{
    /// Centred on the vertices' bounding box; encloses every vertex up to float32 rounding.
    float3 CenterWorld;
    float RadiusWorld;
    /// Unit area-weighted mean geometric normal; +Z when the normals cancel.
    float3 NormalAxisWorld;
    /// Every geometric normal lies within acos(CosConeHalfAngle) of the axis; -1 covers all directions.
    float CosConeHalfAngle;
    /// The emitter's material emits from both faces, as MaterialData.IsEmissionTwoSided.
    bool IsEmissionTwoSided;
    /// False when emitterIndex is not below GetEmitterCount; every field is then zero.
    bool IsValid;
};
/// Read the bounds of mesh emitter emitterIndex (the GetEmitter index).
/// Constant for one scene source generation; an index outside GetEmitterCount returns IsValid=false.
EmitterBounds GetEmitterBounds(SceneContext scene, uint emitterIndex)
{
    EmitterBounds result = (EmitterBounds)0;
    const uint emitterCount = GetEmitterCount(scene);
    if (emitterIndex >= emitterCount)
        return result;
    const uint offset = Offsets.z + Counts.z + emitterCount + emitterIndex * LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_WORDS;
    const float4 sphereWorld = SceneValue(offset);
    const float4 normalConeWorld = SceneValue(offset + 1);
    result.CenterWorld = sphereWorld.xyz;
    result.RadiusWorld = sphereWorld.w;
    result.NormalAxisWorld = normalConeWorld.xyz;
    result.CosConeHalfAngle = normalConeWorld.w;
    result.IsEmissionTwoSided = (asuint(SceneValue(offset + 2).x) & LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_TWO_SIDED) != 0;
    result.IsValid = true;
    return result;
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
    float4 etaAndModel = SceneValue(base + LUDUS_GPU_PREVIEW_SCATTERING_ETA_WORD);
    float4 extinctionAndAlpha = SceneValue(base + LUDUS_GPU_PREVIEW_SCATTERING_K_WORD);
    result.Model = asuint(etaAndModel.w);
    result.Eta = etaAndModel.xyz;
    result.K = extinctionAndAlpha.xyz;
    result.AlphaU = extinctionAndAlpha.w;
    result.AlphaV = SceneValue(base + LUDUS_GPU_PREVIEW_SCATTERING_ALPHA_WORD).x;
    result.CoatingEta = SceneValue(base + LUDUS_GPU_PREVIEW_SCATTERING_ALPHA_WORD).y;
    result.CoatingThickness = SceneValue(base + LUDUS_GPU_PREVIEW_SCATTERING_ALPHA_WORD).z;
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
// Internal stream advancement shared by uniform draws and bounded source selection.
static bool TryAdvanceSceneRandomDimension(inout RayQueryContext queries, out uint outDimension)
{
    outDimension = 0;
    if (queries.HasFailed == true)
        return false;
    if (queries.NextRandomDimension > RANDOM_MAX_DIMENSION)
    {
        queries.HasFailed = true;
        return false;
    }
    outDimension = queries.NextRandomDimension++;
    return true;
}
/// Select a uniform scene source using the renderer-initialized invocation stream.
/// Consumes one dimension for a nonempty source set. Empty sets return HasSample=false.
/// Stream exhaustion or bounded-mapping failure latches queries.HasFailed.
SelectedLight SelectLight(SceneContext scene, inout RayQueryContext queries)
{
    SelectedLight result = (SelectedLight)0;
    uint count = GetLightCount(scene);
    if (count == 0)
        return result;
    uint dimension;
    if (TryAdvanceSceneRandomDimension(queries, dimension) == false)
        return result;
    result.HasSample =
        TrySampleRandomBounded(queries.RandomSeed,
                               PixelRandomAddress(queries.RandomPixel, queries.RandomSampleIndex, dimension),
                               count,
                               result.LightIndex);
    if (result.HasSample == false)
        queries.HasFailed = true;
    result.SelectionPmf = result.HasSample == true ? 1.0 / float(count) : 0;
    return result;
}
/// Select a uniform scene source at an explicit random address, without advancing queries.
/// Empty sets or invalid/exhausted bounded addresses return HasSample=false.
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
/// One emissive triangle drawn from the renderer's power-weighted triangle CDF.
struct EmissiveTriangleSelection
{
    uint TriangleIndex; ///< Scene triangle index; SCENE_INVALID_INDEX without a sample.
    float SelectionPmf; ///< Discrete probability of TriangleIndex among emissive triangles.
    bool HasSample;
};
// The scene's power CDF over emissive triangles, which RT1 also reads. Every triangle
// with positive area and emitted luminance keeps a nonzero probability.
static bool HasEmissiveTriangleCdf(SceneContext scene)
{
    return scene.Version == SCENE_INTERFACE_VERSION && (asuint(Options.w) & SCENE_DATA_LIGHTS) != 0 && Counts.z != 0 &&
           EmitterCumulative(Counts.z - 1) != 0;
}
/// Select an emissive triangle in proportion to emitted luminance times area, at an explicit
/// random address and without advancing queries. Consumes one bounded draw over the CDF's integer
/// total, as RT1 does. Scenes without lights or emitted power, and invalid/exhausted bounded
/// addresses, return HasSample=false. SelectionPmf equals EvaluateEmissiveTriangleSelectionPmf.
EmissiveTriangleSelection SelectEmissiveTriangle(SceneContext scene, RayLaunch launch, uint dimension)
{
    EmissiveTriangleSelection result = (EmissiveTriangleSelection)0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    if (HasEmissiveTriangleCdf(scene) == false)
        return result;
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    uint value;
    if (TrySampleRandomBounded(launch.Seed,
                               PixelRandomAddress(pixel, launch.SampleIndex, dimension),
                               EmitterCumulative(Counts.z - 1),
                               value) == false)
        return result;
    uint emitter = SelectEmitter(value);
    result.TriangleIndex = sceneWords[Offsets.z + emitter].x;
    result.SelectionPmf = EmitterProbability(emitter);
    result.HasSample = true;
    return result;
}
/// Probability that SelectEmissiveTriangle returns triangleIndex: zero for triangles without
/// emitted power, out-of-range indices and scenes without lights.
float EvaluateEmissiveTriangleSelectionPmf(SceneContext scene, uint triangleIndex)
{
    uint emitter;
    if (HasEmissiveTriangleCdf(scene) == false || FindEmitter(triangleIndex, emitter) == false)
        return 0;
    return EmitterProbability(emitter);
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
/// A world-space segment whose visibility from a receiver is queried, usually a chord of an emitter
/// triangle. TriangleIndex is the packed index of the triangle that carries the segment, which is
/// excluded from the occluders, or SCENE_INVALID_INDEX for a segment that no scene triangle carries.
struct LineSegment
{
    float3 StartWorld;
    float3 EndWorld;
    uint TriangleIndex;
};
/// Visible parts of a LineSegment. Intervals[0, IntervalCount) are sorted, disjoint and nonempty
/// [start, end) segment parameters in [0, 1], from StartWorld (0) to EndWorld (1); later entries are
/// zero. VisibleLengthWorld is their total length in metres. HasMaskedOccluders reports an
/// alpha-tested triangle across the path: such triangles are never clipped, so a point drawn from
/// these intervals must be confirmed with TestVisibility. It is always false in an opaque scene.
struct LineVisibility
{
    float2 Intervals[SCENE_LINE_INTERVAL_CAPACITY];
    uint IntervalCount;
    float VisibleLengthWorld;
    bool HasMaskedOccluders;
};
/// Zero a LineVisibility field by field, as FindVisibleLineIntervals does with its output.
/// Prefer it to assigning (LineVisibility)0: on Metal (Apple M5, macOS 26.6) that whole-struct zero
/// assignment followed by indexed interval stores lost the stores in large merged kernels.
void ClearLineVisibility(out LineVisibility outVisibility)
{
    for (uint i = 0; i < SCENE_LINE_INTERVAL_CAPACITY; ++i)
        outVisibility.Intervals[i] = float2(0, 0);
    outVisibility.IntervalCount = 0;
    outVisibility.VisibleLengthWorld = 0;
    outVisibility.HasMaskedOccluders = false;
}
#include "ray_line_visibility.slang"
/// Find which parts of a segment the receiver sees past opaque scene triangles, consuming one line
/// query. The apex is the receiver offset as SpawnSurfaceRay offsets it toward the segment midpoint,
/// and the endpoints are offset toward the receiver on segment.TriangleIndex as TestVisibility offsets
/// a light endpoint. The receiver and carrying triangles are excluded; coplanar occluders, occluders
/// behind the receiver and occluders beyond the segment do not occlude. Occluded intervals within
/// SCENE_LINE_INTERVAL_MERGE_TOLERANCE merge, and shorter visible intervals are dropped. Every pass
/// walks the packed BVH in software, whatever its traversal strategy, or scans every packed triangle
/// when scene_interface.line_traversal is "TriangleLoop"; both give the same intervals. The query
/// draws no random dimensions. HasMaskedOccluders is false when no length is visible.
/// Returns SCENE_QUERY_MISS when no opaque occluder removes any length, SCENE_QUERY_SURFACE when one
/// does (a fully occluded segment has zero intervals), and SCENE_QUERY_FAILURE for an exhausted line
/// budget, an earlier failure, an invalid receiver or segment triangle, nonfinite positions, a segment
/// shorter than SCENE_UNIT_TOLERANCE, a receiver in the segment's line, a nonfinite interval, more
/// than SCENE_LINE_INTERVAL_CAPACITY - 1 running occluded intervals or an exhausted BVH stack.
/// Failure latches queries.HasFailed and zeroes outVisibility.
uint FindVisibleLineIntervals(SceneContext scene,
                              inout RayQueryContext queries,
                              SurfaceHit receiver,
                              LineSegment segment,
                              out LineVisibility outVisibility)
{
    ClearLineVisibility(outVisibility);
    if (queries.HasFailed == true || scene.Version != SCENE_INTERFACE_VERSION ||
        queries.LineQueries >= queries.MaximumLineQueries || receiver.IsValid == false ||
        (segment.TriangleIndex != SCENE_INVALID_INDEX && segment.TriangleIndex >= Counts.x))
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    queries.LineQueries += 1;
    float3 segmentWorld = segment.EndWorld - segment.StartWorld;
    float segmentLengthWorld = length(segmentWorld);
    if (all(isfinite(receiver.PositionWorld)) == false || all(isfinite(segment.StartWorld)) == false ||
        all(isfinite(segment.EndWorld)) == false || isfinite(segmentLengthWorld) == false ||
        segmentLengthWorld < SCENE_UNIT_TOLERANCE)
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    float3 middleWorld = (segment.StartWorld + segment.EndWorld) * 0.5;
    float3 apexWorld = SpawnSurfaceRay(receiver, NormalizeOrZero(middleWorld - receiver.PositionWorld), 0).OriginWorld;
    float3 startWorld = segment.StartWorld;
    float3 endWorld = segment.EndWorld;
    if (segment.TriangleIndex != SCENE_INVALID_INDEX)
    {
        startWorld = OffsetPositionWorld(startWorld,
                                         segment.TriangleIndex,
                                         NormalizeOrZero(receiver.PositionWorld - segment.StartWorld));
        endWorld = OffsetPositionWorld(endWorld,
                                       segment.TriangleIndex,
                                       NormalizeOrZero(receiver.PositionWorld - segment.EndWorld));
    }
    // A receiver in the segment's line spans no shadow triangle; the sine of the apex angle is tested
    // against the triangle loop's parallel tolerance.
    float3 edgeAWorld = startWorld - apexWorld;
    float3 edgeBWorld = endWorld - apexWorld;
    float normalLength = length(cross(edgeAWorld, edgeBWorld));
    if (isfinite(normalLength) == false ||
        normalLength <= TRIANGLE_PARALLEL_COSINE_TOLERANCE * length(edgeAWorld) * length(edgeBWorld))
    {
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    uint receiverTriangle = receiver.TriangleIndex < Counts.x ? receiver.TriangleIndex : SCENE_INVALID_INDEX;
    bool isOccluded;
    if (TraceLineVisibility(apexWorld,
                            startWorld,
                            endWorld,
                            receiverTriangle,
                            segment.TriangleIndex,
                            outVisibility,
                            isOccluded) == false)
    {
        ClearLineVisibility(outVisibility);
        queries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    float visibleParameter = 0;
    for (uint i = 0; i < outVisibility.IntervalCount; ++i)
        visibleParameter += outVisibility.Intervals[i].y - outVisibility.Intervals[i].x;
    // Parameters address the caller's unoffset segment, so its length scales them.
    outVisibility.VisibleLengthWorld = visibleParameter * segmentLengthWorld;
    return isOccluded == true ? SCENE_QUERY_SURFACE : SCENE_QUERY_MISS;
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
/// Draw a pseudorandom float in [0,1) at an explicit address, without advancing queries.
/// @pre dimension is no greater than FoundationMath's RANDOM_MAX_DIMENSION.
float SampleSceneRandom(RayLaunch launch, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    return SampleRandomFloat01(launch.Seed, PixelRandomAddress(pixel, launch.SampleIndex, dimension));
}
/// Draw a pseudorandom float in [0,1), advancing the renderer-owned invocation stream.
/// Available in ray generation, closest-hit and miss with no ScenePayload fields.
/// Forward the same queries through callbacks; do not reset or copy its state.
/// Exhaustion latches HasFailed and returns zero, preventing sample publication.
/// Explicit launch/dimension overloads address their own draws and do not advance this stream.
float SampleSceneRandom(inout RayQueryContext queries)
{
    uint dimension;
    if (TryAdvanceSceneRandomDimension(queries, dimension) == false)
        return 0;
    return SampleRandomFloat01(queries.RandomSeed,
                               PixelRandomAddress(queries.RandomPixel, queries.RandomSampleIndex, dimension));
}
/// Draw an unbiased uint in [minimumInclusive, maximumExclusive), consuming one stream dimension.
/// Uses FoundationMath's bounded integer mapper, without float conversion or modulo bias.
/// Empty/reversed ranges latch HasFailed without consuming a dimension and return zero.
/// Stream or bounded-mapping exhaustion also latches HasFailed and returns zero.
/// Zero on failure is not a valid sample; the renderer rejects the invocation's result.
uint SampleSceneRandom(inout RayQueryContext queries, uint minimumInclusive, uint maximumExclusive)
{
    if (minimumInclusive >= maximumExclusive)
    {
        queries.HasFailed = true;
        return 0;
    }
    uint dimension;
    if (TryAdvanceSceneRandomDimension(queries, dimension) == false)
        return 0;
    uint offset;
    if (TrySampleRandomBounded(queries.RandomSeed,
                               PixelRandomAddress(queries.RandomPixel, queries.RandomSampleIndex, dimension),
                               maximumExclusive - minimumInclusive,
                               offset) == false)
    {
        queries.HasFailed = true;
        return 0;
    }
    return minimumInclusive + offset;
}
void EvaluateStandardBsdf(MaterialSample material,
                          float3 viewDirectionWorld,
                          float3 lightDirectionWorld,
                          out float3 outValue,
                          out float outPdf)
{
    EvaluateScattering(material.BaseColor,
                       material.Metallic,
                       material.Roughness,
                       material.Model,
                       material.Eta,
                       material.K,
                       material.AlphaU,
                       material.ShadingNormalWorld,
                       viewDirectionWorld,
                       lightDirectionWorld,
                       outValue,
                       outPdf);
}
float3 SampleStandardBsdf(MaterialSample material, float3 viewDirectionWorld, float3 samples)
{
    return SampleScatteringBsdf(material.ShadingNormalWorld,
                                viewDirectionWorld,
                                ComputeMaterialAlpha(material.Model, material.Roughness, material.AlphaU),
                                ComputeMaterialSpecularProbability(material.Model, material.Metallic),
                                samples);
}

/// Complete continuation proposal for the typed reference models. Weight already
/// contains f*cos/pdf; Pdf is solely the external MIS density (estimated for coatings).
struct StandardBsdfTransportSample
{
    float3 DirectionWorld;
    float3 Weight;
    float Pdf;
    bool HasSample;
};
CoatingMaterial MakeCoatingMaterial(MaterialSample material)
{
    CoatingMaterial coating;
    coating.BaseColor = material.BaseColor;
    coating.Alpha = material.AlphaU;
    coating.Eta = material.CoatingEta;
    coating.Thickness = material.CoatingThickness;
    return coating;
}
/// Layered-aware evaluation with explicitly addressed independent value/PDF estimates.
/// Pair this overload with SampleStandardBsdfTransport for matching MIS semantics.
/// Requires a valid supported material and unit directions; material event is 0..255.
void EvaluateStandardBsdf(MaterialSample material,
                          float3 viewDirectionWorld,
                          float3 lightDirectionWorld,
                          CoatingRandomContext random,
                          out float3 outValue,
                          out float outPdf)
{
    if (material.Model == LUDUS_MATERIAL_MODEL_COATED_DIFFUSE)
    {
        CoatingEvaluation evaluated = EvaluateCoatedDiffuse(MakeCoatingMaterial(material),
                                                            material.ShadingNormalWorld,
                                                            viewDirectionWorld,
                                                            lightDirectionWorld,
                                                            random);
        outValue = evaluated.Value;
        outPdf = evaluated.Pdf;
        return;
    }
    EvaluateStandardBsdf(material, viewDirectionWorld, lightDirectionWorld, outValue, outPdf);
}
/// Sample typed reference transport. Coatings use their bounded internal random
/// walk; its path ratio must not be reconstructed from a fresh EvaluateStandardBsdf call.
/// Requires a valid supported material and unit directions; material event is 0..255.
StandardBsdfTransportSample SampleStandardBsdfTransport(MaterialSample material,
                                                        float3 viewDirectionWorld,
                                                        float3 samples,
                                                        CoatingRandomContext random)
{
    StandardBsdfTransportSample result = (StandardBsdfTransportSample)0;
    if (material.Model == LUDUS_MATERIAL_MODEL_COATED_DIFFUSE)
    {
        CoatingSample sampled = SampleCoatedDiffuse(MakeCoatingMaterial(material),
                                                    material.ShadingNormalWorld,
                                                    viewDirectionWorld,
                                                    samples,
                                                    random);
        result.DirectionWorld = sampled.DirectionWorld;
        result.Weight = sampled.Weight;
        result.Pdf = sampled.MisPdf;
        result.HasSample = sampled.Accepted;
        return result;
    }
    result.DirectionWorld = SampleStandardBsdf(material, viewDirectionWorld, samples);
    float3 value;
    float pdf;
    EvaluateStandardBsdf(material, viewDirectionWorld, result.DirectionWorld, value, pdf);
    result.HasSample = pdf > 0;
    result.Pdf = pdf;
    result.Weight =
        result.HasSample == true ? value * (dot(material.ShadingNormalWorld, result.DirectionWorld) / pdf) : float3(0);
    return result;
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
