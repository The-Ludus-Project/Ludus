#include "coated_diffuse.slang"
#include "dielectric.slang"
#include "gaussian.slang"
#include "geometry_sampling_layout.h"
#include "metropolis.slang"
#include "metropolis_layout.h"
#include "photon_layout.h"
#include "stochastic_pairwise_mis.slang"
// Public logical scene shader interface v1. Packed buffers and traversal remain Renderer-owned.
// Thanks to Pharr, Jakob and Humphreys, PBRT 4e (2023), sections 12.1/12.6/13.4:
// https://pbr-book.org/4ed/Light_Sources/Light_Interface
// separate discrete source choice from incident-light proposals and solid-angle PDFs.
// No integrator, MIS weighting or bounce decision is performed in this module.
#include "scene_data.h"
#include "scene_shader_layout.h"
static const uint SCENE_INTERFACE_VERSION = LUDUS_SCENE_SHADER_INTERFACE_VERSION;
static const uint SCENE_INVALID_INDEX = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
/// Outcome of TraceClosest, TestVisibility, FindVisibleLineIntervals and TraceAndShade. Scoped Slang enums
/// keep a plain number or another enumeration from standing in for a result; values are 32-bit and stable.
enum SceneQuery : uint
{
    Miss = 0,    ///< Nothing was hit, or a visibility test found no occluder.
    Surface = 1, ///< A surface was hit, or an occluder removed visibility.
    Failure = 2, ///< queries.HasFailed is latched; outputs are zero and the sample shows the error colour.
};
/// LightInfo.Kind. Directional, Point and Spot match the LUSCN light kinds; Mesh and Environment are
/// shader-interface sources without an authored light record.
enum SceneLightKind : uint
{
    Directional = 0, ///< Analytic directional light along DirectionWorld.
    Point = 1,       ///< Analytic point light at PositionWorld.
    Spot = 2,        ///< Analytic spot light with InnerCos/OuterCos cone falloff.
    Mesh = 3,        ///< Emissive triangles; EmitterIndex names the emitter.
    Environment = 4, ///< The aggregate scene environment source.
};
/// SurfaceHit.MaterialKind: how the hit's material was resolved.
enum SceneMaterialKind : uint
{
    Assigned = LUDUS_SCENE_MATERIAL_KIND_ASSIGNED, ///< The triangle's own material slot.
    Default = LUDUS_SCENE_MATERIAL_KIND_DEFAULT,   ///< No material was assigned; the default material stands in.
    Error = LUDUS_SCENE_MATERIAL_KIND_ERROR,       ///< The assignment was invalid; the error material stands in.
};
/// Interface v1 spellings of SceneQuery and SceneLightKind, retained for existing graphs. New code names
/// the enumerators (SceneQuery::Surface) instead.
static const SceneQuery SCENE_QUERY_MISS = SceneQuery::Miss;
static const SceneQuery SCENE_QUERY_SURFACE = SceneQuery::Surface;
static const SceneQuery SCENE_QUERY_FAILURE = SceneQuery::Failure;
static const SceneLightKind SCENE_LIGHT_DIRECTIONAL = SceneLightKind::Directional;
static const SceneLightKind SCENE_LIGHT_POINT = SceneLightKind::Point;
static const SceneLightKind SCENE_LIGHT_SPOT = SceneLightKind::Spot;
static const SceneLightKind SCENE_LIGHT_MESH = SceneLightKind::Mesh;
static const SceneLightKind SCENE_LIGHT_ENVIRONMENT = SceneLightKind::Environment;
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
/// Publish an already resolved persistent estimator. With Accumulate, its current
/// ratio replaces the previous ratio and the node's limit counts invocations.
/// Ordinary ray samples must return radiance without calling this function.
#if defined(LUDUS_RENDER_GRAPH_RAY_PIPELINE)
float3 ResolveGraphRadiance(float3 radiance)
{
    graphRadianceIsResolved = true;
    return radiance;
}
#endif
struct RayLaunch
{
    uint2 PositionPixel;
    uint2 ExtentPixel;
    Ray CameraRay;
    float4x4 CameraClipToWorld;
    float3 CameraPositionWorld;
    bool IsOrthographic;
    /// True when this invocation belongs to an entire-image dispatch rather than a capture row band.
    bool IsFullImageDispatch;
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
    /// Why HasFailed was set, a LUDUS_SCENE_FAILURE_* reason; the first FailSceneQuery wins. Zero when a shader set
    /// HasFailed directly.
    uint FailureReason;
};
/// Fail the invocation with a reason, a LUDUS_SCENE_FAILURE_* value or an authored reason in
/// [LUDUS_SCENE_FAILURE_AUTHORED_FIRST, LUDUS_SCENE_FAILURE_REASON_COUNT). The renderer counts the pixels that latch
/// each reason, so the editor names the failing check instead of only drawing the failure colour. The first reason
/// of an invocation is kept.
void FailSceneQuery(inout RayQueryContext queries, uint reason)
{
    if (queries.HasFailed == false)
        queries.FailureReason = reason < LUDUS_SCENE_FAILURE_REASON_COUNT ? reason : LUDUS_SCENE_FAILURE_UNSPECIFIED;
    queries.HasFailed = true;
}
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
    SceneMaterialKind MaterialKind;
    uint EmitterIndex;
    uint2 SourceId;
    float2 Barycentric;
    float2 TexcoordUv;
    bool IsFrontFace;
    bool IsValid;
};
struct MaterialSample
{
    MaterialModel Model;
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
    /// GetEmitter/GetEmitterTriangle index for mesh sources; SCENE_INVALID_INDEX otherwise.
    uint EmitterIndex;
    SceneLightKind Kind;
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
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering, 4th ed., sections 12.1 and
// 12.6 (https://pbr-book.org/4ed/Light_Sources/Light_Interface), for separating source selection from incident
// sampling and distinguishing delta mass from solid-angle density. Ludus uses packed scene indices and addressed
// draws. See docs/architecture/renderer-systems.md#unified-light-discovery-and-incident-sampling.
/// Probability measure of a sample conditioned on the selected light.
enum IncidentLightMeasure : uint
{
    SolidAngle, ///< Probability density per steradian at the receiver.
    Discrete,   ///< Probability mass of the deterministic incident event.
};
/// Incident proposal and shadow endpoint for one already chosen source in the current snapshot.
struct IncidentLightSample
{
    uint TriangleIndex;           ///< Sampled mesh primitive, or SCENE_INVALID_INDEX for non-mesh sources.
    float2 Barycentric;           ///< Coordinates on TriangleIndex when sampling a mesh light.
    float3 DirectionWorld;        ///< Unit direction from the receiver towards the light.
    float3 IncidentCoefficient;   ///< Scene-linear contribution before the BSDF, receiver cosine and visibility.
    float ConditionalProbability; ///< Density or mass according to Measure, excluding the source PMF.
    float3 PositionWorld;         ///< Endpoint in world metres when HasFiniteEndpoint is true.
    float DistanceWorld;          ///< Distance to a finite endpoint in metres.
    IncidentLightMeasure Measure; ///< Interpretation of ConditionalProbability.
    bool HasFiniteEndpoint;       ///< False for a connection towards an infinite source.
    bool HasSample;               ///< Other fields describe a proposal only when true.
};
/// Legacy sample with the default uniform selection PMF and a discrete flag.
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
    return scene.Version == SCENE_INTERFACE_VERSION && (asuint(Options.w) & SCENE_DATA_ALL & groups) == groups;
}
/// Material operations used by a scene pass. The restricted sampler consumes three uniform values
/// and a continuous directional density. The full interface also handles delta and layered events.
enum SceneBsdfRequirement : uint
{
    Transport,
    FixedDimensionReflection,
};
bool RequireSceneBsdf(SceneContext scene, SceneBsdfRequirement requirement, inout RayQueryContext queries)
{
    if (HasSceneData(scene, SCENE_DATA_MATERIALS) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return false;
    }
    if (requirement == SceneBsdfRequirement::FixedDimensionReflection &&
        (asuint(Options.w) & LUDUS_GRAPH_REQUIRES_EXTENDED_BSDF) != 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_FIXED_DIMENSION_MATERIAL);
        return false;
    }
    return true;
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
    result.IsEmissionTwoSided = (asuint(SceneValue(offset + LUDUS_GPU_PREVIEW_EMITTER_METADATA_WORD).x) &
                                 LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_TWO_SIDED) != 0;
    result.IsValid = true;
    return result;
}
LightInfo GetLightInfo(SceneContext scene, uint lightIndex)
{
    LightInfo result = (LightInfo)0;
    result.Index = lightIndex;
    result.EmitterIndex = SCENE_INVALID_INDEX;
    result.IsValid = lightIndex < GetLightCount(scene);
    if (result.IsValid == false)
        return result;
    if (lightIndex < Counts.w)
    {
        uint offset = SurfaceLayout.y + lightIndex * LUDUS_GPU_PREVIEW_LIGHT_WORDS;
        float4 intensityAndKind = SceneValue(offset);
        float4 positionAndRangeWorld = SceneValue(offset + 1);
        float4 directionAndConeWorld = SceneValue(offset + 2);
        result.Kind = SceneLightKind(uint(intensityAndKind.w));
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
        result.Kind = SceneLightKind::Mesh;
        result.EmitterIndex = lightIndex - Counts.w;
        float4 emitter = GetEmitter(scene, lightIndex - Counts.w);
        uint triangle = asuint(SceneValue(Offsets.z + uint(emitter.x)).x);
        result.SourceId = asuint(SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 7).zw);
        result.MaterialSlot = asuint(SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).w);
    }
    else
    {
        result.Kind = SceneLightKind::Environment;
        result.Intensity = Environment.xyz;
    }
    return result;
}
/// One panel luminaire (graphics::renderer::PanelLuminaire): a square exit surface centred at CenterWorld with side
/// 2 HalfExtentWorld along the orthonormal TangentWorld and BitangentWorld, emitting along NormalWorld =
/// cross(TangentWorld, BitangentWorld), and the scalars of its beam profile. It is not geometry: no query hits it.
/// The shader that reads it defines its emission; see the slice-sampling-luminaire template.
struct LuminaireInfo
{
    float3 CenterWorld;
    float HalfExtentWorld;
    float3 TangentWorld;
    float3 BitangentWorld;
    float3 NormalWorld;
    float RadianceScale;
    float BeamWidth;
    float BeamSteering;
    float BeamSpread;
    float BeamTwist;
    /// False when luminaireIndex is not below GetLuminaireCount; every other field is then zero.
    bool IsValid;
};
/// Panel luminaires of the scene, in source-identity order; zero without the lights scene data.
uint GetLuminaireCount(SceneContext scene)
{
    return HasSceneData(scene, SCENE_DATA_LIGHTS) == true
               ? asuint(SceneValue(SurfaceLayout.y + Counts.w * LUDUS_GPU_PREVIEW_LIGHT_WORDS).x)
               : 0;
}
/// Read panel luminaire luminaireIndex; constant for one scene source generation.
LuminaireInfo GetLuminaireInfo(SceneContext scene, uint luminaireIndex)
{
    LuminaireInfo result = (LuminaireInfo)0;
    if (luminaireIndex >= GetLuminaireCount(scene))
        return result;
    uint offset = SurfaceLayout.y + Counts.w * LUDUS_GPU_PREVIEW_LIGHT_WORDS +
                  LUDUS_GPU_PREVIEW_LUMINAIRE_HEADER_WORDS + luminaireIndex * LUDUS_GPU_PREVIEW_LUMINAIRE_WORDS;
    float4 centerAndHalfExtentWorld = SceneValue(offset);
    float4 tangentAndRadianceScale = SceneValue(offset + 1);
    float4 bitangentAndBeamWidth = SceneValue(offset + 2);
    float4 beamSlopes = SceneValue(offset + LUDUS_GPU_PREVIEW_LUMINAIRE_BEAM_WORD);
    result.CenterWorld = centerAndHalfExtentWorld.xyz;
    result.HalfExtentWorld = centerAndHalfExtentWorld.w;
    result.TangentWorld = tangentAndRadianceScale.xyz;
    result.RadianceScale = tangentAndRadianceScale.w;
    result.BitangentWorld = bitangentAndBeamWidth.xyz;
    result.BeamWidth = bitangentAndBeamWidth.w;
    result.NormalWorld = cross(result.TangentWorld, result.BitangentWorld);
    result.BeamSteering = beamSlopes.x;
    result.BeamSpread = beamSlopes.y;
    result.BeamTwist = beamSlopes.z;
    result.IsValid = true;
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
    result.Model = MaterialModel(asuint(etaAndModel.w));
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
    if (hit.MaterialKind == SceneMaterialKind::Error)
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
/// Test the sampled endpoint's fixed-mip alpha mask, including vertex/tint alpha, without tracing.
/// Invalid scene/hit indices return false. An opaque endpoint always returns true.
/// Area/geometry proposals must retain uncovered samples as zero-contribution events, without retrying.
/// This uses the same inclusive coverage cutoff as TraceClosest; it issues no visibility query.
bool IsSurfaceCovered(SceneContext scene, SurfaceHit hit)
{
    if (scene.Version != SCENE_INTERFACE_VERSION || hit.IsValid == false || hit.TriangleIndex >= Counts.x)
        return false;
    uint triangle = hit.TriangleIndex;
    if (asuint(Surface(triangle, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) == 0)
        return true;
    float alpha = Texture(triangle, hit.TexcoordUv).w * Surface(triangle, 2).w *
                  InterpolateTriangleColor(triangle, hit.Barycentric).w;
    return isfinite(alpha) == true && alpha >= Surface(triangle, 1).y;
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
SceneQuery TraceClosest(SceneContext scene, inout RayQueryContext queries, Ray ray, out SurfaceHit outHit)
{
    outHit = (SurfaceHit)0;
    if (queries.HasFailed == true)
        return SceneQuery::Failure;
    if (scene.Version != SCENE_INTERFACE_VERSION)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INTERFACE_VERSION);
        return SceneQuery::Failure;
    }
    if (queries.SurfaceQueries >= queries.MaximumSurfaceQueries)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SURFACE_BUDGET);
        return SceneQuery::Failure;
    }
    if (IsSceneRayValid(ray) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RAY);
        return SceneQuery::Failure;
    }
    queries.SurfaceQueries += 1;
    Hit hit = Trace(ray.OriginWorld, ray.DirectionWorld, ray.MinDistanceWorld, ray.MaxDistanceWorld);
    if (hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_TRAVERSAL);
        return SceneQuery::Failure;
    }
    if (hit.Triangle == SCENE_INVALID_INDEX)
        return SceneQuery::Miss;
    uint base = hit.Triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    outHit.IncomingRay = ray;
    outHit.TriangleIndex = hit.Triangle;
    outHit.Primitive = asuint(SceneValue(base + 2).w);
    outHit.InstanceIndex = asuint(SceneValue(base + 3).w);
    outHit.MaterialIndex = asuint(SceneValue(base).w);
    outHit.MaterialSlot = asuint(SceneValue(base + 8).w);
    outHit.SourceMaterialIndex = asuint(SceneValue(base + 4).w);
    outHit.MaterialKind = SceneMaterialKind(asuint(SceneValue(base + 5).w));
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
    return SceneQuery::Surface;
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
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED);
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
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_LIGHT_SAMPLE);
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
static uint ReadMeshPowerCumulative(uint meshCount, uint meshIndex)
{
    const uint offset = Offsets.z + Counts.z + meshCount + meshIndex * LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_WORDS;
    return asuint(SceneValue(offset + LUDUS_GPU_PREVIEW_EMITTER_METADATA_WORD).y);
}
/// Evaluate the catalog PMF for SelectLightByMeshPower, including its uniform fallback.
float EvaluateMeshPowerLightSelectionPmf(SceneContext scene, uint lightIndex)
{
    const uint lightCount = GetLightCount(scene);
    if (lightIndex >= lightCount)
        return 0;
    const uint meshCount = GetEmitterCount(scene);
    if (lightIndex < Counts.w || lightIndex >= Counts.w + meshCount)
        return 1.0 / float(lightCount);
    const uint meshIndex = lightIndex - Counts.w;
    const uint meshTotal = ReadMeshPowerCumulative(meshCount, meshCount - 1);
    if (meshTotal == 0)
        return 1.0 / float(lightCount);
    const uint previous = meshIndex == 0 ? 0 : ReadMeshPowerCumulative(meshCount, meshIndex - 1);
    const uint weight = ReadMeshPowerCumulative(meshCount, meshIndex) - previous;
    return float(weight) / float((meshTotal / meshCount) * lightCount);
}
/// Select one catalog source at the given dimension. Mesh lights split their original total
/// probability by area times emitted luminance, with nonzero support for each light.
/// Analytic and environment source probabilities match SelectLight. With no mesh table,
/// selection is uniform. Empty catalogs or exhausted random addresses return no sample.
SelectedLight SelectLightByMeshPower(SceneContext scene, RayLaunch launch, uint dimension)
{
    const uint meshCount = GetEmitterCount(scene);
    const uint meshTotal = meshCount == 0 ? 0 : ReadMeshPowerCumulative(meshCount, meshCount - 1);
    if (meshTotal == 0)
        return SelectLight(scene, launch, dimension);
    const uint ticketsPerSource = meshTotal / meshCount;
    const uint total = ticketsPerSource * GetLightCount(scene);
    const uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    SelectedLight result = (SelectedLight)0;
    uint ticket;
    if (TrySampleRandomBounded(launch.Seed, PixelRandomAddress(pixel, launch.SampleIndex, dimension), total, ticket) ==
        false)
        return result;
    if (ticket < meshTotal)
    {
        uint low = 0;
        uint high = meshCount - 1;
        while (low < high)
        {
            const uint middle = low + (high - low) / 2;
            if (ReadMeshPowerCumulative(meshCount, middle) <= ticket)
                low = middle + 1;
            else
                high = middle;
        }
        result.LightIndex = Counts.w + low;
    }
    else
    {
        const uint otherIndex = (ticket - meshTotal) / ticketsPerSource;
        result.LightIndex = otherIndex < Counts.w ? otherIndex : otherIndex + meshCount;
    }
    result.SelectionPmf = EvaluateMeshPowerLightSelectionPmf(scene, result.LightIndex);
    result.HasSample = true;
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
/// Resolve an emissive hit's source index in this snapshot, or SCENE_INVALID_INDEX.
uint GetSurfaceLightIndex(SceneContext scene, SurfaceHit endpoint)
{
    return endpoint.IsValid == true && endpoint.EmitterIndex < GetEmitterCount(scene) ? Counts.w + endpoint.EmitterIndex
                                                                                      : SCENE_INVALID_INDEX;
}
/// Resolve the aggregate environment source index in this snapshot, or SCENE_INVALID_INDEX.
uint GetEnvironmentLightIndex(SceneContext scene)
{
    uint index = Counts.w + GetEmitterCount(scene);
    return HasSceneData(scene, SCENE_DATA_LIGHTS) == true && index < GetLightCount(scene) ? index : SCENE_INVALID_INDEX;
}
static float ComputeMeshIncidentPdfSolidAngle(float distanceSquaredWorld, float areaWorld, float cosine)
{
    return cosine > 0 && areaWorld > 0 ? distanceSquaredWorld / (areaWorld * cosine) : 0;
}
/// Evaluate the area-uniform mesh proposal at an emitter hit, per steradian at the receiver.
/// The source PMF is separate. Non-emitter hits have zero density.
float EvaluateIncidentLightPdfSolidAngle(SceneContext scene, float3 receiverPositionWorld, SurfaceHit endpoint)
{
    if (GetSurfaceLightIndex(scene, endpoint) == SCENE_INVALID_INDEX)
        return 0;
    float4 emitter = GetEmitter(scene, endpoint.EmitterIndex);
    float3 offsetWorld = endpoint.PositionWorld - receiverPositionWorld;
    float cosine = abs(dot(endpoint.GeometricNormalWorld, -NormalizeOrZero(offsetWorld)));
    return ComputeMeshIncidentPdfSolidAngle(dot(offsetWorld, offsetWorld), emitter.z, cosine);
}
/// Joint mesh-hit density for the legacy uniform source selection and area proposal.
float EvaluateLightPdfSolidAngle(SceneContext scene, float3 receiverPositionWorld, SurfaceHit endpoint)
{
    return EvaluateLightSelectionPmf(scene, GetSurfaceLightIndex(scene, endpoint)) *
           EvaluateIncidentLightPdfSolidAngle(scene, receiverPositionWorld, endpoint);
}
static const float UNIFORM_ENVIRONMENT_PDF_SOLID_ANGLE = 1 / (4 * PI);
/// Slots to reserve for an addressed default incident proposal, regardless of which kind is selected.
static const uint SCENE_INCIDENT_LIGHT_DIMENSION_COUNT = 3;
static const uint INCIDENT_LIGHT_SECOND_DIMENSION_OFFSET = 1;
static const uint INCIDENT_LIGHT_LAST_DIMENSION_OFFSET = SCENE_INCIDENT_LIGHT_DIMENSION_COUNT - 1;

static IncidentLightSample
SampleIncidentLightAtCoordinates(SceneContext scene, LightInfo light, SurfaceHit receiver, float3 shapeSample)
{
    IncidentLightSample result = (IncidentLightSample)0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    result.Measure = IncidentLightMeasure::SolidAngle;
    result.ConditionalProbability = 1;
    if (light.Kind == SceneLightKind::Mesh)
    {
        float4 emitter = GetEmitter(scene, light.EmitterIndex);
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
        result.ConditionalProbability =
            ComputeMeshIncidentPdfSolidAngle(dot(offsetWorld, offsetWorld), emitter.z, cosine);
        result.IncidentCoefficient =
            Surface(triangle, 4).xyz * Texture(triangle, TexcoordUv(triangle, result.Barycentric), 2).xyz;
        if (asuint(Surface(triangle, 5).w) == 0 && dot(normalWorld, -result.DirectionWorld) <= 0)
            result.IncidentCoefficient = 0;
        result.HasFiniteEndpoint = true;
    }
    else if (light.Kind == SceneLightKind::Environment)
    {
        float cosine = 1 - 2 * shapeSample.y;
        float radius = sqrt(max(0, 1 - cosine * cosine));
        float longitude = 2 * PI * shapeSample.z;
        result.DirectionWorld = float3(radius * cos(longitude), cosine, radius * sin(longitude));
        result.IncidentCoefficient = EvaluateEnvironmentWorld(result.DirectionWorld).xyz;
        result.ConditionalProbability = UNIFORM_ENVIRONMENT_PDF_SOLID_ANGLE;
    }
    else
    {
        result.Measure = IncidentLightMeasure::Discrete;
        result.DirectionWorld = -light.DirectionWorld;
        result.IncidentCoefficient = light.Intensity;
        if (light.Kind != SceneLightKind::Directional)
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
            if (light.Kind == SceneLightKind::Spot)
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
/// Legacy explicit-coordinate proposal with the default uniform selection PMF.
LightSample SampleIncidentLight(SceneContext scene, SelectedLight selected, SurfaceHit receiver, float3 shapeSample)
{
    LightSample result = (LightSample)0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    if (selected.HasSample == false || selected.LightIndex >= GetLightCount(scene) || any(shapeSample < 0) ||
        any(shapeSample >= 1) || any(isfinite(shapeSample) == false))
        return result;
    IncidentLightSample incident =
        SampleIncidentLightAtCoordinates(scene, GetLightInfo(scene, selected.LightIndex), receiver, shapeSample);
    result.LightIndex = selected.LightIndex;
    result.TriangleIndex = incident.TriangleIndex;
    result.Barycentric = incident.Barycentric;
    result.DirectionWorld = incident.DirectionWorld;
    result.IncidentCoefficient = incident.IncidentCoefficient;
    result.SelectionPmf = EvaluateLightSelectionPmf(scene, selected.LightIndex);
    result.ConditionalPdf = incident.ConditionalProbability;
    result.PositionWorld = incident.PositionWorld;
    result.DistanceWorld = incident.DistanceWorld;
    result.IsDiscrete = incident.Measure == IncidentLightMeasure::Discrete;
    result.HasFiniteEndpoint = incident.HasFiniteEndpoint;
    result.HasSample = incident.HasSample;
    return result;
}
/// Trace visibility to a conditional incident sample using the invocation's query budget.
SceneQuery
TestVisibility(SceneContext scene, inout RayQueryContext queries, SurfaceHit receiver, IncidentLightSample light)
{
    if (queries.HasFailed == true)
        return SceneQuery::Failure;
    if (light.HasSample == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_LIGHT_SAMPLE);
        return SceneQuery::Failure;
    }
    if (queries.VisibilityQueries >= queries.MaximumVisibilityQueries)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_VISIBILITY_BUDGET);
        return SceneQuery::Failure;
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
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RAY);
        return SceneQuery::Failure;
    }
    Hit hit = Trace(shadow.OriginWorld, shadow.DirectionWorld, 0, shadow.MaxDistanceWorld, true);
    if (hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_TRAVERSAL);
        return SceneQuery::Failure;
    }
    return hit.Triangle == SCENE_INVALID_INDEX ? SceneQuery::Miss : SceneQuery::Surface;
}
/// Trace visibility to a sample returned by the legacy explicit-coordinate overload.
SceneQuery TestVisibility(SceneContext scene, inout RayQueryContext queries, SurfaceHit receiver, LightSample light)
{
    IncidentLightSample incident = (IncidentLightSample)0;
    incident.TriangleIndex = light.TriangleIndex;
    incident.DirectionWorld = light.DirectionWorld;
    incident.PositionWorld = light.PositionWorld;
    incident.HasFiniteEndpoint = light.HasFiniteEndpoint;
    incident.HasSample = light.HasSample;
    return TestVisibility(scene, queries, receiver, incident);
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
/// Returns SceneQuery::Miss when no opaque occluder removes any length, SceneQuery::Surface when one
/// does (a fully occluded segment has zero intervals), and SceneQuery::Failure for an exhausted line
/// budget, an earlier failure, an invalid receiver or segment triangle, nonfinite positions, a segment
/// shorter than SCENE_UNIT_TOLERANCE, a receiver in the segment's line, a nonfinite interval, more
/// than SCENE_LINE_INTERVAL_CAPACITY - 1 running occluded intervals or an exhausted BVH stack.
/// Failure latches queries.HasFailed and zeroes outVisibility.
SceneQuery FindVisibleLineIntervals(SceneContext scene,
                                    inout RayQueryContext queries,
                                    SurfaceHit receiver,
                                    LineSegment segment,
                                    out LineVisibility outVisibility)
{
    ClearLineVisibility(outVisibility);
    if (queries.HasFailed == true)
        return SceneQuery::Failure;
    if (scene.Version != SCENE_INTERFACE_VERSION)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INTERFACE_VERSION);
        return SceneQuery::Failure;
    }
    if (queries.LineQueries >= queries.MaximumLineQueries)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_LINE_BUDGET);
        return SceneQuery::Failure;
    }
    if (receiver.IsValid == false ||
        (segment.TriangleIndex != SCENE_INVALID_INDEX && segment.TriangleIndex >= Counts.x))
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
        return SceneQuery::Failure;
    }
    queries.LineQueries += 1;
    float3 segmentWorld = segment.EndWorld - segment.StartWorld;
    float segmentLengthWorld = length(segmentWorld);
    if (all(isfinite(receiver.PositionWorld)) == false || all(isfinite(segment.StartWorld)) == false ||
        all(isfinite(segment.EndWorld)) == false || isfinite(segmentLengthWorld) == false ||
        segmentLengthWorld < SCENE_UNIT_TOLERANCE)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
        return SceneQuery::Failure;
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
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
        return SceneQuery::Failure;
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
        // The traversal's interval set or stack could not hold the segment's occluders.
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_TRAVERSAL);
        return SceneQuery::Failure;
    }
    float visibleParameter = 0;
    for (uint i = 0; i < outVisibility.IntervalCount; ++i)
        visibleParameter += outVisibility.Intervals[i].y - outVisibility.Intervals[i].x;
    // Parameters address the caller's unoffset segment, so its length scales them.
    outVisibility.VisibleLengthWorld = visibleParameter * segmentLengthWorld;
    return isOccluded == true ? SceneQuery::Surface : SceneQuery::Miss;
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
/// Logical immutable geometry hierarchy node. Root is index zero; nodes are depth-first preorder, with the complete
/// Left subtree before Right. Leaf ranges address GetGeometryHierarchyTriangle, not triangle or native-opacity order
/// directly. Invalid indices return IsValid=false with zero fields. Borrowed values last for scene.Generation.
struct GeometryHierarchyNode
{
    float3 MinimumWorld; ///< Bounds in world units, including every descendant primitive.
    float3 MaximumWorld;
    uint Left;          ///< First child for inner nodes (TriangleCount == 0).
    uint Right;         ///< Second child for inner nodes.
    uint FirstTriangle; ///< First leaf-order slot for leaves.
    uint TriangleCount; ///< Positive for leaves, zero for inner nodes.
    bool IsValid;
};
/// Number of immutable hierarchy nodes; zero for empty/invalid scenes. Requires scene geometry.
uint GetGeometryHierarchyNodeCount(SceneContext scene)
{
    return HasSceneData(scene, SCENE_DATA_GEOMETRY) == true ? Counts.y : 0;
}
/// Read a logical BVH node without issuing a ray query. Child ordering is independent of native opacity sorting.
GeometryHierarchyNode GetGeometryHierarchyNode(SceneContext scene, uint nodeIndex)
{
    GeometryHierarchyNode result = (GeometryHierarchyNode)0;
    if (nodeIndex >= GetGeometryHierarchyNodeCount(scene))
        return result;
    BvhNode node = LoadBvhNode(nodeIndex);
    result.MinimumWorld = node.MinimumWorld;
    result.MaximumWorld = node.MaximumWorld;
    result.Left = node.Left;
    result.Right = node.Right;
    result.FirstTriangle = node.First;
    result.TriangleCount = node.Count;
    result.IsValid = true;
    return result;
}
/// Resolve one leaf-order slot to a scene triangle index; invalid slots return SCENE_INVALID_INDEX.
uint GetGeometryHierarchyTriangle(SceneContext scene, uint slot)
{
    return HasSceneData(scene, SCENE_DATA_GEOMETRY) == true && slot < Counts.x ? GetBvhLeafTriangle(slot)
                                                                               : SCENE_INVALID_INDEX;
}
TriangleData GetEmitterTriangle(SceneContext scene, uint emitterIndex, uint primitiveInEmitter)
{
    float4 emitter = GetEmitter(scene, emitterIndex);
    if (emitterIndex >= GetEmitterCount(scene) || primitiveInEmitter >= uint(emitter.y))
        return (TriangleData)0;
    uint triangle = asuint(SceneValue(Offsets.z + uint(emitter.x) + primitiveInEmitter).x);
    return GetTriangle(scene, triangle);
}
/// Uniform environment density per steradian, excluding the source PMF. Zero when the source is absent.
float EvaluateIncidentEnvironmentPdfSolidAngle(SceneContext scene)
{
    return GetEnvironmentLightIndex(scene) != SCENE_INVALID_INDEX ? UNIFORM_ENVIRONMENT_PDF_SOLID_ANGLE : 0;
}
/// Joint environment density for the legacy uniform source selection and sphere proposal.
float EvaluateEnvironmentPdfSolidAngle(SceneContext scene, float3 directionWorld)
{
    return EvaluateLightSelectionPmf(scene, GetEnvironmentLightIndex(scene)) * UNIFORM_ENVIRONMENT_PDF_SOLID_ANGLE;
}
/// Draw a pseudorandom float in [0,1) at an explicit address, without advancing queries.
/// @pre dimension is no greater than FoundationMath's RANDOM_MAX_DIMENSION.
float SampleSceneRandom(RayLaunch launch, uint dimension)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    return SampleRandomFloat01(launch.Seed, PixelRandomAddress(pixel, launch.SampleIndex, dimension));
}
/// Sample one catalog source using the kind's addressed incident proposal, without advancing the automatic stream.
/// Mesh reads three dimensions starting at the address, environment reads only the last two, and analytic sources
/// read no dimensions. An out-of-range source returns no sample. Exhausting a needed address fails the invocation.
IncidentLightSample SampleIncidentLight(SceneContext scene,
                                        uint lightIndex,
                                        SurfaceHit receiver,
                                        uint2 seed,
                                        RandomAddress firstAddress,
                                        inout RayQueryContext inoutQueries)
{
    IncidentLightSample empty = (IncidentLightSample)0;
    empty.TriangleIndex = SCENE_INVALID_INDEX;
    if (inoutQueries.HasFailed == true)
        return empty;
    LightInfo light = GetLightInfo(scene, lightIndex);
    if (light.IsValid == false)
        return empty;
    float3 shapeSample = float3(0);
    if (light.Kind == SceneLightKind::Mesh || light.Kind == SceneLightKind::Environment)
    {
        if (firstAddress.Dimension > RANDOM_MAX_DIMENSION - INCIDENT_LIGHT_LAST_DIMENSION_OFFSET)
        {
            FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED);
            return empty;
        }
        RandomAddress address = firstAddress;
        if (light.Kind == SceneLightKind::Mesh)
            shapeSample.x = SampleRandomFloat01(seed, address);
        address.Dimension = firstAddress.Dimension + INCIDENT_LIGHT_SECOND_DIMENSION_OFFSET;
        shapeSample.y = SampleRandomFloat01(seed, address);
        address.Dimension = firstAddress.Dimension + INCIDENT_LIGHT_LAST_DIMENSION_OFFSET;
        shapeSample.z = SampleRandomFloat01(seed, address);
    }
    return SampleIncidentLightAtCoordinates(scene, light, receiver, shapeSample);
}
/// Sample one source using the launch's pixel/sample address and the first reserved incident dimension.
IncidentLightSample SampleIncidentLight(SceneContext scene,
                                        uint lightIndex,
                                        SurfaceHit receiver,
                                        RayLaunch launch,
                                        uint firstDimension,
                                        inout RayQueryContext inoutQueries)
{
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    return SampleIncidentLight(scene,
                               lightIndex,
                               receiver,
                               launch.Seed,
                               PixelRandomAddress(pixel, launch.SampleIndex, firstDimension),
                               inoutQueries);
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
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
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
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED);
        return 0;
    }
    return minimumInclusive + offset;
}
/// True for dielectric (glass) materials, solid or thin. Both are smooth, delta interfaces that scatter on either side
/// of their surface. SampleStandardBsdfTransport returns IsSpecular samples for them and ComputeStandardBsdfCosine
/// returns zero, so no light sample reaches them; EvaluateStandardBsdf and the direction-only SampleStandardBsdf keep
/// the record's legacy opaque fields for graphs without transmission.
bool IsDielectricMaterial(MaterialSample material)
{
    return IsDielectricModel(material.Model);
}
/// Glass scatters only in delta directions; kept as its own question for callers that branch on delta events.
bool IsSpecularMaterial(MaterialSample material)
{
    return IsDielectricMaterial(material);
}
/// Shading normal on the viewer's side. Without a hit, the shading normal also decides whether the viewer is inside
/// a solid dielectric; FollowSpecularChain decides it from the geometric back face, as the reference tracers do.
float3 OrientDielectricNormal(MaterialSample material, float3 viewDirectionWorld)
{
    return dot(material.ShadingNormalWorld, viewDirectionWorld) < 0 ? -material.ShadingNormalWorld
                                                                    : material.ShadingNormalWorld;
}
/// Directional cosine factor of a light direction: the clamped cosine on the shading normal's side, or zero for
/// glass, whose delta lobes no sampled light reaches.
float ComputeStandardBsdfCosine(MaterialSample material, float3 viewDirectionWorld, float3 directionWorld)
{
    if (IsDielectricMaterial(material) == true)
        return 0;
    return max(0, dot(material.ShadingNormalWorld, directionWorld));
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
    if (material.Model == MaterialModel::ProjectBrdf)
        return SampleProjectBrdfDirection(material.Roughness,
                                          material.Eta,
                                          material.K,
                                          material.ShadingNormalWorld,
                                          viewDirectionWorld,
                                          samples);
    return SampleScatteringBsdf(material.ShadingNormalWorld,
                                viewDirectionWorld,
                                ComputeMaterialAlpha(material.Model, material.Roughness, material.AlphaU),
                                ComputeMaterialSpecularProbability(material.Model, material.Metallic),
                                samples);
}

