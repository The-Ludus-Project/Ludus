#include "math.slang"
#include "raster_bindings.slang"
#include "renderer_gpu_preview_layout.h"
#include "renderer_surface_functions.slang"

// GPU preview ports Ludus's frozen RGB transport, not the CPU transport loop.
// Thanks to Khronos Group, Vulkan Ray Traversal, Ray Query,
// https://docs.vulkan.org/spec/latest/chapters/raytraversal.html: native traversal
// produces candidates; the shared fixed-mip material alpha test accepts them.
// Thanks to John K. Salmon et al., "Parallel Random Numbers: As Easy as 1, 2, 3",
// SC11 (2011), section 4.3, for Philox4x32-10
// (https://www.thesalmons.org/john/random123/papers/random123sc11.pdf).
// Thanks to Eric Heitz, "Sampling the GGX Distribution of Visible Normals",
// JCGT 7(4) (2018), sections 3-4/Appendix B, visible-GGX
// (https://jcgt.org/published/0007/04/01/paper.pdf), Google, "Physically Based
// Rendering in Filament", sections 4.4/5.2.2
// (https://google.github.io/filament/main/filament.html), and Matt Pharr, Wenzel
// Jakob and Greg Humphreys, "Physically Based Rendering", 4e, section 13.4
// (https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer)
// for the shared reflection, MIS and roulette conventions. Original ports from
// reference_bsdf.cpp/reference_path.cpp; renderer-systems.md owns quality limits.
LUDUS_SHADER_BINDING(0) cbuffer Parameters
{
    column_major float4x4 ClipToWorld;
    float4 CameraPositionWorld;
    uint4 Dimensions; // width, height, depth, roulette
    uint4 Offsets;    // nodes, order, emitters, surfaces
    uint4 Counts;     // triangles, nodes, emitters, analytic lights
    uint4 Sampling;   // key lo, key hi, flags (reverse/infinite/center), target
    float4 Environment;
    float4
        Options; // secondary range, exposure (zero bypasses graph ToneMap), material count bits, scene data group bits
    float4 BoundsMinimumWorld;
    float4 BoundsMaximumWorld;
    uint4 SurfaceLayout; // surface stride, analytic offset, private kernel ABI, see renderer_gpu_preview_layout.h,
                         // reserved
};
LUDUS_SHADER_BINDING(1) StructuredBuffer<uint4> sceneWords;
float4 SceneValue(uint index)
{
    return asfloat(sceneWords[index]);
}
LUDUS_SHADER_BINDING(2) RWStructuredBuffer<float4> accumulation; // sum/count, compensation/error
LUDUS_SHADER_BINDING(LUDUS_GPU_PREVIEW_COLORS_BINDING)
RWStructuredBuffer<float4> colors; // Encoded RGB and binary32-exact pixel index.
LUDUS_SHADER_BINDING(LUDUS_GPU_PREVIEW_TEXELS_BINDING) StructuredBuffer<uint4> textureWords;
#ifndef LUDUS_SOFTWARE_RAY_TRAVERSAL
LUDUS_SHADER_BINDING(LUDUS_GPU_PREVIEW_RAY_SCENE_BINDING) RaytracingAccelerationStructure rayScene;
#endif
// Mirrors the CPU reference path's addresses: Scope is the 32-bit pixel index,
// Event is the sample and dimensions follow LUDUS_PATH_RANDOM_* in
// renderer_gpu_preview_layout.h. Host admission bounds depth, so every
// dimension stays inside addressed-random layout v1.
RandomAddress PixelRandomAddress(uint pixel, uint sample, uint dimension)
{
    return MakeRandomAddress(uint2(pixel, 0), sample, dimension);
}
float Random01(uint pixel, uint sample, uint dimension)
{
    return SampleRandomFloat01(Sampling.xy, PixelRandomAddress(pixel, sample, dimension));
}
float3 PositionWorld(uint t, float2 uv)
{
    uint b = t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    return SceneValue(b).xyz * (1 - uv.x - uv.y) + SceneValue(b + 1).xyz * uv.x + SceneValue(b + 2).xyz * uv.y;
}
float2 TexcoordUv(uint t, float2 uv)
{
    uint b = t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    return SceneValue(b + 6).xy * (1 - uv.x - uv.y) + SceneValue(b + 6).zw * uv.x + SceneValue(b + 7).xy * uv.y;
}
float4 Surface(uint t, uint field)
{
    uint i = asuint(SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS).w);
    return SceneValue(Offsets.w + i * SurfaceLayout.x + field);
}
int Address(int i, int extent, bool repeat)
{
    if (!repeat)
        return clamp(i, 0, extent - 1);
    int v = i % extent;
    return v < 0 ? v + extent : v;
}
float4 Texel(uint t, int x, int y, uint map)
{
    uint4 layout = asuint(Surface(t, map == 2 ? 8 : map == 1 ? 7 : 5));
    uint2 size = map == 2 ? asuint(Surface(t, 8).zw) : map == 1 ? asuint(Surface(t, 6).yz) : asuint(Surface(t, 1).zw);
    uint flags = layout.y;
    x = Address(x, int(size.x), (flags & 2) != 0);
    y = Address(y, int(size.y), (flags & 4) != 0);
    uint index = uint(y) * size.x + uint(x);
    uint word = textureWords[layout.x + (index >> 2)][index & 3];
    float4 c = float4(word & 255, (word >> 8) & 255, (word >> 16) & 255, word >> 24) / 255.0;
    if ((flags & 8) != 0)
        c.xyz = float3(DecodeSrgb(c.x), DecodeSrgb(c.y), DecodeSrgb(c.z));
    return c;
}
// Thanks to the Slang authors, core module noinline attribute reference:
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html
// Keep the four-texel bilinear filter out of line in SPIR-V. Slang's default
// optimizer otherwise copies it into every material, emission and alpha-test
// site, which made translated ray-graph Metal sources exceed the 2 MiB stage
// artifact limit. SPIRV-Cross still marks the MSL function always_inline, and
// Slang emits no inlining hint for Metal, WGSL or GLSL source; Vulkan drivers
// make their own inlining decision when compiling the SPIR-V.
[noinline] float4 Texture(uint t, float2 uv, uint map = 0) {
    if ((map == 2 ? asuint(Surface(t, 8).z) : asuint(Surface(t, map == 1 ? 6 : 4).w)) == 0)
        return float4(1);
    float4 transform = Surface(t, 3);
    uv = uv * transform.xy + transform.zw;
    uint flags = asuint(Surface(t, map == 2 ? 8 : map == 1 ? 7 : 5).y);
    uint2 size = map == 2 ? asuint(Surface(t, 8).zw) : map == 1 ? asuint(Surface(t, 6).yz) : asuint(Surface(t, 1).zw);
    uv.x = (flags & 2) != 0 ? frac(uv.x) : saturate(uv.x);
    uv.y = (flags & 4) != 0 ? frac(uv.y) : saturate(uv.y);
    float2 p = uv * float2(size);
    if ((flags & 1) == 0)
        return Texel(t, int(floor(p.x)), int(floor(p.y)), map);
    p -= .5;
    int2 cell = int2(floor(p));
    float2 f = frac(p);
    return lerp(lerp(Texel(t, cell.x, cell.y, map), Texel(t, cell.x + 1, cell.y, map), f.x),
                lerp(Texel(t, cell.x, cell.y + 1, map), Texel(t, cell.x + 1, cell.y + 1, map), f.x),
                f.y);
}

