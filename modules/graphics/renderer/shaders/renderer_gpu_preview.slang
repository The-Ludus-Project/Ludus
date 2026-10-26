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
float4 Texture(uint t, float2 uv, uint map = 0)
{
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
struct Hit
{
    uint Triangle;
    float DistanceWorld;
    float2 Barycentric;
};
float4 InterpolateTriangle(uint triangle, uint first, in float2 barycentric);
float4 InterpolateTriangleColor(uint triangle, in float2 barycentric);
#ifndef LUDUS_SOFTWARE_RAY_TRAVERSAL
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
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(rayScene, flags, LUDUS_GPU_PREVIEW_RAY_MASK, ray);
    while (query.Proceed() == true)
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        uint candidatePrimitive = query.CandidatePrimitiveIndex();
        if ((Sampling.z & LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG) != 0)
        {
            if (query.CandidateGeometryIndex() == 0)
            {
                query.CommitNonOpaqueTriangleHit();
                continue;
            }
            candidatePrimitive += asuint(SceneValue(Offsets.y).y);
        }
        uint triangle = asuint(SceneValue(Offsets.y + candidatePrimitive).x);
        float2 barycentric = query.CandidateTriangleBarycentrics();
        if (asuint(Surface(triangle, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) != 0 &&
            Texture(triangle, TexcoordUv(triangle, barycentric)).w * Surface(triangle, 2).w *
                    InterpolateTriangleColor(triangle, barycentric).w <
                Surface(triangle, 1).y)
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
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
#endif
#if defined(LUDUS_GRAPH_TRAVERSAL_STRATEGIES) || defined(LUDUS_SOFTWARE_RAY_TRAVERSAL)
#    include "ray_triangle_loop.slang"
#endif
Hit Trace(float3 originWorld,
          float3 directionWorld,
          float nearDistanceWorld,
          float farDistanceWorld,
          bool isVisibility = false)
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
float AreaPdf(uint t, float3 fromPositionWorld, float3 toPositionWorld, uint lightCount)
{
    if (lightCount == 0)
        return 0;
    float3 offsetWorld = toPositionWorld - fromPositionWorld;
    float cosine = abs(dot(SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).xyz, -NormalizeOrZero(offsetWorld)));
    return cosine > 0 ? dot(offsetWorld, offsetWorld) /
                            (float(lightCount) * SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 1).w * cosine)
                      : 0;
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
        float3 positionWorld = PositionWorld(t, hit.Barycentric);
        float3 geometricNormalWorld = SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 8).xyz;
        float3 viewDirectionWorld = -directionWorld;
        float3 emission = Surface(t, 4).xyz * Texture(t, TexcoordUv(t, hit.Barycentric), 2).xyz;
        if (asuint(Surface(t, 5).w) == 0 && dot(geometricNormalWorld, viewDirectionWorld) <= 0)
            emission = float3(0);
        radiance +=
            throughput * emission *
            (depth == 0 ? 1 : PowerWeight(previousPdf, AreaPdf(t, previousPositionWorld, positionWorld, lights)));
        if (depth >= Dimensions.z || dot(geometricNormalWorld, viewDirectionWorld) <= 0)
            break;
        float3 normalWorld = NormalizeOrZero(
            SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 3).xyz * (1 - hit.Barycentric.x - hit.Barycentric.y) +
            SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 4).xyz * hit.Barycentric.x +
            SceneValue(t * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + 5).xyz * hit.Barycentric.y);
        float3 color = saturate(Surface(t, 0).xyz * Texture(t, TexcoordUv(t, hit.Barycentric)).xyz);
        if (asuint(Surface(t, 7).z) != 0)
            color *= InterpolateTriangleColor(t, hit.Barycentric).xyz;
        float metallic = Surface(t, 0).w;
        float roughness = Surface(t, 1).x;
        if (asuint(Surface(t, 6).w) != 0)
        {
            SurfaceParameters parameters = DecodeSurfaceParameters(Texture(t, TexcoordUv(t, hit.Barycentric), 1),
                                                                   metallic,
                                                                   roughness,
                                                                   Surface(t, 6).x);
            metallic = parameters.Metallic;
            roughness = parameters.Roughness;
            float2 uvScale = Surface(t, 3).xy;
            float4 authoredTangentWorld = InterpolateTriangle(t, 10, hit.Barycentric);
            if (asuint(Surface(t, 7).z) != 0 && abs(authoredTangentWorld.w) > .5)
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
                emitter = asuint(SceneValue(Offsets.z + selected - Counts.w).x);
                float root = sqrt(Random01(pixel, sample, dimension + 1));
                float2 uv = float2(root * (1 - Random01(pixel, sample, dimension + 2)), 0);
                uv.y = root - uv.x;
                lightPositionWorld = PositionWorld(emitter, uv);
                float3 offsetWorld = lightPositionWorld - positionWorld;
                range = length(offsetWorld);
                lightDirectionWorld = NormalizeOrZero(offsetWorld);
                pdf = AreaPdf(emitter, positionWorld, lightPositionWorld, lights);
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
                Hit shadow = Trace(shadowOriginWorld, shadowDirectionWorld, 0, distance, true);
                if (shadow.Triangle == LUDUS_GPU_PREVIEW_FAILED_TRIANGLE)
                    return float3(-1);
                if (shadow.Triangle == LUDUS_GPU_PREVIEW_INVALID_TRIANGLE)
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
#    define LUDUS_GPU_PREVIEW_COMPUTE_ENTRY [numthreads(LUDUS_GPU_PREVIEW_GROUP_WIDTH, 1, 1)]
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
#if defined(LUDUS_RENDER_GRAPH_RAY_PIPELINE)
        // Keep one callback invocation: duplicating it for raw/mean output expands large estimators twice.
        float3 sample = EvaluateGraphRay(pixel);
#else
        float3 sample = Integrate(pixel, sampleCount);
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
