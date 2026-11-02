#pragma once
#include "weights.slang"

// Kettunen et al.'s conditional estimator; attribution and equations are in weights.slang.
// A suffix is an emissive endpoint, or a surface point with a cached, variable-length transport tail.
// Its first point is sampled in world area, independently of the supporting prefix. The outgoing
// direction at that point is also prefix independent; its BSDF is evaluated only when reconnecting.
// All later BSDF-sampled edges and the terminal NEE are cached. The marked suffix domain is common
// to every prefix, so the identity shift has Jacobian one and constant confidence shares suffice.
// Independent final-gather prefixes never participate in selection or tail generation.

static const uint RESERVOIR_WORDS = 6;
static const uint IDENTITY_WORD = 3;
static const uint DIRECTION_WORD = 4;
static const uint RADIANCE_WORD = 5;
// Include both depth settings in the identity: a recompiled graph cannot reuse a different integral.
static const uint LAYOUT_TAG = 0x43520003u | (CRIS_SUFFIX_BOUNCES << 8) | (CRIS_PREFIX_BOUNCES << 12);
static const uint FAILED_LAYOUT_TAG = 0x4352ffffu;
static const uint EXTENT_MASK = 0xffffu;
static const uint EXTENT_SHIFT = 16;
static const float MINIMUM_AREA_TARGET = 0.000001f;
static const float MAXIMUM_TRACE_DISTANCE_WORLD = 3.402823466e38f;
static const float3 LUMINANCE_WEIGHTS = float3(0.2126f, 0.7152f, 0.0722f);
static const uint MATERIAL_RANDOM_SCOPE = 0x43524953u;
static const uint PREFIX_MATERIAL_EVENT = 0;
static const uint DIRECT_MATERIAL_EVENT = 1;
static const uint GATHER_MATERIAL_EVENT = 2;
static const uint SUFFIX_GATHER_EVENT_OFFSET = 16;
static const uint CANDIDATE_DIMENSIONS = 5;
static const uint SUFFIX_DEPTH_DIMENSION = 3;
static const uint SUFFIX_DIMENSION_FIRST = 4;
static const uint TAIL_MATERIAL_EVENT_FIRST = 3;
static const uint TAIL_VERTEX_DIMENSIONS = 3;
static const uint MAXIMUM_SUFFIX_BOUNCES = 7;
static const uint MAXIMUM_NEIGHBORS = 7;
static const float NEIGHBOR_RADIUS_PIXELS = 8;

/// A marked suffix and its conditional area UCW, plus supporting prefix and identity.
/// Six uint4 words: first triangle/barycentric/W; M/support triangle/barycentric;
/// support view/layout (failure reason in x on failure); scene/extent/frame;
/// outgoing direction/suffix scattering count; cached tail coefficient/reserved.
/// The tail coefficient excludes the first suffix BSDF, which depends on the gather prefix.
struct Reservoir
{
    uint TriangleIndex;
    float2 Barycentric;
    float Weight;
    uint CandidateCount;
    uint SupportTriangleIndex;
    float2 SupportBarycentric;
    float3 SupportViewWorld;
    uint LayoutTag;
    uint2 SceneGeneration;
    uint2 ExtentPixel;
    uint SampleIndex;
    uint FailureReason;
    uint SuffixBounces;
    float3 DirectionWorld;
    float3 TailCoefficient;
};

/// A separately traced prefix endpoint and its f*cos/pdf throughput. A miss has IsValid=false.
struct Prefix
{
    SurfaceHit Hit;
    float3 ViewWorld;
    float3 Throughput;
    CoatingRandomContext MaterialRandom;
    bool IsValid;
};

uint GetPixelIndex(RayLaunch launch)
{
    return launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
}

RandomAddress MakePassAddress(RayLaunch launch, uint dimension)
{
    return MakeRandomAddress(uint2(GetPixelIndex(launch), GetGraphPassIndex()), launch.SampleIndex, dimension);
}

CoatingRandomContext MakeMaterialRandom(RayLaunch launch, uint event)
{
    CoatingRandomContext result;
    result.Key = launch.Seed;
    result.Address = MakeRandomAddress(uint2(GetPixelIndex(launch), GetGraphPassIndex() ^ MATERIAL_RANDOM_SCOPE),
                                       launch.SampleIndex,
                                       event);
    return result;
}

float SamplePassRandom(RayLaunch launch, uint dimension)
{
    return SampleRandomFloat01(launch.Seed, MakePassAddress(launch, dimension));
}