struct HitMaterial
{
    uint Triangle;
#ifndef LUDUS_GPU_PREVIEW_CACHE_HIT
    float2 Barycentric;
#else
    uint SurfaceAddress;
    float2 TexcoordUv;
#endif
};
HitMaterial LoadHitMaterial(uint triangle, float2 barycentric)
{
    HitMaterial material;
    material.Triangle = triangle;
#ifndef LUDUS_GPU_PREVIEW_CACHE_HIT
    material.Barycentric = barycentric;
#else
    uint index = asuint(SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS).w);
    material.SurfaceAddress = Offsets.w + index * SurfaceLayout.x;
    material.TexcoordUv = TexcoordUv(triangle, barycentric);
#endif
    return material;
}
float4 ReadHitSurface(in HitMaterial material, uint field)
{
#ifndef LUDUS_GPU_PREVIEW_CACHE_HIT
    return Surface(material.Triangle, field);
#else
    return SceneValue(material.SurfaceAddress + field);
#endif
}
float4 SampleHitTexture(in HitMaterial material, uint map = 0)
{
#ifndef LUDUS_GPU_PREVIEW_CACHE_HIT
    return Texture(material.Triangle, TexcoordUv(material.Triangle, material.Barycentric), map);
#else
    return Texture(material.Triangle, material.TexcoordUv, map);
#endif
}