/// Complete continuation proposal for the typed reference models. Weight already
/// contains f*cos/pdf; Pdf is solely the external MIS density (estimated for coatings).
/// A specular (glass) sample carries its complete weight with Pdf one: emission it finds takes the
/// full MIS weight, because light sampling cannot reach a delta direction.
struct StandardBsdfTransportSample
{
    float3 DirectionWorld;
    float3 Weight;
    float Pdf;
    bool HasSample;
    bool IsSpecular;     ///< Delta event; never combine Pdf with a light density.
    bool IsTransmission; ///< Refracted to the other side of the surface.
};
/// Glass continuation (dielectric.slang): samples.x chooses reflection with the Fresnel probability, so each weight is
/// one, or 1/eta^2 when refracted into a solid dielectric. isViewerInside selects the inverse index of a solid.
StandardBsdfTransportSample
SampleDielectricTransport(MaterialSample material, float3 viewDirectionWorld, float3 samples, bool isViewerInside)
{
    StandardBsdfTransportSample result = (StandardBsdfTransportSample)0;
    float3 normalWorld = OrientDielectricNormal(material, viewDirectionWorld);
    DielectricSample sampled =
        SampleDielectric(material.Model, material.Eta.x, isViewerInside, normalWorld, viewDirectionWorld, samples.x);
    result.HasSample = sampled.HasSample;
    if (result.HasSample == false)
        return result;
    result.DirectionWorld = sampled.DirectionWorld;
    result.Weight = sampled.Weight;
    result.Pdf = 1;
    result.IsSpecular = true;
    result.IsTransmission = dot(normalWorld, sampled.DirectionWorld) < 0;
    return result;
}
/// Glass continuation that decides the side from the shading normal, for callers without a hit.
StandardBsdfTransportSample
SampleDielectricTransport(MaterialSample material, float3 viewDirectionWorld, float3 samples)
{
    return SampleDielectricTransport(material,
                                     viewDirectionWorld,
                                     samples,
                                     dot(material.ShadingNormalWorld, viewDirectionWorld) < 0);
}
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
    if (material.Model == MaterialModel::CoatedDiffuse)
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
    if (IsDielectricMaterial(material) == true)
        return SampleDielectricTransport(material, viewDirectionWorld, samples);
    if (material.Model == MaterialModel::CoatedDiffuse)
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