uint SamplePassIndex(RayLaunch launch, uint dimension, uint count, inout RayQueryContext queries)
{
    uint index = 0;
    if (TrySampleRandomBounded(launch.Seed, MakePassAddress(launch, dimension), count, index) == false)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
    return index;
}

bool HasCapacity(RayLaunch launch, bool hasSecond)
{
    uint firstWords;
    uint firstStride;
    uint secondWords;
    uint secondStride;
    GraphBuffer0.GetDimensions(firstWords, firstStride);
    secondWords = firstWords;
    if (hasSecond == true)
        GraphBuffer1.GetDimensions(secondWords, secondStride);
    if (launch.ExtentPixel.x == 0 || launch.ExtentPixel.y == 0 || any(launch.ExtentPixel > EXTENT_MASK) == true)
        return false;
    uint capacity = (hasSecond == true ? min(firstWords, secondWords) : firstWords) / RESERVOIR_WORDS;
    return launch.ExtentPixel.x <= capacity / launch.ExtentPixel.y;
}

Reservoir MakeEmptyReservoir(SceneContext scene, RayLaunch launch)
{
    Reservoir result = (Reservoir)0;
    result.TriangleIndex = SCENE_INVALID_INDEX;
    result.SupportTriangleIndex = SCENE_INVALID_INDEX;
    result.LayoutTag = LAYOUT_TAG;
    result.SceneGeneration = scene.Generation;
    result.ExtentPixel = launch.ExtentPixel;
    result.SampleIndex = launch.SampleIndex;
    return result;
}

uint4 LoadWord(uint pixel, uint word, bool isSecond)
{
    uint address = pixel * RESERVOIR_WORDS + word;
    return isSecond == true ? GraphBuffer1[address] : GraphBuffer0[address];
}

void StoreWord(uint pixel, uint word, bool isSecond, uint4 value)
{
    uint address = pixel * RESERVOIR_WORDS + word;
    if (isSecond == true)
        GraphBuffer1[address] = value;
    else
        GraphBuffer0[address] = value;
}

Reservoir LoadReservoir(uint pixel, bool isSecond)
{
    uint4 sample = LoadWord(pixel, 0, isSecond);
    uint4 support = LoadWord(pixel, 1, isSecond);
    uint4 view = LoadWord(pixel, 2, isSecond);
    uint4 identity = LoadWord(pixel, IDENTITY_WORD, isSecond);
    Reservoir result = (Reservoir)0;
    result.TriangleIndex = sample.x;
    result.Barycentric = asfloat(sample.yz);
    result.Weight = asfloat(sample.w);
    result.CandidateCount = support.x;
    result.SupportTriangleIndex = support.y;
    result.SupportBarycentric = asfloat(support.zw);
    result.SupportViewWorld = asfloat(view.xyz);
    result.LayoutTag = view.w;
    result.FailureReason = view.w == FAILED_LAYOUT_TAG ? view.x : 0;
    result.SceneGeneration = identity.xy;
    result.ExtentPixel = uint2(identity.z & EXTENT_MASK, identity.z >> EXTENT_SHIFT);
    result.SampleIndex = identity.w;
    uint4 direction = LoadWord(pixel, DIRECTION_WORD, isSecond);
    result.DirectionWorld = asfloat(direction.xyz);
    result.SuffixBounces = direction.w;
    result.TailCoefficient = asfloat(LoadWord(pixel, RADIANCE_WORD, isSecond).xyz);
    return result;
}

void StoreReservoir(RayLaunch launch, bool isSecond, Reservoir reservoir, RayQueryContext queries)
{
    uint pixel = GetPixelIndex(launch);
    StoreWord(pixel,
              0,
              isSecond,
              uint4(reservoir.TriangleIndex, asuint(reservoir.Barycentric), asuint(reservoir.Weight)));
    StoreWord(pixel,
              1,
              isSecond,
              uint4(reservoir.CandidateCount, reservoir.SupportTriangleIndex, asuint(reservoir.SupportBarycentric)));
    StoreWord(pixel,
              2,
              isSecond,
              queries.HasFailed == true ? uint4(queries.FailureReason, 0, 0, FAILED_LAYOUT_TAG)
                                        : uint4(asuint(reservoir.SupportViewWorld), LAYOUT_TAG));
    StoreWord(pixel,
              IDENTITY_WORD,
              isSecond,
              uint4(reservoir.SceneGeneration,
                    reservoir.ExtentPixel.x | (reservoir.ExtentPixel.y << EXTENT_SHIFT),
                    reservoir.SampleIndex));
    StoreWord(pixel, DIRECTION_WORD, isSecond, uint4(asuint(reservoir.DirectionWorld), reservoir.SuffixBounces));
    StoreWord(pixel, RADIANCE_WORD, isSecond, uint4(asuint(reservoir.TailCoefficient), 0));
}