struct Hit
{
    uint Triangle;
    float DistanceWorld;
    float2 Barycentric;
};
float4 InterpolateTriangle(uint triangle, uint first, in float2 barycentric);
float4 InterpolateTriangleColor(uint triangle, in float2 barycentric);
#ifdef LUDUS_RAY_PATH_CAPTURE
// Diagnostic ray path capture; see render-graph-authoring.md#ray-path-inspection-proposed.
// Selected invocations append each Trace() call to this buffer. Capture writes only the
// buffer and the private statics below: it consumes no random dimension or query budget
// and never changes a transport value, so color and accumulation match the plain kernel.
LUDUS_SHADER_BINDING(LUDUS_RAY_PATH_CAPTURE_BINDING) RWStructuredBuffer<uint> rayPathCapture;
static bool isRayPathSelected = false;
static bool hasRayPathFailure = false;
static uint rayPathSummary = 0;
static uint2 rayPathPixel = 0;
static uint rayPathSample = 0;
static uint rayPathPass = 0;
static uint rayPathSequence = 0;
static uint rayPathRejected = 0;
/// Select this invocation when the header's pixel rectangle and sample range contain it.
void BeginRayPathCapture(uint2 positionPixel, uint sampleIndex)
{
    isRayPathSelected = false;
    hasRayPathFailure = false;
    rayPathSequence = 0;
    rayPathRejected = 0;
    if (rayPathCapture[LUDUS_RAY_PATH_HEADER_VERSION_WORD] != LUDUS_RAY_PATH_CAPTURE_VERSION)
        return;
    uint2 firstPixel =
        uint2(rayPathCapture[LUDUS_RAY_PATH_HEADER_PIXEL_X_WORD], rayPathCapture[LUDUS_RAY_PATH_HEADER_PIXEL_Y_WORD]);
    uint2 extentPixel =
        uint2(rayPathCapture[LUDUS_RAY_PATH_HEADER_WIDTH_WORD], rayPathCapture[LUDUS_RAY_PATH_HEADER_HEIGHT_WORD]);
    uint firstSample = rayPathCapture[LUDUS_RAY_PATH_HEADER_FIRST_SAMPLE_WORD];
    uint sampleCount = rayPathCapture[LUDUS_RAY_PATH_HEADER_SAMPLE_COUNT_WORD];
    if (any(positionPixel < firstPixel) || any(positionPixel - firstPixel >= extentPixel) ||
        sampleIndex < firstSample || sampleIndex - firstSample >= sampleCount)
        return;
    uint2 offsetPixel = positionPixel - firstPixel;
    rayPathPixel = positionPixel;
    rayPathSample = sampleIndex;
    rayPathPass = (Sampling.z >> LUDUS_GRAPH_PASS_INDEX_SHIFT) & LUDUS_GRAPH_PASS_INDEX_MASK;
    rayPathSummary = ((offsetPixel.y * extentPixel.x + offsetPixel.x) * sampleCount + sampleIndex - firstSample) *
                         LUDUS_RAY_PATH_CAPTURE_PASS_COUNT +
                     rayPathPass;
    isRayPathSelected = true;
}
/// Reserve and write one event record; a full buffer keeps earlier records and counts the drop.
void AppendRayPathEvent(uint kind,
                        float3 originWorld,
                        float3 directionWorld,
                        float minimumDistanceWorld,
                        float maximumDistanceWorld,
                        uint outcome,
                        float hitDistanceWorld,
                        uint triangle)
{
    uint rejected = rayPathRejected;
    rayPathRejected = 0;
    if (isRayPathSelected == false)
        return;
    uint sequence = rayPathSequence;
    rayPathSequence += 1;
    if (outcome == LUDUS_RAY_PATH_OUTCOME_FAILURE)
        hasRayPathFailure = true;
    uint record = 0;
    InterlockedAdd(rayPathCapture[LUDUS_RAY_PATH_HEADER_APPENDED_WORD], 1, record);
    if (record >= rayPathCapture[LUDUS_RAY_PATH_HEADER_RECORD_CAPACITY_WORD])
    {
        InterlockedAdd(rayPathCapture[LUDUS_RAY_PATH_HEADER_DROPPED_WORD], 1);
        return;
    }
    uint summaryCount = rayPathCapture[LUDUS_RAY_PATH_HEADER_WIDTH_WORD] *
                        rayPathCapture[LUDUS_RAY_PATH_HEADER_HEIGHT_WORD] *
                        rayPathCapture[LUDUS_RAY_PATH_HEADER_SAMPLE_COUNT_WORD] * LUDUS_RAY_PATH_CAPTURE_PASS_COUNT;
    uint base = LUDUS_RAY_PATH_HEADER_WORDS + summaryCount * LUDUS_RAY_PATH_SUMMARY_WORDS +
                record * LUDUS_RAY_PATH_RECORD_WORDS;
    bool isHit = outcome == LUDUS_RAY_PATH_OUTCOME_HIT && triangle < Counts.x;
    uint triangleWord = triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_PIXEL_X_WORD] = rayPathPixel.x;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_PIXEL_Y_WORD] = rayPathPixel.y;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_SAMPLE_WORD] = rayPathSample;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_PASS_WORD] = rayPathPass;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_SEQUENCE_WORD] = sequence;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_KIND_WORD] = kind;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_OUTCOME_WORD] = outcome;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_REJECTED_WORD] = rejected;
    for (uint axis = 0; axis < 3; ++axis)
    {
        rayPathCapture[base + LUDUS_RAY_PATH_RECORD_ORIGIN_WORD + axis] = asuint(originWorld[axis]);
        rayPathCapture[base + LUDUS_RAY_PATH_RECORD_DIRECTION_WORD + axis] = asuint(directionWorld[axis]);
    }
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_MIN_DISTANCE_WORD] = asuint(minimumDistanceWorld);
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_MAX_DISTANCE_WORD] = asuint(maximumDistanceWorld);
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_HIT_DISTANCE_WORD] = asuint(hitDistanceWorld);
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_TRIANGLE_WORD] =
        isHit == true ? triangle : LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_INSTANCE_WORD] =
        isHit == true ? asuint(SceneValue(triangleWord + LUDUS_GRAPH_INSTANCE_WORD).w)
                      : LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_PRIMITIVE_WORD] =
        isHit == true ? asuint(SceneValue(triangleWord + 2).w) : LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    uint2 sourceId = isHit == true ? asuint(SceneValue(triangleWord + LUDUS_GRAPH_SOURCE_WORD).zw) : uint2(0);
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_SOURCE_LOW_WORD] = sourceId.x;
    rayPathCapture[base + LUDUS_RAY_PATH_RECORD_SOURCE_HIGH_WORD] = sourceId.y;
}
/// Write the selected invocation's summary: final sequence count, failure and sample radiance.
void EndRayPathCapture(float3 radiance)
{
    if (isRayPathSelected == false)
        return;
    bool hasFailed = hasRayPathFailure == true || any(isfinite(radiance) == false) || any(radiance < 0);
    uint base = LUDUS_RAY_PATH_HEADER_WORDS + rayPathSummary * LUDUS_RAY_PATH_SUMMARY_WORDS;
    rayPathCapture[base + LUDUS_RAY_PATH_SUMMARY_SAMPLE_WORD] = rayPathSample;
    rayPathCapture[base + LUDUS_RAY_PATH_SUMMARY_PASS_WORD] = rayPathPass;
    rayPathCapture[base + LUDUS_RAY_PATH_SUMMARY_EVENT_COUNT_WORD] = rayPathSequence;
    for (uint channel = 0; channel < 3; ++channel)
        rayPathCapture[base + LUDUS_RAY_PATH_SUMMARY_RADIANCE_WORD + channel] = asuint(radiance[channel]);
    rayPathCapture[base + LUDUS_RAY_PATH_SUMMARY_FLAGS_WORD] =
        LUDUS_RAY_PATH_SUMMARY_WRITTEN_FLAG | (hasFailed == true ? LUDUS_RAY_PATH_SUMMARY_FAILED_FLAG : 0u);
    isRayPathSelected = false;
}
#endif
#ifndef LUDUS_SOFTWARE_RAY_TRAVERSAL
void TraceNativeQuery(float3 originWorld,
                      float3 directionWorld,
                      float nearDistanceWorld,
                      float farDistanceWorld,
                      bool isVisibility,
                      inout RayQuery<RAY_FLAG_NONE> inoutQuery)
{
    RayDesc ray;
    ray.Origin = originWorld;
    ray.Direction = directionWorld;
    ray.TMin = nearDistanceWorld;
    ray.TMax = farDistanceWorld;
    // Keep closest-hit acceptance on the existing path: automatic opaque closest
    // hits did not meet the native CPU-oracle image tolerance.
    // Visibility needs only hit/miss and can bypass opaque candidates natively.
    uint flags = (Sampling.z & LUDUS_GPU_PREVIEW_OPAQUE_FLAG) != 0 ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_FORCE_NON_OPAQUE;
    if (isVisibility == true)
        flags = ((Sampling.z & LUDUS_GPU_PREVIEW_OPAQUE_FLAG) != 0          ? RAY_FLAG_FORCE_OPAQUE
                 : (Sampling.z & LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG) != 0 ? RAY_FLAG_NONE
                                                                            : RAY_FLAG_FORCE_NON_OPAQUE) |
                RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
    inoutQuery.TraceRayInline(rayScene, flags, LUDUS_GPU_PREVIEW_RAY_MASK, ray);
    while (inoutQuery.Proceed() == true)
    {
        if (inoutQuery.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        uint candidatePrimitive = inoutQuery.CandidatePrimitiveIndex();
        if ((Sampling.z & LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG) != 0)
        {
            if (inoutQuery.CandidateGeometryIndex() == 0)
            {
                inoutQuery.CommitNonOpaqueTriangleHit();
                continue;
            }
            candidatePrimitive += asuint(SceneValue(Offsets.y).y);
        }
        uint triangle = asuint(SceneValue(Offsets.y + candidatePrimitive).x);
        float2 barycentric = inoutQuery.CandidateTriangleBarycentrics();
        if (asuint(Surface(triangle, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) != 0 &&
            Texture(triangle, TexcoordUv(triangle, barycentric)).w * Surface(triangle, 2).w *
                    InterpolateTriangleColor(triangle, barycentric).w <
                Surface(triangle, 1).y)
        {
#    ifdef LUDUS_RAY_PATH_CAPTURE
            rayPathRejected += 1;
#    endif
            continue;
        }
        inoutQuery.CommitNonOpaqueTriangleHit();
    }
}
Hit TraceNative(float3 originWorld,
                float3 directionWorld,
                float nearDistanceWorld,
                float farDistanceWorld,
                bool isVisibility = false)
{
    Hit hit;
    hit.Triangle = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    hit.DistanceWorld = farDistanceWorld;
    hit.Barycentric = 0;
    RayQuery<RAY_FLAG_NONE> query;
    TraceNativeQuery(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility, query);
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        uint primitive = query.CommittedPrimitiveIndex();
        if ((Sampling.z & LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG) != 0)
        {
            if (query.CommittedGeometryIndex() != 0)
                primitive += asuint(SceneValue(Offsets.y).y);
        }
        hit.Triangle = asuint(SceneValue(Offsets.y + primitive).x);
        hit.DistanceWorld = query.CommittedRayT();
        hit.Barycentric = query.CommittedTriangleBarycentrics();
    }
    return hit;
}
// Visibility consumes only committed status; candidate attributes remain necessary
// for exactly the same masked-coverage test used by closest-hit traversal.
bool TraceNativeVisibility(float3 originWorld, float3 directionWorld, float nearDistanceWorld, float farDistanceWorld)
{
    RayQuery<RAY_FLAG_NONE> query;
    TraceNativeQuery(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, true, query);
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#endif
#if defined(LUDUS_GRAPH_TRAVERSAL_STRATEGIES) || defined(LUDUS_SOFTWARE_RAY_TRAVERSAL)
#    include "ray_triangle_loop.slang"
#endif
Hit TraceWorld(float3 originWorld,
               float3 directionWorld,
               float nearDistanceWorld,
               float farDistanceWorld,
               bool isVisibility)
{
#ifdef LUDUS_SOFTWARE_RAY_TRAVERSAL
    return TraceTriangleLoop(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);
#else
#    ifdef LUDUS_GRAPH_TRAVERSAL_STRATEGIES
    if (GetGraphTraversalStrategy() == 1)
        return TraceTriangleLoop(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);
#    endif
    return TraceNative(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);
#endif
}
/// Every scene traversal of the kernel; capture variants record the submitted ray and its result.
Hit Trace(float3 originWorld,
          float3 directionWorld,
          float nearDistanceWorld,
          float farDistanceWorld,
          bool isVisibility = false)
{
#ifdef LUDUS_RAY_PATH_CAPTURE
    rayPathRejected = 0;
    Hit hit = TraceWorld(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);
    bool isValidRay = all(isfinite(originWorld)) == true && all(isfinite(directionWorld)) == true &&
                      isfinite(nearDistanceWorld) == true && isfinite(farDistanceWorld) == true &&
                      nearDistanceWorld >= 0 && farDistanceWorld >= nearDistanceWorld;
    uint outcome = isValidRay == false || hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE
                       ? LUDUS_RAY_PATH_OUTCOME_FAILURE
                   : hit.Triangle == LUDUS_GPU_PREVIEW_INVALID_TRIANGLE ? LUDUS_RAY_PATH_OUTCOME_MISS
                                                                        : LUDUS_RAY_PATH_OUTCOME_HIT;
    AppendRayPathEvent(isVisibility == true ? LUDUS_RAY_PATH_KIND_VISIBILITY : LUDUS_RAY_PATH_KIND_CLOSEST,
                       originWorld,
                       directionWorld,
                       nearDistanceWorld,
                       farDistanceWorld,
                       outcome,
                       hit.DistanceWorld,
                       hit.Triangle);
    return hit;
#else
    return TraceWorld(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, isVisibility);
#endif
}
// Three outcomes preserve the existing software traversal failure contract.
struct VisibilityResult
{
    bool HasFailed;
    bool IsOccluded;
};
VisibilityResult ResolveVisibility(in Hit hit)
{
    VisibilityResult result;
    result.HasFailed = hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE;
    result.IsOccluded = result.HasFailed == false && hit.Triangle != LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    return result;
}
VisibilityResult
TraceVisibility(float3 originWorld, float3 directionWorld, float nearDistanceWorld, float farDistanceWorld)
{
// Capture requires committed hit identity/distance for visibility event records.
#if defined(LUDUS_RAY_PATH_CAPTURE) || defined(LUDUS_GPU_PREVIEW_VISIBILITY_FULL_HIT) ||                               \
    defined(LUDUS_SOFTWARE_RAY_TRAVERSAL)
    return ResolveVisibility(Trace(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, true));
#else
#    ifdef LUDUS_GRAPH_TRAVERSAL_STRATEGIES
    if (GetGraphTraversalStrategy() == 1)
        return ResolveVisibility(
            TraceTriangleLoop(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, true));
#    endif
    VisibilityResult result;
    result.HasFailed = false;
    result.IsOccluded = TraceNativeVisibility(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld);
    return result;
#endif
}
// Thanks to Dirk Gerrit van Antwerpen, "Solving Self-Intersection Artifacts in
// DirectX Raytracing", NVIDIA Technical Blog (2023), "Error bounds":
// https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/
// Use the reference scene's conservative world-space error allowance, then the
// representable-neighbor offset convention from FoundationMath, not RTX bounds.
float3 OffsetPositionWorld(float3 positionWorld, uint t, float3 directionWorld)
{
    float3 normalWorld = SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).xyz;
    float3 positionErrorWorld = SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 9).xyz;
    float3 offsetWorld =
        normalWorld * dot(abs(normalWorld), positionErrorWorld) * (dot(normalWorld, directionWorld) < 0 ? -1 : 1);
    float3 offsetPositionWorld = positionWorld + offsetWorld;
    for (uint i = 0; i < 3; ++i)
        if (offsetWorld[i] != 0)
        {
            uint b = asuint(offsetPositionWorld[i]);
            if (offsetPositionWorld[i] == 0)
                b = offsetWorld[i] > 0 ? 0x00800000 : 0x80800000;
            else
                b += (offsetPositionWorld[i] > 0) == (offsetWorld[i] > 0) ? 1u : 0xffffffffu;
            offsetPositionWorld[i] = asfloat(b);
        }
    return offsetPositionWorld;
}
float PowerWeight(float a, float b)
{
    if (a <= 0)
        return 0;
    if (b <= 0)
        return 1;
    float m = max(a, b);
    a /= m;
    b /= m;
    return a * a / (a * a + b * b);
}
float AreaPdf(uint t, float3 fromPositionWorld, float3 toPositionWorld, float selection)
{
    if (selection <= 0)
        return 0;
    float3 offsetWorld = toPositionWorld - fromPositionWorld;
    float cosine = abs(dot(SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).xyz, -NormalizeOrZero(offsetWorld)));
    return cosine > 0 ? selection * dot(offsetWorld, offsetWorld) /
                            (SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 1).w * cosine)
                      : 0;
}
// Emitter words: ascending triangle index in X, inclusive integer power CDF in Y.
// Matches the CPU reference's SelectReferenceEmitter/FindReferenceEmitter/ComputeEmitterSelection.
uint EmitterCumulative(uint emitter)
{
    return sceneWords[Offsets.z + emitter].y;
}
uint SelectEmitter(uint value)
{
    uint low = 0;
    uint high = Counts.z - 1;
    while (low < high)
    {
        uint middle = low + (high - low) / 2;
        if (EmitterCumulative(middle) <= value)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}
bool FindEmitter(uint t, out uint outEmitter)
{
    outEmitter = 0;
    uint low = 0;
    uint high = Counts.z;
    while (low < high)
    {
        uint middle = low + (high - low) / 2;
        if (sceneWords[Offsets.z + middle].x < t)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == Counts.z || sceneWords[Offsets.z + low].x != t)
        return false;
    outEmitter = low;
    return true;
}
float EmitterSelection(uint emitter, uint lightCount)
{
    uint total = Counts.z == 0 ? 0 : EmitterCumulative(Counts.z - 1);
    if (lightCount == 0 || emitter >= Counts.z || total == 0)
        return 0;
    uint previous = emitter == 0 ? 0 : EmitterCumulative(emitter - 1);
    return float(EmitterCumulative(emitter) - previous) / float(total) * (float(Counts.z) / float(lightCount));
}
float HitEmitterSelection(uint t, uint lightCount)
{
    uint emitter;
    return FindEmitter(t, emitter) == true ? EmitterSelection(emitter, lightCount) : 0;
}
void Evaluate(float3 color,
              float metallic,
              float roughness,
              float3 normalWorld,
              float3 v,
              float3 l,
              out float3 outValue,
              out float outPdf)
{
    outValue = 0;
    outPdf = 0;
    float nv = dot(normalWorld, v);
    float nl = dot(normalWorld, l);
    if (nv <= 0 || nl <= 0)
        return;
    float3 h = NormalizeOrZero(v + l);
    float nh = saturate(dot(normalWorld, h));
    float vh = saturate(dot(v, h));
    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    float denominator = (1 - nh * nh) + nh * nh * a2;
    float distribution = a2 / (PI * denominator * denominator);
    float gv = sqrt(nv * nv * (1 - a2) + a2);
    float gl = sqrt(nl * nl * (1 - a2) + a2);
    float visibility = .5 / max(nl * gv + nv * gl, 1e-6);
    float3 f0 = .04 * (1 - metallic) + color * metallic;
    float term = 1 - vh;
    float3 fresnel = f0 + (1 - f0) * term * term * term * term * term;
    outValue = fresnel * (distribution * visibility) + color * (1 - fresnel) * ((1 - metallic) / PI);
    float probability = metallic == 1 ? 1 : .5;
    outPdf = probability * distribution / (2 * (nv + gv)) + (1 - probability) * nl / PI;
}
float3 SampleBsdf(float3 normalWorld, float3 viewDirectionWorld, float roughness, float probability, float3 u)
{
    if (dot(normalWorld, viewDirectionWorld) <= 0)
        return float3(0);
    float3 x = NormalizeOrZero(cross(abs(normalWorld.z) < .999 ? float3(0, 0, 1) : float3(0, 1, 0), normalWorld));
    float3 y = cross(normalWorld, x);
    float radius = sqrt(u.y);
    float angle = 2 * PI * u.z;
    float dx = radius * cos(angle);
    float dy = radius * sin(angle);
    float3 local;
    if (u.x >= probability)
        local = float3(dx, dy, sqrt(max(0, 1 - u.y)));
    else
    {
        float3 lv =
            float3(dot(viewDirectionWorld, x), dot(viewDirectionWorld, y), dot(viewDirectionWorld, normalWorld));
        float a = roughness * roughness;
        float3 stretch = NormalizeOrZero(float3(a * lv.xy, lv.z));
        float len = dot(stretch.xy, stretch.xy);
        float3 tangent = len > 0 ? float3(-stretch.y, stretch.x, 0) / sqrt(len) : float3(1, 0, 0);
        float3 bitangent = cross(stretch, tangent);
        float blend = .5 * (1 + stretch.z);
        float wy = (1 - blend) * sqrt(max(0, 1 - dx * dx)) + blend * dy;
        float3 hemisphere = tangent * dx + bitangent * wy + stretch * sqrt(max(0, 1 - dx * dx - wy * wy));
        float3 hl = NormalizeOrZero(float3(a * hemisphere.xy, max(0, hemisphere.z)));
        float3 h = x * hl.x + y * hl.y + normalWorld * hl.z;
        return NormalizeOrZero(h * (2 * dot(viewDirectionWorld, h)) - viewDirectionWorld);
    }
    return NormalizeOrZero(x * local.x + y * local.y + normalWorld * local.z);
}
float4 InterpolateTriangle(uint triangle, uint first, in float2 barycentric)
{
    return SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + first) * (1 - barycentric.x - barycentric.y) +
           SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + first + 1) * barycentric.x +
           SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + first + 2) * barycentric.y;
}
// Match InterpolateReferenceColor: mesh vertex multipliers have an admitted [0,1]
// domain. Preserve invalid arithmetic for the GPU reference's failure reporting.
float4 InterpolateTriangleColor(uint triangle, in float2 barycentric)
{
    float4 color = InterpolateTriangle(triangle, LUDUS_GPU_PREVIEW_VERTEX_COLOR_WORD, barycentric);
    return all(isfinite(color)) == true ? saturate(color) : color;
}
float2 ComputeEnvironmentCoordinates(in float3 directionWorld)
{
    float longitude = atan2(directionWorld.z, directionWorld.x) / (2 * PI);
    return float2(longitude - floor(longitude), atan2(length(directionWorld.xz), directionWorld.y) / PI);
}
float ComputeEnvironmentSolidAngle(uint row, uint width, uint height)
{
    // Subtractive cosine loses FP32 precision near poles; use the sine identity.
    return (2 * PI / float(width)) * 2 * sin(PI * (float(row) + .5) / float(height)) * sin(PI / (2 * float(height)));
}
float4 EvaluateEnvironmentWorld(in float3 directionWorld)
{
    if (SurfaceLayout.w == 0)
        return float4(Environment.xyz, 1 / (4 * PI));
    uint width = asuint(SceneValue(SurfaceLayout.w - 1).x);
    uint height = asuint(SceneValue(SurfaceLayout.w - 1).y);
    uint2 cell =
        min(uint2(ComputeEnvironmentCoordinates(directionWorld) * float2(width, height)), uint2(width - 1, height - 1));
    uint pixel = cell.y * width + cell.x;
    float4 value = SceneValue(SurfaceLayout.w + pixel);
    float previous = pixel == 0 ? 0 : SceneValue(SurfaceLayout.w + pixel - 1).w;
    value.w = (value.w - previous) / ComputeEnvironmentSolidAngle(cell.y, width, height);
    return value;
}
void SampleEnvironmentWorld(float selector,
                            float latitude,
                            out float3 outDirectionWorld,
                            out float3 outRadiance,
                            out float outPdf)
{
    if (SurfaceLayout.w == 0)
    {
        float cosine = 1 - 2 * selector;
        float radius = sqrt(max(0, 1 - cosine * cosine));
        outDirectionWorld = float3(radius * cos(2 * PI * latitude), radius * sin(2 * PI * latitude), cosine);
        outRadiance = Environment.xyz;
        outPdf = 1 / (4 * PI);
        return;
    }
    uint width = asuint(SceneValue(SurfaceLayout.w - 1).x);
    uint height = asuint(SceneValue(SurfaceLayout.w - 1).y);
    uint low = 0;
    uint high = width * height - 1;
    while (low < high)
    {
        uint middle = low + (high - low) / 2;
        if (SceneValue(SurfaceLayout.w + middle).w <= selector)
            low = middle + 1;
        else
            high = middle;
    }
    float4 value = SceneValue(SurfaceLayout.w + low);
    float previous = low == 0 ? 0 : SceneValue(SurfaceLayout.w + low - 1).w;
    float probability = value.w - previous;
    float longitude = (float(low % width) + (selector - previous) / probability) * (2 * PI / float(width));
    float row = float(low / width);
    float cosine = lerp(cos(PI * row / float(height)), cos(PI * (row + 1) / float(height)), latitude);
    float radius = sqrt(max(0, 1 - cosine * cosine));
    outDirectionWorld = float3(radius * cos(longitude), cosine, radius * sin(longitude));
    outRadiance = value.xyz;
    outPdf = probability / ComputeEnvironmentSolidAngle(low / width, width, height);
}
float3 Integrate(uint pixel, uint sample)
{
    if (SurfaceLayout.x != LUDUS_GPU_PREVIEW_SURFACE_WORDS || SurfaceLayout.z != LUDUS_GPU_PREVIEW_KERNEL_VERSION)
        return float3(-1);
    float2 jitter = (Sampling.z & 4) != 0 ? float2(.5) : float2(Random01(pixel, sample, 0), Random01(pixel, sample, 1));
    float2 positionUv = (float2(pixel % Dimensions.x, pixel / Dimensions.x) + jitter) / float2(Dimensions.xy);
    float nearClipZ = (Sampling.z & 1) != 0 ? 1 : 0;
    float4 nearPositionWorld = mul(ClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, nearClipZ, 1));
    nearPositionWorld /= nearPositionWorld.w;
    float4 farPositionWorld = mul(ClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, 1 - nearClipZ, 1));
    // Match RT1's farPositionWorld-plane/homogeneous shadowDirectionWorld, including non-default pinholes.
    float3 originWorld = CameraPositionWorld.w != 0 ? nearPositionWorld.xyz : CameraPositionWorld.xyz;
    float3 directionWorld = NormalizeOrZero(
        farPositionWorld.w != 0 ? farPositionWorld.xyz / farPositionWorld.w - originWorld : farPositionWorld.xyz);
    float minimum = CameraPositionWorld.w != 0 ? 0 : dot(nearPositionWorld.xyz - originWorld, directionWorld);
    float maximum = Options.x;
    if (farPositionWorld.w != 0)
        maximum = dot(farPositionWorld.xyz / farPositionWorld.w - originWorld, directionWorld);
    else
    {
        maximum = 0;
        for (uint corner = 0; corner < 8; ++corner)
        {
            float3 boundsCornerWorld = float3((corner & 1) != 0 ? BoundsMaximumWorld.x : BoundsMinimumWorld.x,
                                              (corner & 2) != 0 ? BoundsMaximumWorld.y : BoundsMinimumWorld.y,
                                              (corner & 4) != 0 ? BoundsMaximumWorld.z : BoundsMinimumWorld.z);
            maximum = max(maximum, length(boundsCornerWorld - originWorld));
        }
        maximum = maximum * 1.000001 + 1;
    }
    float3 radiance = 0;
    float3 throughput = 1;
    float3 previousPositionWorld = 0;
    float previousPdf = 0;
    uint lights = Counts.w + Counts.z + ((SurfaceLayout.w != 0 || HasPositiveComponent(Environment.xyz)) ? 1u : 0u);
    for (uint depth = 0; depth <= Dimensions.z; ++depth)
    {
        Hit hit = Trace(originWorld, directionWorld, minimum, maximum);
        if (hit.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
            return float3(-1);
        if (hit.Triangle == LUDUS_GPU_PREVIEW_INVALID_TRIANGLE)
        {
            float4 environment = EvaluateEnvironmentWorld(directionWorld);
            float w = depth == 0 ? 1 : PowerWeight(previousPdf, lights > 0 ? environment.w / float(lights) : 0);
            radiance += throughput * environment.xyz * w;
            break;
        }
        uint t = hit.Triangle;
        HitMaterial material = LoadHitMaterial(t, hit.Barycentric);
        float3 positionWorld = PositionWorld(t, hit.Barycentric);
        float3 geometricNormalWorld = SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).xyz;
        float3 viewDirectionWorld = -directionWorld;
        float3 emission =
            ReadHitSurface(material, LUDUS_GPU_PREVIEW_EMISSION_WORD).xyz * SampleHitTexture(material, 2).xyz;
        if (asuint(ReadHitSurface(material, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).w) == 0 &&
            dot(geometricNormalWorld, viewDirectionWorld) <= 0)
            emission = float3(0);
        radiance +=
            throughput * emission *
            (depth == 0
                 ? 1
                 : PowerWeight(previousPdf,
                               AreaPdf(t, previousPositionWorld, positionWorld, HitEmitterSelection(t, lights))));
        if (depth >= Dimensions.z || dot(geometricNormalWorld, viewDirectionWorld) <= 0)
            break;
        float3 normalWorld = NormalizeOrZero(
            SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 3).xyz * (1 - hit.Barycentric.x - hit.Barycentric.y) +
            SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 4).xyz * hit.Barycentric.x +
            SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 5).xyz * hit.Barycentric.y);
        float3 color =
            saturate(ReadHitSurface(material, LUDUS_GPU_PREVIEW_BASE_COLOR_WORD).xyz * SampleHitTexture(material).xyz);
        if (asuint(ReadHitSurface(material, LUDUS_GPU_PREVIEW_SURFACE_OPTIONS_WORD).z) != 0)
            color *= InterpolateTriangleColor(t, hit.Barycentric).xyz;
        float metallic = ReadHitSurface(material, LUDUS_GPU_PREVIEW_BASE_COLOR_WORD).w;
        float roughness = ReadHitSurface(material, LUDUS_GPU_PREVIEW_ROUGHNESS_WORD).x;
        if (asuint(ReadHitSurface(material, LUDUS_GPU_PREVIEW_SURFACE_MAP_WORD).w) != 0)
        {
            SurfaceParameters parameters =
                DecodeSurfaceParameters(SampleHitTexture(material, 1),
                                        metallic,
                                        roughness,
                                        ReadHitSurface(material, LUDUS_GPU_PREVIEW_SURFACE_MAP_WORD).x);
            metallic = parameters.Metallic;
            roughness = parameters.Roughness;
            float2 uvScale = ReadHitSurface(material, LUDUS_GPU_PREVIEW_UV_TRANSFORM_WORD).xy;
            float4 authoredTangentWorld = InterpolateTriangle(t, 10, hit.Barycentric);
            if (asuint(ReadHitSurface(material, LUDUS_GPU_PREVIEW_SURFACE_OPTIONS_WORD).z) != 0 &&
                abs(authoredTangentWorld.w) > .5)
            {
                float3 tangentWorld = NormalizeOrZero(authoredTangentWorld.xyz -
                                                      normalWorld * dot(normalWorld, authoredTangentWorld.xyz));
                normalWorld = NormalizeOrZero(tangentWorld * parameters.NormalTangent.x +
                                              cross(normalWorld, tangentWorld) * (authoredTangentWorld.w < 0 ? -1 : 1) *
                                                  parameters.NormalTangent.y +
                                              normalWorld * parameters.NormalTangent.z);
            }
            else
                normalWorld = ApplyNormalWorld(normalWorld,
                                               parameters.NormalTangent,
                                               SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 1).xyz -
                                                   SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS).xyz,
                                               SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 2).xyz -
                                                   SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS).xyz,
                                               (SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 6).zw -
                                                SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 6).xy) *
                                                   uvScale,
                                               (SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 7).xy -
                                                SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 6).xy) *
                                                   uvScale);
        }
        uint dimension = LUDUS_PATH_RANDOM_FIRST_BOUNCE_DIMENSION + depth * LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE;
        if (lights > 0)
        {
            uint selected;
            if (TrySampleRandomBounded(Sampling.xy, PixelRandomAddress(pixel, sample, dimension), lights, selected) ==
                false)
                return float3(-1);
            float3 lightDirectionWorld;
            float3 lightColor = 0;
            float3 lightPositionWorld = 0;
            float pdf = 0;
            float range = Options.x;
            uint emitter = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
            bool isDelta = false;
            bool hasFiniteEndpoint = false;
            if (selected < Counts.w)
            {
                uint light = SurfaceLayout.y + selected * LUDUS_GPU_PREVIEW_LIGHT_WORDS;
                float4 colorAndKind = SceneValue(light);
                float4 positionAndRangeWorld = SceneValue(light + 1);
                float4 directionAndConeWorld = SceneValue(light + 2);
                lightColor = colorAndKind.xyz;
                lightDirectionWorld = -directionAndConeWorld.xyz;
                pdf = 1 / float(lights);
                isDelta = true;
                if (colorAndKind.w != 0)
                {
                    lightPositionWorld = positionAndRangeWorld.xyz;
                    float3 offsetWorld = lightPositionWorld - positionWorld;
                    float distanceSquared = dot(offsetWorld, offsetWorld);
                    if (distanceSquared <= 0)
                        return float3(-1);
                    range = sqrt(distanceSquared);
                    lightDirectionWorld = offsetWorld / range;
                    hasFiniteEndpoint = true;
                    float ratio = range / positionAndRangeWorld.w;
                    float window = max(1 - ratio * ratio * ratio * ratio, 0);
                    float angular = 1;
                    if (colorAndKind.w == 2)
                    {
                        float cone =
                            saturate((dot(-lightDirectionWorld, directionAndConeWorld.xyz) - directionAndConeWorld.w) /
                                     (SceneValue(light + 3).x - directionAndConeWorld.w));
                        angular = cone * cone;
                    }
                    if (colorAndKind.w == 3 || colorAndKind.w == 4)
                    {
                        float emitterCosine = dot(-lightDirectionWorld, directionAndConeWorld.xyz);
                        angular = colorAndKind.w == 4 ? abs(emitterCosine) : max(emitterCosine, 0);
                    }
                    lightColor *= range <= Options.x ? window * window * angular / max(distanceSquared, 1e-4) : 0;
                }
            }
            else if (selected - Counts.w < Counts.z)
            {
                uint total = EmitterCumulative(Counts.z - 1);
                uint value = 0;
                if (total != 0 && TrySampleRandomBounded(
                                      Sampling.xy,
                                      PixelRandomAddress(pixel, sample, dimension + LUDUS_PATH_RANDOM_EMITTER_SLOT),
                                      total,
                                      value) == false)
                    return float3(-1);
                uint selectedEmitter = total == 0 ? 0 : SelectEmitter(value);
                emitter = asuint(SceneValue(Offsets.z + selectedEmitter).x);
                float root = sqrt(Random01(pixel, sample, dimension + 1));
                float2 uv = float2(root * (1 - Random01(pixel, sample, dimension + 2)), 0);
                uv.y = root - uv.x;
                lightPositionWorld = PositionWorld(emitter, uv);
                float3 offsetWorld = lightPositionWorld - positionWorld;
                range = length(offsetWorld);
                lightDirectionWorld = NormalizeOrZero(offsetWorld);
                pdf = total == 0 ? 0
                                 : AreaPdf(emitter,
                                           positionWorld,
                                           lightPositionWorld,
                                           EmitterSelection(selectedEmitter, lights));
                if ((asuint(Surface(emitter, 5).w) != 0 ||
                     dot(SceneValue(emitter * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).xyz, -lightDirectionWorld) > 0) &&
                    range <= Options.x &&
                    (asuint(Surface(emitter, 5).z) == 0 || Texture(emitter, TexcoordUv(emitter, uv)).w *
                                                                   Surface(emitter, 2).w *
                                                                   InterpolateTriangleColor(emitter, uv).w >=
                                                               Surface(emitter, 1).y))
                    lightColor = Surface(emitter, 4).xyz * Texture(emitter, TexcoordUv(emitter, uv), 2).xyz;
            }
            else
            {
                SampleEnvironmentWorld(Random01(pixel, sample, dimension + 1),
                                       Random01(pixel, sample, dimension + 2),
                                       lightDirectionWorld,
                                       lightColor,
                                       pdf);
                pdf /= float(lights);
            }
            float3 value;
            float bsdfPdf;
            Evaluate(color, metallic, roughness, normalWorld, viewDirectionWorld, lightDirectionWorld, value, bsdfPdf);
            if (pdf > 0 && HasPositiveComponent(lightColor) && dot(normalWorld, lightDirectionWorld) > 0 &&
                dot(geometricNormalWorld, lightDirectionWorld) > 0)
            {
                float3 shadowOriginWorld = OffsetPositionWorld(positionWorld, t, lightDirectionWorld);
                float3 shadowDirectionWorld = lightDirectionWorld;
                float distance = range;
                if (emitter != LUDUS_GPU_PREVIEW_INVALID_TRIANGLE)
                {
                    float3 shadowEndWorld = OffsetPositionWorld(lightPositionWorld, emitter, -lightDirectionWorld);
                    float3 offsetWorld = shadowEndWorld - shadowOriginWorld;
                    distance = length(offsetWorld);
                    shadowDirectionWorld = NormalizeOrZero(offsetWorld);
                    distance = asfloat(asuint(distance) - 1);
                }
                else if (hasFiniteEndpoint)
                {
                    float3 offsetWorld = lightPositionWorld - shadowOriginWorld;
                    distance = length(offsetWorld);
                    shadowDirectionWorld = NormalizeOrZero(offsetWorld);
                    distance = asfloat(asuint(distance) - 1);
                }
                VisibilityResult shadow = TraceVisibility(shadowOriginWorld, shadowDirectionWorld, 0, distance);
                if (shadow.HasFailed == true)
                    return float3(-1);
                if (shadow.IsOccluded == false)
                    radiance +=
                        throughput * value * lightColor *
                        (dot(normalWorld, lightDirectionWorld) * (isDelta ? 1 : PowerWeight(pdf, bsdfPdf)) / pdf);
            }
        }
        float3 outgoingDirectionWorld = SampleBsdf(normalWorld,
                                                   viewDirectionWorld,
                                                   roughness,
                                                   metallic == 1 ? 1 : .5,
                                                   float3(Random01(pixel, sample, dimension + 3),
                                                          Random01(pixel, sample, dimension + 4),
                                                          Random01(pixel, sample, dimension + 5)));
        if (dot(normalWorld, outgoingDirectionWorld) <= 0 || dot(geometricNormalWorld, outgoingDirectionWorld) <= 0)
            break;
        float3 value;
        float pdf;
        Evaluate(color, metallic, roughness, normalWorld, viewDirectionWorld, outgoingDirectionWorld, value, pdf);
        if (pdf <= 0)
            break;
        throughput *= value * (dot(normalWorld, outgoingDirectionWorld) / pdf);
        if (!HasPositiveComponent(throughput))
            break;
        if (depth + 1 >= Dimensions.w)
        {
            float keep = clamp(max(throughput.x, max(throughput.y, throughput.z)), .05, .95);
            if (Random01(pixel, sample, dimension + 6) >= keep)
                break;
            throughput /= keep;
        }
        previousPdf = pdf;
        previousPositionWorld = positionWorld;
        originWorld = OffsetPositionWorld(positionWorld, t, outgoingDirectionWorld);
        directionWorld = outgoingDirectionWorld;
        minimum = 0;
        maximum = Options.x;
    }
    return radiance;
}
// The graph wrapper owns the entry point when this kernel is included as a helper.
// Calling another annotated compute entry point hangs Slang 2026.1.2's parameter transform.
#if defined(LUDUS_RENDER_GRAPH_RAY_PIPELINE)
#    define LUDUS_GPU_PREVIEW_COMPUTE_ENTRY
#else
#    define LUDUS_GPU_PREVIEW_COMPUTE_ENTRY                                                                            \
        [numthreads(LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_WIDTH, LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_HEIGHT, 1)]