/// Glass events FollowSpecularChain follows at most; each one costs a surface query.
static const uint SCENE_SPECULAR_CHAIN_EVENTS = 8;
/// Explicit random dimensions FollowSpecularChain draws from its base: one Fresnel choice per event.
static const uint SCENE_SPECULAR_CHAIN_DIMENSIONS = SCENE_SPECULAR_CHAIN_EVENTS;
/// Base for graphs that draw from the automatic stream, which shares the explicit addresses from zero upward: the
/// top of the address range, which that stream reaches only after RANDOM_MAX_DIMENSION draws.
static const uint SCENE_SPECULAR_CHAIN_DEFAULT_DIMENSION_FIRST =
    RANDOM_MAX_DIMENSION + 1 - SCENE_SPECULAR_CHAIN_DIMENSIONS;
/// Camera path through glass, ending at the first surface an estimator can shade.
struct SpecularChain
{
    SurfaceHit Hit;            ///< Final surface; its IncomingRay is the chain's last ray. Valid when IsSurface.
    float3 ViewDirectionWorld; ///< Unit direction from Hit back along that ray.
    float3 Throughput;         ///< Product of the delta-event weights followed before Hit.
    float3 Radiance;           ///< Throughput-weighted emission of the glass passed and a miss's environment.
    uint Events;               ///< Delta events followed.
    bool IsSurface;            ///< False after a miss, a failure, a null sample or SCENE_SPECULAR_CHAIN_EVENTS events.
};
/// Follow glass reflection and refraction from a surface reached along hit.IncomingRay, choosing each lobe by its
/// Fresnel probability so every event's weight is one, or 1/eta^2 across refraction into a solid dielectric. A path
/// tracer handles delta bounces this way: emission found after one takes the full MIS weight. Thanks to Matt Pharr,
/// Wenzel Jakob and Greg Humphreys, Physically Based Rendering, 4th edition, section 13.4 (A Better Path Tracer),
/// https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer.
/// Draws event k at SampleSceneRandom(launch, firstDimension + k); reserve SCENE_SPECULAR_CHAIN_DIMENSIONS of them
/// and SCENE_SPECULAR_CHAIN_EVENTS surface queries. Emission of the final Hit is left to the caller. A hit that is
/// not glass returns itself with unit throughput and no queries.
SpecularChain FollowSpecularChain(SceneContext scene,
                                  RayLaunch launch,
                                  inout RayQueryContext queries,
                                  SurfaceHit hit,
                                  uint firstDimension)
{
    SpecularChain chain;
    chain.Hit = hit;
    chain.ViewDirectionWorld = -hit.IncomingRay.DirectionWorld;
    chain.Throughput = float3(1);
    chain.Radiance = float3(0);
    chain.Events = 0;
    chain.IsSurface = hit.IsValid;
    while (chain.IsSurface == true)
    {
        MaterialSample material = EvaluateMaterial(scene, chain.Hit);
        if (material.IsValid == false || IsSpecularMaterial(material) == false)
            return chain;
        chain.Radiance += chain.Throughput * EvaluateEmission(scene, chain.Hit, chain.ViewDirectionWorld);
        chain.IsSurface = false;
        if (chain.Events >= SCENE_SPECULAR_CHAIN_EVENTS)
            return chain;
        // As in the reference tracers: a viewer behind the geometric front side is inside the glass, a reflection
        // stays on the viewer's side and a refraction leaves through the other one, or the sample is null.
        float3 geometricNormalWorld = chain.Hit.GeometricNormalWorld;
        bool isViewerInside = dot(geometricNormalWorld, chain.ViewDirectionWorld) < 0;
        StandardBsdfTransportSample sampled =
            SampleDielectricTransport(material,
                                      chain.ViewDirectionWorld,
                                      float3(SampleSceneRandom(launch, firstDimension + chain.Events), 0, 0),
                                      isViewerInside);
        bool isOnViewerSide = (dot(geometricNormalWorld, sampled.DirectionWorld) > 0) == (isViewerInside == false);
        if (sampled.HasSample == false || isOnViewerSide == sampled.IsTransmission)
            return chain;
        chain.Throughput *= sampled.Weight;
        SurfaceHit next;
        SceneQuery status =
            TraceClosest(scene,
                         queries,
                         SpawnSurfaceRay(chain.Hit, sampled.DirectionWorld, launch.CameraRay.MaxDistanceWorld),
                         next);
        chain.Events += 1;
        if (status == SceneQuery::Miss)
            chain.Radiance += chain.Throughput * EvaluateEnvironment(scene, sampled.DirectionWorld);
        if (status != SceneQuery::Surface)
            return chain;
        chain.Hit = next;
        chain.ViewDirectionWorld = -sampled.DirectionWorld;
        chain.IsSurface = true;
    }
    return chain;
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
    result.MaterialKind = SceneMaterialKind(asuint(SceneValue(base + 5).w));
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

#include "stochastic_reuse.slang"

// Scalar optical connections share this immutable scene and its fixed-mip material coverage.
bool IsWaveTriangleCovered(SceneContext scene, uint triangleIndex, float2 barycentric)
{
    return IsSurfaceCovered(scene, GetSurfaceHit(scene, triangleIndex, barycentric));
}
#include "wave_connection.slang"