bool HasIdentity(Reservoir source, SceneContext scene, RayLaunch launch)
{
    return source.LayoutTag == LAYOUT_TAG && all(source.SceneGeneration == scene.Generation) == true &&
           all(source.ExtentPixel == launch.ExtentPixel) == true && source.CandidateCount <= CRIS_HISTORY_LIMIT &&
           isfinite(source.Weight) == true && source.Weight >= 0 && source.SuffixBounces <= CRIS_SUFFIX_BOUNCES &&
           all(isfinite(source.DirectionWorld)) == true && all(isfinite(source.TailCoefficient)) == true &&
           all(source.TailCoefficient >= 0) == true &&
           (source.SuffixBounces == 0 || any(source.TailCoefficient > 0) == false ||
            abs(dot(source.DirectionWorld, source.DirectionWorld) - 1) <= SCENE_UNIT_TOLERANCE);
}

bool IsCurrentRecord(Reservoir source, SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (source.SampleIndex == launch.SampleIndex && HasIdentity(source, scene, launch) == true)
        return true;
    FailSceneQuery(queries,
                   source.SampleIndex == launch.SampleIndex && source.LayoutTag == FAILED_LAYOUT_TAG
                       ? source.FailureReason
                       : LUDUS_SCENE_FAILURE_STORED_RECORD);
    return false;
}

// Thanks to the Slang authors, "noinline", core module reference,
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html .
// Value-only boundaries keep layered walks shared in emitted Metal/SPIR-V instead of duplicating
// them at every connection and continuation site. No forwarded inout query context crosses here.
[noinline] float3 EvaluateSuffixBsdf(MaterialSample material,
                                     float3 viewWorld,
                                     float3 lightWorld,
                                     CoatingRandomContext random,
                                     bool shouldUseLayeredBsdf)
{
    float3 value;
    float density;
    if (shouldUseLayeredBsdf == true)
        EvaluateStandardBsdf(material, viewWorld, lightWorld, random, value, density);
    else
        EvaluateStandardBsdf(material, viewWorld, lightWorld, value, density);
    return value;
}

[noinline] StandardBsdfTransportSample
SampleSuffixBsdf(MaterialSample material, float3 viewWorld, float3 random, CoatingRandomContext materialRandom)
{
    return SampleStandardBsdfTransport(material, viewWorld, random, materialRandom);
}

/// One BSDF-sampled segment. Its emission is handled by primary direct lighting, never counted twice.
Prefix TracePrefix(SceneContext scene, RayLaunch launch, SurfaceHit primary, inout RayQueryContext queries)
{
    Prefix result = (Prefix)0;
    result.Hit = primary;
    result.ViewWorld = -launch.CameraRay.DirectionWorld;
    result.Throughput = float3(1);
    result.MaterialRandom = MakeMaterialRandom(launch, GATHER_MATERIAL_EVENT);
    result.IsValid = primary.IsValid;
    if (result.IsValid == false || CRIS_PREFIX_BOUNCES == 0)
        return result;
    MaterialSample material = EvaluateMaterial(scene, primary);
    if (material.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        result.IsValid = false;
        return result;
    }
    float3 random = float3(SamplePassRandom(launch, 0), SamplePassRandom(launch, 1), SamplePassRandom(launch, 2));
    StandardBsdfTransportSample step =
        SampleSuffixBsdf(material, result.ViewWorld, random, MakeMaterialRandom(launch, PREFIX_MATERIAL_EVENT));
    result.IsValid = false;
    if (all(isfinite(step.Weight)) == false || any(step.Weight < 0) == true ||
        all(isfinite(step.DirectionWorld)) == false || isfinite(step.Pdf) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        return result;
    }
    if (step.HasSample == false || any(step.Weight > 0) == false)
        return result;
    float3 directionWorld = NormalizeOrZero(step.DirectionWorld);
    if (any(directionWorld != 0) == false)
        return result;
    // Coatings provide a path ratio from their internal walk; never reconstruct f*cos/pdf.
    result.Throughput = step.Weight;
    result.ViewWorld = -directionWorld;
    result.IsValid = TraceClosest(scene,
                                  queries,
                                  SpawnSurfaceRay(primary, directionWorld, MAXIMUM_TRACE_DISTANCE_WORLD),
                                  result.Hit) == SCENE_QUERY_SURFACE;
    return result;
}