#endif
LUDUS_GPU_PREVIEW_COMPUTE_ENTRY void ComputeMain(uint3 positionPixel : SV_DispatchThreadID)
{
    if (positionPixel.x >= Dimensions.x || positionPixel.y >= Dimensions.y)
        return;
    uint pixel = positionPixel.y * Dimensions.x + positionPixel.x;
    float4 sum = accumulation[pixel * 2];
    float4 compensation = accumulation[pixel * 2 + 1];
    float historyTag = float((Sampling.z >> LUDUS_GPU_PREVIEW_HISTORY_SHIFT) * 2);
    if (compensation.w != historyTag && compensation.w != historyTag + 1)
    {
        sum = 0;
        accumulation[pixel * 2] = 0; // Scene callbacks read the sample index from this buffer.
        compensation = float4(0, 0, 0, historyTag);
    }
#if defined(LUDUS_RENDER_GRAPH_RAY_PIPELINE)
    uint sampleCount = asuint(sum.w);
#else
    uint sampleCount = uint(sum.w);
#endif
    // Graphs average only when an authored Accumulate operation supplies a positive limit.
    bool hasAccumulation = Sampling.w != 0;
    bool shouldSample = hasAccumulation == false || (sampleCount < Sampling.w && compensation.w == historyTag);
    if (shouldSample == true)
    {
#ifdef LUDUS_RAY_PATH_CAPTURE
        BeginRayPathCapture(positionPixel.xy, sampleCount);
#endif
#if defined(LUDUS_RENDER_GRAPH_RAY_PIPELINE)
        // Keep one callback invocation: duplicating it for raw/mean output expands large estimators twice.
        float3 sample = EvaluateGraphRay(pixel);
#else
        float3 sample = Integrate(pixel, sampleCount);
#endif
#ifdef LUDUS_RAY_PATH_CAPTURE
        EndRayPathCapture(sample);
#endif
        if (hasAccumulation == false)
        {
            sum.xyz = sample;
            sampleCount += 1;
            compensation.w = any(isfinite(sample) == false) || any(sample < 0) ? historyTag + 1 : historyTag;
        }
        else if (any(isfinite(sample) == false) || any(sample < 0))
            compensation.w = historyTag + 1;
        else
        {
            float3 adjusted = sample - compensation.xyz;
            float3 next = sum.xyz + adjusted;
            compensation.xyz = (next - sum.xyz) - adjusted;
            if (any(isfinite(next) == false) || any(isfinite(compensation.xyz) == false))
                compensation.w = historyTag + 1;
            else
            {
                sum.xyz = next;
                sampleCount += 1;
            }
        }
#if defined(LUDUS_RENDER_GRAPH_RAY_PIPELINE)
        sum.w = asfloat(sampleCount);
#else
        sum.w = float(sampleCount);
#endif
        accumulation[pixel * 2] = sum;
        accumulation[pixel * 2 + 1] = compensation;
    }
    float3 mean = hasAccumulation == true && sampleCount > 0 ? sum.xyz / float(sampleCount) : sum.xyz;
    // OIDN filters linear HDR in place, preserving accumulation and the error tag.
    // Its consumer applies display conversion only after the native denoising pass.
    if ((Sampling.z & LUDUS_GPU_PREVIEW_DENOISING_FLAG) != 0)
    {
        colors[pixel] = float4(compensation.w != historyTag ? float3(1, 0, 1) : mean,
                               compensation.w != historyTag ? -1 : float(pixel));
        return;
    }
    static const uint TONE_MAPPING_CHANNEL_COUNT = 3;
    static const float SRGB_LINEAR_THRESHOLD = .0031308;
    static const float SRGB_POWER_SCALE = 1.055;
    static const float SRGB_GAMMA = 2.4;
    static const float SRGB_POWER_OFFSET = .055;
    static const float SRGB_LINEAR_SCALE = 12.92;
    float3 displayColor = mean;
    if (Options.y > 0)
    {
        float3 linear = mean * Options.y;
        for (uint channel = 0; channel < TONE_MAPPING_CHANNEL_COUNT; ++channel)
        {
            // Finite nonnegative radiance can overflow when exposure is applied.
            // Reinhard tends to one there; do not encode an infinity/infinity NaN.
            linear[channel] = isfinite(linear[channel]) == true ? linear[channel] / (1 + linear[channel]) : 1;
        }
        float3 encoded = select(linear > SRGB_LINEAR_THRESHOLD,
                                SRGB_POWER_SCALE * pow(linear, 1.0 / SRGB_GAMMA) - SRGB_POWER_OFFSET,
                                linear * SRGB_LINEAR_SCALE);
        displayColor = encoded;
    }
    colors[pixel] = float4(compensation.w != historyTag ? float3(1, 0, 1) : saturate(displayColor), float(pixel));
}
#undef LUDUS_GPU_PREVIEW_COMPUTE_ENTRY