Prefix LoadSupport(SceneContext scene, Reservoir reservoir)
{
    Prefix result = (Prefix)0;
    if (reservoir.SupportTriangleIndex != SCENE_INVALID_INDEX)
    {
        result.Hit = GetSurfaceHit(scene, reservoir.SupportTriangleIndex, reservoir.SupportBarycentric);
        result.ViewWorld = reservoir.SupportViewWorld;
        result.IsValid = result.Hit.IsValid;
    }
    return result;
}

/// RGB suffix contribution per unit world area, reevaluating both connection BSDFs for this prefix.
float3 EvaluateAreaContributionBody(SceneContext scene,
                                    Prefix prefix,
                                    Reservoir suffix,
                                    bool shouldUseLayeredBsdf,
                                    inout RayQueryContext queries)
{
    SurfaceHit endpoint = GetSurfaceHit(scene, suffix.TriangleIndex, suffix.Barycentric);
    if (endpoint.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    if (prefix.IsValid == false || (suffix.SuffixBounces > 0 && any(suffix.TailCoefficient > 0) == false))
        return float3(0);
    float3 offsetWorld = endpoint.PositionWorld - prefix.Hit.PositionWorld;
    float distanceSquaredWorld = dot(offsetWorld, offsetWorld);
    if (distanceSquaredWorld <= 0)
        return float3(0);
    float3 directionWorld = offsetWorld * rsqrt(distanceSquaredWorld);
    MaterialSample material = EvaluateMaterial(scene, prefix.Hit);
    float3 bsdfValue =
        EvaluateSuffixBsdf(material, prefix.ViewWorld, directionWorld, prefix.MaterialRandom, shouldUseLayeredBsdf);
    float receiverCosine = max(dot(NormalizeOrZero(material.ShadingNormalWorld), directionWorld), 0);
    float emitterCosine = abs(dot(NormalizeOrZero(endpoint.GeometricNormalWorld), -directionWorld));
    if (material.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    float3 suffixRadiance = EvaluateEmission(scene, endpoint, -directionWorld);
    if (suffix.SuffixBounces > 0)
    {
        MaterialSample suffixMaterial = EvaluateMaterial(scene, endpoint);
        if (suffixMaterial.IsValid == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
            return float3(0);
        }
        CoatingRandomContext suffixRandom = prefix.MaterialRandom;
        suffixRandom.Address.Dimension += SUFFIX_GATHER_EVENT_OFFSET;
        float3 suffixBsdf = EvaluateSuffixBsdf(suffixMaterial,
                                               -directionWorld,
                                               suffix.DirectionWorld,
                                               suffixRandom,
                                               shouldUseLayeredBsdf);
        suffixRadiance = suffixBsdf * suffix.TailCoefficient;
    }
    float3 contribution = bsdfValue * suffixRadiance * (receiverCosine * emitterCosine / distanceSquaredWorld);
    if (all(isfinite(contribution)) == false || any(contribution < 0) == true)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
        return float3(0);
    }
    return contribution;
}

// Slang preserves the shared boundary when the incoming context is a value and the updated context
// is a distinct output. Forwarding inout through a noinline function can still expand the body.
[noinline] float3 EvaluateAreaContributionOutOfLine(SceneContext scene,
                                                    Prefix prefix,
                                                    Reservoir suffix,
                                                    bool shouldUseLayeredBsdf,
                                                    RayQueryContext queriesIn,
                                                    out RayQueryContext outQueries)
{
    RayQueryContext queries = queriesIn;
    float3 value = EvaluateAreaContributionBody(scene, prefix, suffix, shouldUseLayeredBsdf, queries);
    outQueries = queries;
    return value;
}

float3 EvaluateAreaContribution(SceneContext scene,
                                Prefix prefix,
                                Reservoir suffix,
                                bool shouldUseLayeredBsdf,
                                inout RayQueryContext queries)
{
    RayQueryContext resultQueries;
    float3 value =
        EvaluateAreaContributionOutOfLine(scene, prefix, suffix, shouldUseLayeredBsdf, queries, resultQueries);
    queries = resultQueries;
    return value;
}

// A prefix-independent envelope keeps retargeting between freshly sampled supporting prefixes
// bounded. A tiny absolute floor alone allows a near-zero source target to acquire an enormous
// weight and persist as a rare bright tail across many frames. Only the SELECTION target is bounded;
// the final radiance and contribution weights are never clamped.
[noinline] float EvaluateIntrinsicTarget(SceneContext scene, Reservoir suffix)
{
    float3 coefficient =
        suffix.SuffixBounces > 0
            ? suffix.TailCoefficient
            : EvaluateMaterial(scene, GetSurfaceHit(scene, suffix.TriangleIndex, suffix.Barycentric)).Emission;
    return max(dot(coefficient, LUMINANCE_WEIGHTS), MINIMUM_AREA_TARGET);
}

float EvaluateTarget(SceneContext scene, Prefix support, Reservoir suffix, inout RayQueryContext queries)
{
    float intrinsic = EvaluateIntrinsicTarget(scene, suffix);
    float connection = dot(EvaluateAreaContribution(scene, support, suffix, false, queries), LUMINANCE_WEIGHTS);
    // Stable connection/(intrinsic+connection), even when both finite inputs approach float's limit.
    float ratio = connection > intrinsic ? 1 / (1 + intrinsic / connection)
                                         : (connection / intrinsic) / (1 + connection / intrinsic);
    return intrinsic * (1 + ratio);
}

void StreamCandidate(Reservoir suffix,
                     float mass,
                     uint count,
                     float coin,
                     inout Reservoir inoutResult,
                     inout float inoutMassSum,
                     inout RayQueryContext queries)
{
    inoutResult.CandidateCount += count;
    bool shouldSelect;
    if (TryStreamConditionalMass(mass, coin, inoutMassSum, shouldSelect) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        return;
    }
    if (shouldSelect == true)
    {
        inoutResult.TriangleIndex = suffix.TriangleIndex;
        inoutResult.Barycentric = suffix.Barycentric;
        inoutResult.SuffixBounces = suffix.SuffixBounces;
        inoutResult.DirectionWorld = suffix.DirectionWorld;
        inoutResult.TailCoefficient = suffix.TailCoefficient;
    }
}

void MergeReservoir(SceneContext scene,
                    RayLaunch launch,
                    Prefix support,
                    Reservoir source,
                    uint dimension,
                    inout Reservoir inoutResult,
                    inout float inoutMassSum,
                    inout RayQueryContext queries)
{
    if (source.CandidateCount == 0)
        return;
    if (source.TriangleIndex == SCENE_INVALID_INDEX)
    {
        inoutResult.CandidateCount += source.CandidateCount;
        return;
    }
    float target = EvaluateTarget(scene, support, source, queries);
    StreamCandidate(source,
                    ComputeConditionalMass(target, source.Weight, source.CandidateCount),
                    source.CandidateCount,
                    SamplePassRandom(launch, dimension),
                    inoutResult,
                    inoutMassSum,
                    queries);
}

void FinalizeReservoir(SceneContext scene,
                       Prefix support,
                       float massSum,
                       inout Reservoir inoutResult,
                       inout RayQueryContext queries)
{
    float target =
        inoutResult.TriangleIndex != SCENE_INVALID_INDEX ? EvaluateTarget(scene, support, inoutResult, queries) : 0;
    inoutResult.Weight = ComputeConditionalWeight(massSum, target, inoutResult.CandidateCount);
    // Capping is a confidence policy; it must never change the finalized contribution weight.
    inoutResult.CandidateCount = min(inoutResult.CandidateCount, CRIS_HISTORY_LIMIT);
    if (isfinite(inoutResult.Weight) == false || inoutResult.Weight < 0)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
}

/// Final gather: integrate the stored suffix with a fresh prefix and its own throughput. Visibility is
/// evaluated HERE and excluded from the selection target, keeping the light-area domain prefix independent.
float3 GatherSuffix(SceneContext scene, Prefix prefix, Reservoir reservoir, inout RayQueryContext queries)
{
    if (prefix.IsValid == false || reservoir.TriangleIndex == SCENE_INVALID_INDEX || reservoir.Weight <= 0)
        return float3(0);
    SurfaceHit endpoint = GetSurfaceHit(scene, reservoir.TriangleIndex, reservoir.Barycentric);
    float3 contribution = EvaluateAreaContribution(scene, prefix, reservoir, true, queries);
    if (any(contribution > 0) == false)
        return float3(0);
    float3 offsetWorld = endpoint.PositionWorld - prefix.Hit.PositionWorld;
    LightSample light = (LightSample)0;
    light.TriangleIndex = reservoir.TriangleIndex;
    light.Barycentric = reservoir.Barycentric;
    light.PositionWorld = endpoint.PositionWorld;
    light.DistanceWorld = length(offsetWorld);
    light.DirectionWorld = offsetWorld / light.DistanceWorld;
    light.HasFiniteEndpoint = true;
    light.HasSample = true;
    return TestVisibility(scene, queries, prefix.Hit, light) == SCENE_QUERY_MISS
               ? prefix.Throughput * contribution * reservoir.Weight
               : float3(0);
}
