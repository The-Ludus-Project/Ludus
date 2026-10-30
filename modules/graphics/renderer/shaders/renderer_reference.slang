#include "material_scattering.slang"
#include "math.slang"
#include "raster_bindings.slang"
#include "renderer_gpu_preview_layout.h"
#include "renderer_reference_layout.h"
#include "renderer_surface_functions.slang"

// GPU-compute implementation of the frozen RT1 reference integral. Each function
// below ports its CPU oracle counterpart in reference_scene.cpp (BVH traversal,
// triangle intersection, fixed-mip texture and opacity lookup, camera rays) or
// reference_path.cpp/reference_bsdf.cpp (light proposals, MIS, BSDF sampling,
// roulette and emission/direct/indirect transport), with the same addressed Philox
// dimensions so matching samples follow matching paths. Departures are float32
// arithmetic where the CPU oracle uses float64 (triangle edges, slab tests,
// normalization and the environment CDF); see renderer-systems.md for tolerances.
// Thanks to Sven Woop, Carsten Benthin and Ingo Wald, "Watertight Ray/Triangle
// Intersection", JCGT 2(1), 2013, section 3,
// https://jcgt.org/published/0002/01/05/paper.pdf, for the permuted/sheared edge
// test; this port omits the paper's float64 fallback for exactly-zero edges.
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based
// Rendering", 3rd edition, section 3.9.2, "Conservative Ray-Bounds Intersections",
// https://pbr-book.org/3ed-2018/Shapes/Managing_Rounding_Error, for widening the
// float32 slab exit by 1 + 2 * gamma(3), and 4th edition, section 13.4,
// https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer,
// for the MIS/roulette conventions shared with the CPU oracle. Thanks to Eric Heitz,
// "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018, sections 3-4
// and Appendix B, https://jcgt.org/published/0007/04/01/paper.pdf, Google, "Physically
// Based Rendering in Filament", sections 4.4/5.2.2,
// https://google.github.io/filament/main/filament.html, and John K. Salmon et al.,
// "Parallel Random Numbers: As Easy as 1, 2, 3", SC11 (2011), section 4.3,
// https://www.thesalmons.org/john/random123/papers/random123sc11.pdf, for Philox4x32-10.
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_PARAMETERS_BINDING) cbuffer Parameters
{
    column_major float4x4 ClipToWorld;
    float4 CameraPositionWorld; // w: orthographic flag
    uint4 Dimensions;           // width, height, maximum depth, roulette depth
    uint4 Offsets;              // nodes, order, emitters, surfaces
    uint4 Counts;               // triangles, nodes, emitters, analytic lights
    uint4 Sampling;             // key low, key high, flags (reverse/infinite/center/opaque), target
    float4 Environment;
    float4 Options; // secondary range, unused exposure, instance count bits, scene data bits
    float4 BoundsMinimumWorld;
    float4 BoundsMaximumWorld;
    uint4 SurfaceLayout; // surface stride, analytic light offset, profile version, environment offset + 1
};
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_SCENE_BINDING) StructuredBuffer<uint4> sceneWords;
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_ACCUMULATION_BINDING) RWStructuredBuffer<float4> accumulation;
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_STATUS_BINDING) RWStructuredBuffer<uint4> status;
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_TEXELS_BINDING) StructuredBuffer<uint4> textureWords;
// The renderer's LTC table, read only by the resampled ReferenceLightSampling modes. Declared after every
// lower binding: Slang numbers Metal buffers in declaration order.
#define LUDUS_LTC_TABLE_BINDING LUDUS_REFERENCE_LTC_TABLE_BINDING
#include "linearly_transformed_cosine.slang"
#include "ltc_table.slang"
#include "reference_light_sampling.slang"
#include "weighted_reservoir.slang"

// Keeps the conventional brace layout of both kernel entry points under the pinned formatter.
#define LUDUS_REFERENCE_COMPUTE_ENTRY [numthreads(LUDUS_REFERENCE_GROUP_WIDTH, 1, 1)]

static const uint REVERSE_Z_FLAG = 1;
static const uint INFINITE_FAR_FLAG = 2;
static const uint CENTER_RAYS_FLAG = 4;
static const uint RESAMPLED_LIGHT_FLAG = LUDUS_REFERENCE_RESAMPLED_LIGHT_FLAG;
static const uint FORM_FACTOR_TARGET_FLAG = LUDUS_REFERENCE_FORM_FACTOR_TARGET_FLAG;
static const uint LINEAR_FILTER_FLAG = 1;
static const uint REPEAT_U_FLAG = 2;
static const uint REPEAT_V_FLAG = 4;
static const uint SRGB_FLAG = 8;
static const uint COLOR_IMAGE = 0;
static const uint SURFACE_IMAGE = 1;
static const uint EMISSION_IMAGE = 2;
static const uint NO_TRIANGLE = 0xffffffff;
static const uint DIRECTIONAL_LIGHT = 0;
static const uint SPOT_LIGHT = 2;
static const uint AREA_ONE_SIDED_LIGHT = 3;
static const uint AREA_TWO_SIDED_LIGHT = 4;
static const uint RGBA8_CHANNEL_BITS = 8;
static const uint RGBA8_CHANNEL_MASK = 0xff;
static const uint TEXELS_PER_WORD = 4;
static const float RGBA8_CHANNEL_MAXIMUM = 255;
static const float MAXIMUM_WORLD_COORDINATE = 1000000;
static const float UNIT_LENGTH_TOLERANCE = 1e-5;
static const float MINIMUM_ROUGHNESS = .045;
static const float DIELECTRIC_REFLECTANCE = .04;
static const float SMITH_DENOMINATOR_FLOOR = 1e-6;
static const float NEAR_LIGHT_SQUARED_DISTANCE_FLOOR = 1e-4;
static const float ROULETTE_MINIMUM_SURVIVAL = .05;
static const float ROULETTE_MAXIMUM_SURVIVAL = .95;
static const float BASIS_POLE_COSINE = .999;
static const float INFINITE_FAR_DISTANCE_SCALE = 1.000001;
static const float MINIMUM_NORMAL_FLOAT = 1.17549435e-38;
static const uint MINIMUM_NORMAL_FLOAT_BITS = 0x00800000;
static const uint SIGN_BIT = 0x80000000;
static const uint QUIET_NAN_BITS = 0x7fc00000;
static const uint PROPOSAL_SELECTION_SLOT = 0;
static const uint PROPOSAL_FIRST_SLOT = 1;
static const uint PROPOSAL_SECOND_SLOT = 2;
static const uint BSDF_LOBE_SLOT = 3;
static const uint BSDF_FIRST_SLOT = 4;
static const uint BSDF_SECOND_SLOT = 5;
static const uint ROULETTE_SLOT = 6;
static const uint EMITTER_SLOT = LUDUS_PATH_RANDOM_EMITTER_SLOT;
static const uint LTC_FIRST_SLOT = LUDUS_REFERENCE_LTC_FIRST_SLOT;

float4 SceneValue(uint index)
{
    return asfloat(sceneWords[index]);
}
float4 TriangleValue(uint triangle, uint word)
{
    return SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + word);
}
uint GetInstance(uint triangle)
{
    return asuint(TriangleValue(triangle, 0).w);
}
float4 SurfaceValue(uint instance, uint word)
{
    return SceneValue(Offsets.w + instance * SurfaceLayout.x + word);
}
float4 InterpolateTriangle(uint triangle, uint first, float u, float v)
{
    return TriangleValue(triangle, first) * (1 - u - v) + TriangleValue(triangle, first + 1) * u +
           TriangleValue(triangle, first + 2) * v;
}
float3 GetPositionWorld(uint triangle, float u, float v)
{
    return InterpolateTriangle(triangle, 0, u, v).xyz;
}
float2 GetTexcoordUv(uint triangle, float u, float v)
{
    float4 first = TriangleValue(triangle, 6);
    float2 last = TriangleValue(triangle, 7).xy;
    return first.xy * (1 - u - v) + first.zw * u + last * v;
}
float3 GetGeometricNormalWorld(uint triangle)
{
    return TriangleValue(triangle, LUDUS_GPU_PREVIEW_GEOMETRIC_NORMAL_WORD).xyz;
}
float3 GetPositionErrorWorld(uint triangle)
{
    return TriangleValue(triangle, LUDUS_GRAPH_EMITTER_WORD).xyz;
}
float GetArea(uint triangle)
{
    return TriangleValue(triangle, 1).w;
}
float4 InterpolateTriangleColor(uint triangle, float u, float v)
{
    // Match the oracle and GPU preview: finite normalized colors stay in their
    // admitted range after interpolation; preserve nonfinite values for validation.
    float4 color = InterpolateTriangle(triangle, LUDUS_GPU_PREVIEW_VERTEX_COLOR_WORD, u, v);
    return all(isfinite(color)) == true ? saturate(color) : color;
}
float GetVertexAlpha(uint triangle, float u, float v)
{
    return InterpolateTriangleColor(triangle, u, v).w;
}
float QuietNan()
{
    return asfloat(QUIET_NAN_BITS);
}
bool IsFiniteVector(float3 value)
{
    return all(isfinite(value));
}
bool IsValidContribution(float3 value)
{
    return IsFiniteVector(value) && all(value >= 0);
}
bool IsUnit(float3 value)
{
    float square = dot(value, value);
    return IsFiniteVector(value) && square >= 1 - UNIT_LENGTH_TOLERANCE && square <= 1 + UNIT_LENGTH_TOLERANCE;
}
bool IsWithinWorld(float3 positionWorld)
{
    return IsFiniteVector(positionWorld) && all(abs(positionWorld) <= MAXIMUM_WORLD_COORDINATE);
}
float NextFloatDown(float value)
{
    return value > 0 ? asfloat(asuint(value) - 1) : value;
}

// --- Fixed-mip texture lookup (SampleReferenceImage) ------------------------
struct ImageLayout
{
    uint Offset;
    uint Flags;
    uint Width;
    uint Height;
    bool IsPresent;
};
ImageLayout GetImageLayout(uint instance, uint image)
{
    ImageLayout result;
    if (image == COLOR_IMAGE)
    {
        float4 options = SurfaceValue(instance, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD);
        result.Offset = asuint(options.x);
        result.Flags = asuint(options.y);
        result.Width = asuint(SurfaceValue(instance, 1).z);
        result.Height = asuint(SurfaceValue(instance, 1).w);
        result.IsPresent = asuint(SurfaceValue(instance, 4).w) != 0;
    }
    else if (image == SURFACE_IMAGE)
    {
        float4 extent = SurfaceValue(instance, 6);
        result.Offset = asuint(SurfaceValue(instance, 7).x);
        result.Flags = asuint(SurfaceValue(instance, 7).y);
        result.Width = asuint(extent.y);
        result.Height = asuint(extent.z);
        result.IsPresent = asuint(extent.w) != 0;
    }
    else
    {
        float4 options = SurfaceValue(instance, 8);
        result.Offset = asuint(options.x);
        result.Flags = asuint(options.y);
        result.Width = asuint(options.z);
        result.Height = asuint(options.w);
        result.IsPresent = result.Width != 0;
    }
    return result;
}
int AddressTexel(int coordinate, bool isRepeated, uint extent)
{
    int size = int(extent);
    if (isRepeated == false)
        return clamp(coordinate, 0, size - 1);
    int remainder = coordinate % size;
    return remainder < 0 ? remainder + size : remainder;
}
float4 ReadTexel(ImageLayout image, int x, int y)
{
    uint column = uint(AddressTexel(x, (image.Flags & REPEAT_U_FLAG) != 0, image.Width));
    uint row = uint(AddressTexel(y, (image.Flags & REPEAT_V_FLAG) != 0, image.Height));
    uint index = row * image.Width + column;
    uint word = textureWords[image.Offset + index / TEXELS_PER_WORD][index % TEXELS_PER_WORD];
    float4 value = float4(word & RGBA8_CHANNEL_MASK,
                          (word >> RGBA8_CHANNEL_BITS) & RGBA8_CHANNEL_MASK,
                          (word >> (2 * RGBA8_CHANNEL_BITS)) & RGBA8_CHANNEL_MASK,
                          word >> (3 * RGBA8_CHANNEL_BITS)) /
                   RGBA8_CHANNEL_MAXIMUM;
    if ((image.Flags & SRGB_FLAG) != 0)
    {
        value.x = DecodeSrgb(value.x);
        value.y = DecodeSrgb(value.y);
        value.z = DecodeSrgb(value.z);
    }
    return value;
}
float4 SampleImage(uint instance, uint image, float2 uv)
{
    ImageLayout layout = GetImageLayout(instance, image);
    if (layout.IsPresent == false)
        return float4(1);
    float4 uvTransform = SurfaceValue(instance, 3);
    uv = float2(uv.x * uvTransform.x + uvTransform.z, uv.y * uvTransform.y + uvTransform.w);
    if (all(isfinite(uv)) == false)
        return float4(QuietNan());
    uv.x = (layout.Flags & REPEAT_U_FLAG) != 0 ? uv.x - floor(uv.x) : clamp(uv.x, 0, 1);
    uv.y = (layout.Flags & REPEAT_V_FLAG) != 0 ? uv.y - floor(uv.y) : clamp(uv.y, 0, 1);
    float x = uv.x * float(layout.Width);
    float y = uv.y * float(layout.Height);
    if ((layout.Flags & LINEAR_FILTER_FLAG) == 0)
        return ReadTexel(layout, int(floor(x)), int(floor(y)));
    float px = x - .5;
    float py = y - .5;
    int ix = int(floor(px));
    int iy = int(floor(py));
    float tx = px - float(ix);
    float ty = py - float(iy);
    float4 top = ReadTexel(layout, ix, iy) * (1 - tx) + ReadTexel(layout, ix + 1, iy) * tx;
    float4 bottom = ReadTexel(layout, ix, iy + 1) * (1 - tx) + ReadTexel(layout, ix + 1, iy + 1) * tx;
    return top * (1 - ty) + bottom * ty;
}
float3 SampleEmission(uint instance, float2 uv)
{
    return SurfaceValue(instance, 4).xyz * SampleImage(instance, EMISSION_IMAGE, uv).xyz;
}
bool IsMasked(uint instance)
{
    return asuint(SurfaceValue(instance, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) != 0;
}
bool IsEmissionTwoSided(uint instance)
{
    return asuint(SurfaceValue(instance, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).w) != 0;
}
// ReferenceOpacity: masked coverage uses texture alpha, tint alpha and vertex alpha.
bool IsOpaque(uint instance, float2 uv, float vertexAlpha)
{
    return IsMasked(instance) == false ||
           SampleImage(instance, COLOR_IMAGE, uv).w * SurfaceValue(instance, 2).w * vertexAlpha >=
               SurfaceValue(instance, 1).y;
}

// --- BVH traversal (TraceReferenceOracle) ------------------------------------
// The node walk and watertight triangle test are shared with render-graph ComputeBvh passes.
#include "ray_bvh_traversal.slang"
struct Ray
{
    float3 OriginWorld;
    float3 DirectionWorld;
    float MinimumDistanceWorld;
    float MaximumDistanceWorld;
};
struct Hit
{
    uint Triangle; // NO_TRIANGLE for a miss
    float DistanceWorld;
    float U;
    float V;
    bool HasExceededStack;
};
// The opacity test the shared traversal applies to each nearer or lower-index candidate.
bool IsBvhCandidateAccepted(uint triangle, float2 barycentric)
{
    float u = barycentric.x;
    float v = barycentric.y;
    return IsOpaque(GetInstance(triangle), GetTexcoordUv(triangle, u, v), GetVertexAlpha(triangle, u, v));
}
Hit TraceScene(Ray ray, bool isVisibility)
{
    BvhRay query;
    query.OriginWorld = ray.OriginWorld;
    query.DirectionWorld = ray.DirectionWorld;
    query.MinimumDistanceWorld = ray.MinimumDistanceWorld;
    query.MaximumDistanceWorld = ray.MaximumDistanceWorld;
    BvhHit traced = TraceBvh(query, isVisibility);
    Hit result;
    result.Triangle = traced.Triangle;
    result.DistanceWorld = traced.DistanceWorld;
    result.U = traced.Barycentric.x;
    result.V = traced.Barycentric.y;
    result.HasExceededStack = traced.HasExceededStack;
    return result;
}

// --- Camera (CameraRay/GenerateReferencePrimary) -----------------------------
bool GenerateCameraRay(uint pixel, uint sample, out Ray outRay)
{
    outRay.OriginWorld = 0;
    outRay.DirectionWorld = 0;
    outRay.MinimumDistanceWorld = 0;
    outRay.MaximumDistanceWorld = 0;
    bool isCentered = (Sampling.z & CENTER_RAYS_FLAG) != 0;
    float jitterX = isCentered == true ? .5 : RandomValue(pixel, sample, 0);
    float jitterY = isCentered == true ? .5 : RandomValue(pixel, sample, 1);
    float clipX = 2 * (float(pixel % Dimensions.x) + jitterX) / float(Dimensions.x) - 1;
    float clipY = 1 - 2 * (float(pixel / Dimensions.x) + jitterY) / float(Dimensions.y);
    float nearClipZ = (Sampling.z & REVERSE_Z_FLAG) != 0 ? 1 : 0;
    float4 nearWorld = mul(ClipToWorld, float4(clipX, clipY, nearClipZ, 1));
    float4 farWorld = mul(ClipToWorld, float4(clipX, clipY, 1 - nearClipZ, 1));
    if (all(isfinite(nearWorld)) == false || all(isfinite(farWorld)) == false || nearWorld.w == 0)
        return false;
    float3 nearPositionWorld = nearWorld.xyz / nearWorld.w;
    bool isOrthographic = CameraPositionWorld.w != 0;
    float3 originWorld = isOrthographic == true ? nearPositionWorld : CameraPositionWorld.xyz;
    float3 directionWorld;
    if (farWorld.w == 0)
    {
        if ((Sampling.z & INFINITE_FAR_FLAG) == 0 || isOrthographic == true)
            return false;
        directionWorld = NormalizeOrZero(farWorld.xyz);
    }
    else
        directionWorld = NormalizeOrZero(farWorld.xyz / farWorld.w - originWorld);
    if (IsWithinWorld(originWorld) == false || all(directionWorld == 0))
        return false;
    float distanceWorld = 1;
    if (farWorld.w != 0)
        distanceWorld = dot(farWorld.xyz / farWorld.w - originWorld, directionWorld);
    else if (all(BoundsMinimumWorld.xyz <= BoundsMaximumWorld.xyz))
    {
        float maximumWorld = 0;
        for (uint corner = 0; corner < 8; ++corner)
        {
            float3 cornerWorld = float3((corner & 1) != 0 ? BoundsMaximumWorld.x : BoundsMinimumWorld.x,
                                        (corner & 2) != 0 ? BoundsMaximumWorld.y : BoundsMinimumWorld.y,
                                        (corner & 4) != 0 ? BoundsMaximumWorld.z : BoundsMinimumWorld.z);
            maximumWorld = max(maximumWorld, length(cornerWorld - originWorld));
        }
        distanceWorld = maximumWorld * INFINITE_FAR_DISTANCE_SCALE + 1;
    }
    float minimumWorld = isOrthographic == true ? 0 : dot(nearPositionWorld - originWorld, directionWorld);
    if (isfinite(distanceWorld) == false || distanceWorld < 0 || minimumWorld < 0 || minimumWorld > distanceWorld)
        return false;
    outRay.OriginWorld = originWorld;
    outRay.DirectionWorld = directionWorld;
    outRay.MinimumDistanceWorld = minimumWorld;
    outRay.MaximumDistanceWorld = distanceWorld;
    return true;
}

// --- Sampling ---------------------------------------------------------------
// Mirrors ReferenceRandom: scope is the 32-bit pixel index, event the committed
// sample and dimensions follow LUDUS_PATH_RANDOM_* in renderer_gpu_preview_layout.h.
RandomAddress PixelRandomAddress(uint pixel, uint sample, uint dimension)
{
    return MakeRandomAddress(uint2(pixel, 0), sample, dimension);
}
float RandomValue(uint pixel, uint sample, uint dimension)
{
    return SampleRandomFloat01(Sampling.xy, PixelRandomAddress(pixel, sample, dimension));
}
// The resampled ReferenceLightSampling modes: sampler versions 2 and 3 and their 128 dimensions per bounce.
bool IsResampled()
{
    return (Sampling.z & RESAMPLED_LIGHT_FLAG) != 0;
}
// ReferenceLightSampling::ResampledFormFactor, sampler version 3: the approximate target and its frozen counts.
bool HasFormFactorTarget()
{
    return (Sampling.z & FORM_FACTOR_TARGET_FLAG) != 0;
}
uint GetRisCandidateCount()
{
    return HasFormFactorTarget() == true ? LUDUS_REFERENCE_FORM_FACTOR_RIS_CANDIDATE_COUNT
                                         : LUDUS_REFERENCE_RIS_CANDIDATE_COUNT;
}
uint GetRisNormalizationCount()
{
    return HasFormFactorTarget() == true ? LUDUS_REFERENCE_FORM_FACTOR_RIS_NORMALIZATION_CANDIDATE_COUNT
                                         : LUDUS_REFERENCE_RIS_NORMALIZATION_CANDIDATE_COUNT;
}
uint GetBounceDimension(uint depth, uint slot)
{
    uint stride = IsResampled() == true ? LUDUS_PATH_RANDOM_VERSION2_DIMENSIONS_PER_BOUNCE
                                        : LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE;
    return LUDUS_PATH_RANDOM_FIRST_BOUNCE_DIMENSION + depth * stride + slot;
}
float PowerWeight(float pdf, float other)
{
    if (pdf <= 0)
        return 0;
    if (other <= 0)
        return 1;
    float maximum = max(pdf, other);
    float a = pdf / maximum;
    float b = other / maximum;
    return a * a / (a * a + b * b);
}
uint GetLightCount()
{
    return Counts.w + Counts.z + ((SurfaceLayout.w != 0 || HasPositiveComponent(Environment.xyz)) ? 1u : 0u);
}
// AreaPdf: solid-angle density after selecting the triangle with probability `selection`.
float GetAreaPdf(uint triangle, float3 fromPositionWorld, float3 toPositionWorld, float selection)
{
    float3 deltaWorld = toPositionWorld - fromPositionWorld;
    float distanceSquared = dot(deltaWorld, deltaWorld);
    float cosine = abs(dot(GetGeometricNormalWorld(triangle), -NormalizeOrZero(deltaWorld)));
    float area = GetArea(triangle);
    if (selection <= 0 || area <= 0 || cosine <= 0)
        return 0;
    return selection * distanceSquared / (area * cosine);
}
// Emitter words hold the ascending triangle index in X and the inclusive integer
// power CDF in Y (BuildReferenceEmitterSelection).
uint GetEmitterTriangle(uint emitter)
{
    return sceneWords[Offsets.z + emitter].x;
}
uint GetEmitterCumulative(uint emitter)
{
    return sceneWords[Offsets.z + emitter].y;
}
// SelectReferenceEmitter: the emitter whose CDF interval contains value.
uint SelectEmitter(uint value)
{
    uint low = 0;
    uint high = Counts.z - 1;
    while (low < high)
    {
        uint middle = low + (high - low) / 2;
        if (GetEmitterCumulative(middle) <= value)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}
// FindReferenceEmitter: false when the triangle is not a light proposal.
bool FindEmitter(uint triangle, out uint outEmitter)
{
    outEmitter = 0;
    uint low = 0;
    uint high = Counts.z;
    while (low < high)
    {
        uint middle = low + (high - low) / 2;
        if (GetEmitterTriangle(middle) < triangle)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == Counts.z || GetEmitterTriangle(low) != triangle)
        return false;
    outEmitter = low;
    return true;
}
// ComputeEmitterSelection: the uniform category share times the CDF probability.
float GetEmitterSelection(uint emitter, uint lightCount)
{
    uint total = Counts.z == 0 ? 0 : GetEmitterCumulative(Counts.z - 1);
    if (lightCount == 0 || emitter >= Counts.z || total == 0)
        return 0;
    uint previous = emitter == 0 ? 0 : GetEmitterCumulative(emitter - 1);
    return float(GetEmitterCumulative(emitter) - previous) / float(total) * (float(Counts.z) / float(lightCount));
}
// TryOffsetRayOrigin: project the reconstruction error onto the oriented geometric
// normal, then step each nonzero component one representable value outward.
bool TryOffsetOrigin(float3 positionWorld,
                     float3 errorWorld,
                     float3 geometricNormalWorld,
                     float3 directionWorld,
                     out float3 outPositionWorld)
{
    outPositionWorld = positionWorld;
    if (IsFiniteVector(positionWorld) == false || IsFiniteVector(errorWorld) == false || any(errorWorld < 0) ||
        IsUnit(geometricNormalWorld) == false || IsUnit(directionWorld) == false)
        return false;
    float3 normalWorld = dot(geometricNormalWorld, directionWorld) < 0 ? -geometricNormalWorld : geometricNormalWorld;
    float distanceWorld = dot(abs(normalWorld), errorWorld);
    for (uint axis = 0; axis < 3; ++axis)
    {
        float value = positionWorld[axis] + normalWorld[axis] * distanceWorld;
        if (normalWorld[axis] != 0)
        {
            if (value == 0)
                value = asfloat(MINIMUM_NORMAL_FLOAT_BITS | (normalWorld[axis] < 0 ? SIGN_BIT : 0u));
            else
                value = asfloat(asuint(value) + ((value > 0) == (normalWorld[axis] > 0) ? 1u : 0xffffffffu));
            if (abs(value) < MINIMUM_NORMAL_FLOAT)
                value = normalWorld[axis] > 0 ? MINIMUM_NORMAL_FLOAT : -MINIMUM_NORMAL_FLOAT;
        }
        if (isfinite(value) == false)
            return false;
        outPositionWorld[axis] = value;
    }
    return true;
}
float3 OffsetOrigin(float3 positionWorld, uint triangle, float3 directionWorld)
{
    float3 resultWorld;
    if (TryOffsetOrigin(positionWorld,
                        GetPositionErrorWorld(triangle),
                        GetGeometricNormalWorld(triangle),
                        directionWorld,
                        resultWorld) == false)
        return float3(QuietNan(), 0, 0);
    return resultWorld;
}

// --- Environment (environment.cpp) --------------------------------------------
float2 ComputeEnvironmentCoordinates(float3 directionWorld)
{
    float longitude = atan2(directionWorld.z, directionWorld.x) / (2 * PI);
    return float2(longitude - floor(longitude), atan2(length(directionWorld.xz), directionWorld.y) / PI);
}
float ComputeEnvironmentSolidAngle(uint row, uint width, uint height)
{
    // Subtractive cosine loses FP32 precision near poles; use the sine identity.
    return (2 * PI / float(width)) * 2 * sin(PI * (float(row) + .5) / float(height)) * sin(PI / (2 * float(height)));
}
// RGB radiance and solid-angle density of the frozen environment in one direction.
float4 EvaluateEnvironmentWorld(float3 directionWorld)
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
        float angle = 2 * PI * latitude;
        outDirectionWorld = float3(radius * cos(angle), radius * sin(angle), cosine);
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

// --- Reflection model (reference_bsdf.cpp) -------------------------------------
struct Bsdf
{
    float3 BaseColor;
    float Metallic;
    float Roughness;
    uint Model;
    float3 Eta;
    float3 K;
    float Alpha;
};
bool IsValidBsdf(Bsdf bsdf)
{
    return IsFiniteVector(bsdf.BaseColor) && all(bsdf.BaseColor >= 0) && all(bsdf.BaseColor <= 1) &&
           isfinite(bsdf.Metallic) && bsdf.Metallic >= 0 && bsdf.Metallic <= 1 && isfinite(bsdf.Roughness) &&
           bsdf.Roughness >= MINIMUM_ROUGHNESS && bsdf.Roughness <= 1;
}
float GetSpecularProbability(Bsdf bsdf)
{
    return ComputeMaterialSpecularProbability(bsdf.Model, bsdf.Metallic);
}
// Value and full mixture density, without the cosine.
float4 EvaluateBsdf(Bsdf bsdf, float3 normalWorld, float3 viewWorld, float3 lightWorld)
{
    float nv = dot(normalWorld, viewWorld);
    float nl = dot(normalWorld, lightWorld);
    if (nv <= 0 || nl <= 0)
        return 0;
    if (bsdf.Model == LUDUS_MATERIAL_MODEL_DIFFUSE)
        return float4(bsdf.BaseColor / PI, nl / PI);
    float3 halfWorld = NormalizeOrZero(viewWorld + lightWorld);
    float nh = clamp(dot(normalWorld, halfWorld), 0, 1);
    float vh = clamp(dot(viewWorld, halfWorld), 0, 1);
    float alpha = ComputeMaterialAlpha(bsdf.Model, bsdf.Roughness, bsdf.Alpha);
    float alphaSquared = alpha * alpha;
    // This stable equivalent avoids cancellation at low roughness and nh~=1.
    float denominator = (1 - nh * nh) + nh * nh * alphaSquared;
    float distribution = alphaSquared / (PI * denominator * denominator);
    float lambdaView = sqrt(nv * nv * (1 - alphaSquared) + alphaSquared);
    float lambdaLight = sqrt(nl * nl * (1 - alphaSquared) + alphaSquared);
    float visibility = .5 / max(nl * lambdaView + nv * lambdaLight, SMITH_DENOMINATOR_FLOOR);
    float3 reflectance = float3(DIELECTRIC_REFLECTANCE) * (1 - bsdf.Metallic) + bsdf.BaseColor * bsdf.Metallic;
    float oneMinus = 1 - vh;
    float square = oneMinus * oneMinus;
    float3 fresnel = reflectance + (float3(1) - reflectance) * (square * square * oneMinus);
    bool isConductor = bsdf.Model == LUDUS_MATERIAL_MODEL_CONDUCTOR;
    if (isConductor == true)
        fresnel = ComputeConductorFresnelRgb(vh, bsdf.Eta, bsdf.K);
    float diffuseFraction = isConductor == true ? 0 : 1 - bsdf.Metallic;
    float3 value =
        fresnel * (distribution * visibility) + bsdf.BaseColor * (float3(1) - fresnel) * (diffuseFraction / PI);
    float specularPdf = distribution / (2 * (nv + lambdaView));
    float probability = GetSpecularProbability(bsdf);
    return float4(value, probability * specularPdf + (1 - probability) * nl / PI);
}
// SampleReferenceBsdf; returns zero for an ordinary null proposal.
float3 SampleBsdfDirection(Bsdf bsdf, float3 normalWorld, float3 viewWorld, float3 samples)
{
    if (dot(normalWorld, viewWorld) <= 0)
        return 0;
    float3 axisXWorld =
        NormalizeOrZero(cross(abs(normalWorld.z) < BASIS_POLE_COSINE ? float3(0, 0, 1) : float3(0, 1, 0), normalWorld));
    float3 axisYWorld = cross(normalWorld, axisXWorld);
    float radius = sqrt(samples.y);
    float angle = 2 * PI * samples.z;
    float diskX = radius * cos(angle);
    float diskY = radius * sin(angle);
    float3 directionWorld;
    if (samples.x >= GetSpecularProbability(bsdf))
        directionWorld = axisXWorld * diskX + axisYWorld * diskY + normalWorld * sqrt(max(0, 1 - samples.y));
    else
    {
        float3 viewTangent =
            float3(dot(viewWorld, axisXWorld), dot(viewWorld, axisYWorld), dot(viewWorld, normalWorld));
        float alpha = ComputeMaterialAlpha(bsdf.Model, bsdf.Roughness, bsdf.Alpha);
        float3 stretchedTangent = NormalizeOrZero(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));
        float tangentLength = stretchedTangent.x * stretchedTangent.x + stretchedTangent.y * stretchedTangent.y;
        float3 firstTangent = tangentLength > 0
                                  ? float3(-stretchedTangent.y, stretchedTangent.x, 0) / sqrt(tangentLength)
                                  : float3(1, 0, 0);
        float3 secondTangent = cross(stretchedTangent, firstTangent);
        float blend = .5 * (1 + stretchedTangent.z);
        float warpedY = (1 - blend) * sqrt(max(0, 1 - diskX * diskX)) + blend * diskY;
        float3 hemisphereTangent = firstTangent * diskX + secondTangent * warpedY +
                                   stretchedTangent * sqrt(max(0, 1 - diskX * diskX - warpedY * warpedY));
        float3 halfTangent = NormalizeOrZero(
            float3(alpha * hemisphereTangent.x, alpha * hemisphereTangent.y, max(0, hemisphereTangent.z)));
        float3 halfWorld = axisXWorld * halfTangent.x + axisYWorld * halfTangent.y + normalWorld * halfTangent.z;
        directionWorld = halfWorld * (2 * dot(viewWorld, halfWorld)) - viewWorld;
    }
    directionWorld = NormalizeOrZero(directionWorld);
    return dot(normalWorld, directionWorld) > 0 ? directionWorld : float3(0);
}
// ApplyReferenceNormal: authored tangents when present, otherwise the UV-derived frame.
float3 ApplySurfaceNormal(uint triangle, uint instance, float3 normalWorld, float3 normalTangent, float u, float v)
{
    float4 authoredWorld = InterpolateTriangle(triangle, 10, u, v);
    if (asuint(SurfaceValue(instance, 7).z) != 0 && abs(authoredWorld.w) > .5)
    {
        float3 tangentWorld = authoredWorld.xyz - normalWorld * dot(normalWorld, authoredWorld.xyz);
        if (dot(tangentWorld, tangentWorld) <= 0)
            return normalWorld;
        tangentWorld = NormalizeOrZero(tangentWorld);
        float3 bitangentWorld = cross(normalWorld, tangentWorld) * (authoredWorld.w < 0 ? -1 : 1);
        return NormalizeOrZero(tangentWorld * normalTangent.x + bitangentWorld * normalTangent.y +
                               normalWorld * normalTangent.z);
    }
    float2 uvScale = SurfaceValue(instance, 3).xy;
    float4 firstUv = TriangleValue(triangle, 6);
    float2 lastUv = TriangleValue(triangle, 7).xy;
    float3 originWorld = TriangleValue(triangle, 0).xyz;
    return ApplyNormalWorld(normalWorld,
                            normalTangent,
                            TriangleValue(triangle, 1).xyz - originWorld,
                            TriangleValue(triangle, 2).xyz - originWorld,
                            (firstUv.zw - firstUv.xy) * uvScale,
                            (lastUv - firstUv.xy) * uvScale);
}

// --- Transport (PrepareReferenceVertex) -----------------------------------------
struct PathSample
{
    float3 Emission;
    float3 Direct;
    float3 Indirect;
    uint Status;
    uint Failure;
};
struct LightProposal
{
    float3 DirectionWorld;
    float3 Radiance;
    float3 PositionWorld;
    float3 ErrorWorld;
    float3 NormalWorld;
    float DistanceWorld;
    float Pdf;
    bool IsDelta;
    bool HasFiniteEndpoint;
    // A resampled mode drew the emissive category; the vertex resamples an emitter itself.
    bool IsEmissiveCategory;
};
void MarkInvalid(inout PathSample inoutPath, uint depth, uint failure)
{
    inoutPath.Failure = failure;
    inoutPath.Status |= LUDUS_REFERENCE_INVALID_FLAG | (depth << LUDUS_REFERENCE_DEPTH_SHIFT);
}
void CountQuery(inout PathSample inoutPath, bool isVisibility)
{
    inoutPath.Status += isVisibility == true ? 1u << LUDUS_REFERENCE_VISIBILITY_SHIFT : 1u;
}
// AddTransport: primary emission, first-surface direct, then later transport.
void AddTransport(inout PathSample inoutPath, float3 contribution, uint depth, bool isEmission)
{
    if (isEmission == true && depth == 0)
        inoutPath.Emission += contribution;
    else if ((isEmission == true && depth == 1) || (isEmission == false && depth == 0))
        inoutPath.Direct += contribution;
    else
        inoutPath.Indirect += contribution;
}
bool HasValidTransport(PathSample path)
{
    return IsValidContribution(path.Emission) && IsValidContribution(path.Direct) && IsValidContribution(path.Indirect);
}
LightProposal SampleLight(uint pixel, uint sample, uint depth, float3 positionWorld)
{
    LightProposal result;
    result.DirectionWorld = 0;
    result.Radiance = 0;
    result.PositionWorld = 0;
    result.ErrorWorld = 0;
    result.NormalWorld = 0;
    result.DistanceWorld = 0;
    result.Pdf = 0;
    result.IsDelta = false;
    result.HasFiniteEndpoint = false;
    result.IsEmissiveCategory = false;
    uint count = GetLightCount();
    if (count == 0)
        return result;
    uint selected;
    if (TrySampleRandomBounded(Sampling.xy,
                               PixelRandomAddress(pixel, sample, GetBounceDimension(depth, PROPOSAL_SELECTION_SLOT)),
                               count,
                               selected) == false)
    {
        result.Pdf = -1;
        return result;
    }
    float first = RandomValue(pixel, sample, GetBounceDimension(depth, PROPOSAL_FIRST_SLOT));
    float second = RandomValue(pixel, sample, GetBounceDimension(depth, PROPOSAL_SECOND_SLOT));
    if (selected < Counts.w)
    {
        uint light = SurfaceLayout.y + selected * LUDUS_GPU_PREVIEW_LIGHT_WORDS;
        float4 colorAndKind = SceneValue(light);
        float4 positionAndRangeWorld = SceneValue(light + 1);
        float4 directionAndOuterWorld = SceneValue(light + 2);
        uint kind = uint(colorAndKind.w);
        result.IsDelta = true;
        result.Pdf = 1 / float(count);
        result.Radiance = colorAndKind.xyz;
        if (kind == DIRECTIONAL_LIGHT)
        {
            result.DirectionWorld = -directionAndOuterWorld.xyz;
            result.DistanceWorld = Options.x;
            return result;
        }
        float3 deltaWorld = positionAndRangeWorld.xyz - positionWorld;
        float square = dot(deltaWorld, deltaWorld);
        if (square <= 0)
        {
            result.Pdf = -1;
            return result;
        }
        result.DistanceWorld = sqrt(square);
        result.DirectionWorld = deltaWorld / result.DistanceWorld;
        result.PositionWorld = positionAndRangeWorld.xyz;
        result.HasFiniteEndpoint = true;
        if (result.DistanceWorld >= positionAndRangeWorld.w || result.DistanceWorld > Options.x)
        {
            result.Radiance = 0;
            return result;
        }
        float ratio = result.DistanceWorld / positionAndRangeWorld.w;
        float window = max(0, 1 - ratio * ratio * ratio * ratio);
        float angular = 1;
        if (kind == SPOT_LIGHT)
        {
            float cosine = dot(directionAndOuterWorld.xyz, -result.DirectionWorld);
            float t = clamp((cosine - directionAndOuterWorld.w) /
                                (SceneValue(light + LUDUS_GRAPH_LIGHT_SOURCE_WORD).x - directionAndOuterWorld.w),
                            0,
                            1);
            angular = t * t;
        }
        if (kind == AREA_ONE_SIDED_LIGHT || kind == AREA_TWO_SIDED_LIGHT)
        {
            float emitterCosine = dot(directionAndOuterWorld.xyz, -result.DirectionWorld);
            angular = kind == AREA_TWO_SIDED_LIGHT ? abs(emitterCosine) : max(emitterCosine, 0);
        }
        result.Radiance *= window * window * angular / max(square, NEAR_LIGHT_SQUARED_DISTANCE_FLOOR);
        return result;
    }
    if (selected - Counts.w < Counts.z)
    {
        if (IsResampled() == true)
        {
            result.IsEmissiveCategory = true;
            return result;
        }
        uint total = GetEmitterCumulative(Counts.z - 1);
        if (total == 0)
            return result; // No emitter carries power; the proposal contributes nothing.
        uint value;
        if (TrySampleRandomBounded(Sampling.xy,
                                   PixelRandomAddress(pixel, sample, GetBounceDimension(depth, EMITTER_SLOT)),
                                   total,
                                   value) == false)
        {
            result.Pdf = -1;
            return result;
        }
        uint emitter = SelectEmitter(value);
        uint triangle = GetEmitterTriangle(emitter);
        float root = sqrt(first);
        float u = root * (1 - second);
        float v = root - u;
        result.PositionWorld = GetPositionWorld(triangle, u, v);
        float3 deltaWorld = result.PositionWorld - positionWorld;
        result.DistanceWorld = length(deltaWorld);
        if (result.DistanceWorld <= 0)
        {
            result.Pdf = -1;
            return result;
        }
        result.DirectionWorld = deltaWorld / result.DistanceWorld;
        result.ErrorWorld = GetPositionErrorWorld(triangle);
        result.NormalWorld = GetGeometricNormalWorld(triangle);
        result.HasFiniteEndpoint = true;
        result.Pdf = GetAreaPdf(triangle, positionWorld, result.PositionWorld, GetEmitterSelection(emitter, count));
        uint instance = GetInstance(triangle);
        float2 uv = GetTexcoordUv(triangle, u, v);
        if ((IsEmissionTwoSided(instance) == true || dot(result.NormalWorld, -result.DirectionWorld) > 0) &&
            result.DistanceWorld <= Options.x && IsOpaque(instance, uv, GetVertexAlpha(triangle, u, v)) == true)
            result.Radiance = SampleEmission(instance, uv);
        return result;
    }
    float environmentPdf;
    SampleEnvironmentWorld(first, second, result.DirectionWorld, result.Radiance, environmentPdf);
    result.Pdf = environmentPdf / float(count);
    result.DistanceWorld = Options.x;
    return result;
}
// Trace one shadow segment for an accepted proposal; false reports an invalid ray.
bool TraceProposal(inout PathSample inoutPath,
                   LightProposal proposal,
                   uint triangle,
                   float3 positionWorld,
                   uint depth,
                   out bool outIsVisible)
{
    outIsVisible = false;
    Ray shadow;
    shadow.OriginWorld = OffsetOrigin(positionWorld, triangle, proposal.DirectionWorld);
    shadow.DirectionWorld = proposal.DirectionWorld;
    shadow.MinimumDistanceWorld = 0;
    shadow.MaximumDistanceWorld = proposal.DistanceWorld;
    if (proposal.HasFiniteEndpoint == true)
    {
        float3 endWorld = proposal.PositionWorld;
        if (proposal.IsDelta == false && TryOffsetOrigin(proposal.PositionWorld,
                                                         proposal.ErrorWorld,
                                                         proposal.NormalWorld,
                                                         -shadow.DirectionWorld,
                                                         endWorld) == false)
            return false;
        float3 deltaWorld = endWorld - shadow.OriginWorld;
        float distanceWorld = length(deltaWorld);
        if (distanceWorld <= 0 || isfinite(distanceWorld) == false)
            return false;
        shadow.DirectionWorld = deltaWorld / distanceWorld;
        // The target endpoint is excluded by one representable step.
        shadow.MaximumDistanceWorld = NextFloatDown(distanceWorld);
    }
    if (IsWithinWorld(shadow.OriginWorld) == false)
        return false;
    CountQuery(inoutPath, true);
    Hit blocker = TraceScene(shadow, true);
    if (blocker.HasExceededStack == true)
    {
        inoutPath.Status |= LUDUS_REFERENCE_CAPACITY_FLAG;
        return false;
    }
    outIsVisible = blocker.Triangle == NO_TRIANGLE;
    return true;
}
// --- Resampled emitter light (reference_light_sampling.cpp) ----------------------
// The resampled ReferenceLightSampling modes replace the power-CDF emitter draw of the emissive category: RIS over the
// power CDF with the LTC target (exact, or approximate for ResampledFormFactor), then one projected-solid-angle and
// one LTC sample of the selected triangle under clamped optimal MIS, each with its own shadow ray. The
// scene-independent pieces are in reference_light_sampling.slang.

// P_E: the uniform category draw's share of mesh emitters.
float GetEmissiveCategoryProbability(uint lightCount)
{
    return lightCount == 0 ? 0 : float(Counts.z) / float(lightCount);
}
// GetReferenceEmitterProbability: the power CDF's probability of emitter `emitter`.
float GetEmitterProbability(uint emitter)
{
    uint total = Counts.z == 0 ? 0 : GetEmitterCumulative(Counts.z - 1);
    if (emitter >= Counts.z || total == 0)
        return 0;
    uint previous = emitter == 0 ? 0 : GetEmitterCumulative(emitter - 1);
    return float(GetEmitterCumulative(emitter) - previous) / float(total);
}
// SelectReferenceEmissiveTriangle: one bounded draw over the CDF total; false without emitted power or for an
// exhausted bounded draw.
bool TrySelectEmissiveTriangle(uint pixel, uint sample, uint dimension, out uint outTriangle, out float outPmf)
{
    outTriangle = NO_TRIANGLE;
    outPmf = 0;
    uint total = Counts.z == 0 ? 0 : GetEmitterCumulative(Counts.z - 1);
    uint value;
    if (total == 0 ||
        TrySampleRandomBounded(Sampling.xy, PixelRandomAddress(pixel, sample, dimension), total, value) == false)
        return false;
    uint emitter = SelectEmitter(value);
    outTriangle = GetEmitterTriangle(emitter);
    outPmf = GetEmitterProbability(emitter);
    return true;
}
// EvaluateReferenceEmissiveTriangleSelectionPmf: zero when the triangle is not an emitter.
float EvaluateEmissiveTriangleSelectionPmf(uint triangle)
{
    uint emitter;
    return FindEmitter(triangle, emitter) == true ? GetEmitterProbability(emitter) : 0;
}

// One emissive triangle evaluated for one receiver and prepared for both light techniques.
struct EmitterTarget
{
    ReferenceTriangleLight Light;
    // (c_0, c_1) from the exact integrals: Peters' MIS coefficients.
    float2 Coefficients;
    // p-hat as the sampler version evaluates it: the coefficients' sum for version 2, the approximate integrals'
    // coefficients for version 3. Zero for a one-sided emitter seen from behind.
    float Target;
};

// TryLoadEmitter: the luminance of the emitter's constant emission factor towards the receiver.
float GetEmitterLuminanceTowards(ReferenceReceiverFrame frame, uint triangle)
{
    uint instance = GetInstance(triangle);
    // A planar triangle faces the receiver from every point on it, as the emitter-hit test sees it at one point.
    if (IsEmissionTwoSided(instance) == false &&
        dot(GetGeometricNormalWorld(triangle), frame.PositionWorld - TriangleValue(triangle, 0).xyz) <= 0)
        return 0;
    return ComputeReferenceLuminance(max(SurfaceValue(instance, 4).xyz, float3(0)));
}

// TryApproximateEmitterTarget: sampler version 3's p-hat for an emitter with positive emission.
bool TryApproximateEmitterTarget(ReferenceReceiverFrame frame,
                                 uint triangle,
                                 float emissionLuminance,
                                 out float outTarget)
{
    outTarget = 0;
    float2 integrals;
    if (TryApproximateReferenceTriangleIntegrals(frame,
                                                 TriangleValue(triangle, 0).xyz,
                                                 TriangleValue(triangle, 1).xyz,
                                                 TriangleValue(triangle, 2).xyz,
                                                 integrals) == false)
        return false;
    float target = emissionLuminance * frame.DiffuseAlbedoLuminance * integrals.x +
                   emissionLuminance * frame.SpecularAlbedoLuminance * integrals.y;
    if (IsFiniteBits(target) == false)
        return false;
    outTarget = target;
    return true;
}

// TryEvaluateEmitterTarget, for the selected emitter and BSDF hits. The constant emission factor bounds textured
// emission from above, so the target needs no texture lookup. False for a nonfinite polygon, coefficient or target.
// Thanks to the Slang authors, core module noinline attribute reference:
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html
// One copy of the two unrolled polygon preparations serves every site.
[noinline] bool TryEvaluateEmitterTarget(ReferenceReceiverFrame frame, uint triangle, out EmitterTarget outTarget) {
    outTarget.Light = MakeEmptyReferenceTriangleLight(frame);
    outTarget.Coefficients = float2(0);
    outTarget.Target = 0;
    float emissionLuminance = GetEmitterLuminanceTowards(frame, triangle);
    if ((emissionLuminance > 0) == false)
        return true;
    if (TryPrepareReferenceTriangleLight(frame,
                                         TriangleValue(triangle, 0).xyz,
                                         TriangleValue(triangle, 1).xyz,
                                         TriangleValue(triangle, 2).xyz,
                                         outTarget.Light) == false)
        return false;
    outTarget.Coefficients =
        float2(emissionLuminance * frame.DiffuseAlbedoLuminance * outTarget.Light.DiffuseIntegral,
               emissionLuminance * frame.SpecularAlbedoLuminance * outTarget.Light.SpecularIntegral);
    outTarget.Target = outTarget.Coefficients.x + outTarget.Coefficients.y;
    if (IsFiniteBits(outTarget.Target) == false)
        return false;
    return HasFormFactorTarget() == false ||
           TryApproximateEmitterTarget(frame, triangle, emissionLuminance, outTarget.Target);
}

// ReferenceTargetTotal: Z-hat and whether this vertex has computed it.
struct TargetTotal
{
    float Value;
    bool IsReady;
};
TargetTotal MakeTargetTotal()
{
    TargetTotal result;
    result.Value = 0;
    result.IsReady = false;
    return result;
}

// TryEvaluateResamplingTarget: p-hat alone for the reservoir's and Z-hat's candidates, without preparing polygons under
// sampler version 3.
[noinline] bool TryEvaluateResamplingTarget(ReferenceReceiverFrame frame, uint triangle, out float outTarget) {
    outTarget = 0;
    if (HasFormFactorTarget() == false)
    {
        EmitterTarget target;
        bool isEvaluated = TryEvaluateEmitterTarget(frame, triangle, target);
        outTarget = target.Target;
        return isEvaluated;
    }
    float emissionLuminance = GetEmitterLuminanceTowards(frame, triangle);
    if ((emissionLuminance > 0) == false)
        return true;
    return TryApproximateEmitterTarget(frame, triangle, emissionLuminance, outTarget);
}

// TryEnsureTargetTotal: Z-hat = (1/K) sum_k p-hat(t'_k) / p_src(t'_k) over the vertex's normalization slots.
bool TryEnsureTargetTotal(uint pixel,
                          uint sample,
                          uint depth,
                          ReferenceReceiverFrame frame,
                          inout TargetTotal inoutTotal)
{
    if (inoutTotal.IsReady == true)
        return true;
    float sum = 0;
    uint normalizationCount = GetRisNormalizationCount();
    for (uint k = 0; k < normalizationCount; ++k)
    {
        uint candidate;
        float pmf;
        if (TrySelectEmissiveTriangle(pixel,
                                      sample,
                                      GetBounceDimension(depth, LUDUS_REFERENCE_RIS_NORMALIZATION_FIRST_SLOT + k),
                                      candidate,
                                      pmf) == false)
            continue;
        float target;
        if (TryEvaluateResamplingTarget(frame, candidate, target) == false)
            return false;
        sum += target / pmf;
    }
    float estimate = sum / float(normalizationCount);
    if (IsFiniteBits(estimate) == false)
        return false;
    inoutTotal.Value = estimate;
    inoutTotal.IsReady = true;
    return true;
}

// TryEvaluateSurrogate: P-tilde(t) for an evaluated emitter, computing Z-hat on first use.
bool TryEvaluateSurrogate(uint pixel,
                          uint sample,
                          uint depth,
                          ReferenceReceiverFrame frame,
                          float emissiveCategoryProbability,
                          uint triangle,
                          EmitterTarget target,
                          inout TargetTotal inoutTotal,
                          out float outProbability)
{
    outProbability = 0;
    float sourcePmf = EvaluateEmissiveTriangleSelectionPmf(triangle);
    if ((sourcePmf > 0) == false || (target.Target > 0) == false || (emissiveCategoryProbability > 0) == false)
        return true;
    if (TryEnsureTargetTotal(pixel, sample, depth, frame, inoutTotal) == false)
        return false;
    outProbability = EvaluateReferenceSurrogateProbability(emissiveCategoryProbability,
                                                           target.Target,
                                                           sourcePmf,
                                                           inoutTotal.Value,
                                                           GetRisCandidateCount());
    return true;
}

// TryResampleEmitter: stream the vertex's M power-CDF candidates through the reservoir in slot order. A zero weight
// sum succeeds without a selection. False for a failed target evaluation or reservoir update.
bool TryResampleEmitter(uint pixel,
                        uint sample,
                        uint depth,
                        ReferenceReceiverFrame frame,
                        out uint outTriangle,
                        out float outContributionWeight,
                        out bool outHasSelection)
{
    outTriangle = NO_TRIANGLE;
    outContributionWeight = 0;
    outHasSelection = false;
    WeightedReservoir reservoir = MakeWeightedReservoir();
    uint selectedTriangle = NO_TRIANGLE;
    float selectedTarget = 0;
    uint candidateCount = GetRisCandidateCount();
    for (uint i = 0; i < candidateCount; ++i)
    {
        uint slot = LUDUS_REFERENCE_RIS_CANDIDATE_FIRST_SLOT + i * LUDUS_REFERENCE_RIS_SLOTS_PER_CANDIDATE;
        uint candidate;
        float pmf;
        if (TrySelectEmissiveTriangle(pixel, sample, GetBounceDimension(depth, slot), candidate, pmf) == false)
            return true;
        float target;
        if (TryEvaluateResamplingTarget(frame, candidate, target) == false)
            return false;
        bool isSelected;
        if (TryAddReservoirCandidate(reservoir,
                                     target / pmf,
                                     RandomValue(pixel, sample, GetBounceDimension(depth, slot + 1)),
                                     isSelected) == false)
            return false;
        if (isSelected == true)
        {
            selectedTriangle = candidate;
            selectedTarget = target;
        }
    }
    if (HasReservoirSelection(reservoir) == false)
        return true;
    float contributionWeight;
    if (TryComputeReservoirContributionWeight(reservoir, selectedTarget, contributionWeight) == false)
        return false;
    outTriangle = selectedTriangle;
    outContributionWeight = contributionWeight;
    outHasSelection = true;
    return true;
}

// ReferenceResampledLightSample: one technique's sample of the resampled emitter.
struct ResampledLightSample
{
    float3 DirectionWorld;
    float3 PositionWorld;
    float3 Radiance;
    float DistanceWorld;
    float TechniqueDensity;
    float TechniqueWeight;
    float LightDensity;
    bool HasSample;
};

// SampleTechnique: the direction, where it meets the triangle and the emission found there. Densities are evaluated at
// the normalized world direction, as the BSDF-hit density evaluates them. No sample when the technique is disabled or
// cannot sample the direction, or the direction misses the triangle in FP32. Not inlined, like
// TryEvaluateEmitterTarget, so both techniques share one copy.
[noinline] ResampledLightSample SampleTechnique(ReferenceReceiverFrame frame,
                                                uint triangle,
                                                EmitterTarget target,
                                                uint technique,
                                                float2 randomNumbers,
                                                float surrogateProbability) {
    ResampledLightSample result;
    result.DirectionWorld = 0;
    result.PositionWorld = 0;
    result.Radiance = 0;
    result.DistanceWorld = 0;
    result.TechniqueDensity = 0;
    result.TechniqueWeight = 0;
    result.LightDensity = 0;
    result.HasSample = false;
    float3 directionTangent = float3(0);
    bool isSampled = false;
    if (technique == REFERENCE_DIFFUSE_TECHNIQUE)
        isSampled = TrySampleProjectedSolidAnglePolygon(target.Light.Diffuse, randomNumbers, directionTangent);
    else if (target.Light.SpecularIntegral > 0)
        isSampled = TrySampleLinearlyTransformedCosinePolygon(target.Light.Specular, randomNumbers, directionTangent);
    if (isSampled == false)
        return result;
    float3 directionWorld = NormalizeOrZero(TransformReferenceTangentToWorld(frame, directionTangent));
    float2 densities = EvaluateReferenceLightTechniqueDensities(frame, target.Light, directionWorld);
    float techniqueDensity = technique == REFERENCE_DIFFUSE_TECHNIQUE ? densities.x : densities.y;
    float distanceWorld;
    float2 barycentric;
    if ((techniqueDensity > 0) == false || TryIntersectReferenceTrianglePlane(frame.PositionWorld,
                                                                              directionWorld,
                                                                              TriangleValue(triangle, 0).xyz,
                                                                              TriangleValue(triangle, 1).xyz,
                                                                              TriangleValue(triangle, 2).xyz,
                                                                              distanceWorld,
                                                                              barycentric) == false)
        return result;
    uint instance = GetInstance(triangle);
    float u = barycentric.x;
    float v = barycentric.y;
    float2 uv = GetTexcoordUv(triangle, u, v);
    result.DirectionWorld = directionWorld;
    result.PositionWorld = GetPositionWorld(triangle, u, v);
    result.DistanceWorld = distanceWorld;
    // Sidedness, range and opacity at the sampled point, as UniformArea applies them.
    if ((IsEmissionTwoSided(instance) == true || dot(GetGeometricNormalWorld(triangle), -directionWorld) > 0) &&
        distanceWorld <= Options.x && IsOpaque(instance, uv, GetVertexAlpha(triangle, u, v)) == true)
        result.Radiance = SampleEmission(instance, uv);
    float2 techniqueWeights =
        ComputeReferenceLightTechniqueWeights(target.Coefficients, densities, REFERENCE_LIGHT_TECHNIQUE_VISIBILITY);
    result.TechniqueDensity = techniqueDensity;
    result.TechniqueWeight = technique == REFERENCE_DIFFUSE_TECHNIQUE ? techniqueWeights.x : techniqueWeights.y;
    result.LightDensity = surrogateProbability * (densities.x + densities.y);
    result.HasSample = true;
    return result;
}

// SampleResampledEmitter (reference_path.cpp): the emissive branch of the light draw at positionWorld. Each technique's
// sample that the BSDF and the receiver's geometric side admit traces its shadow ray at once; nothing else adds
// transport between this vertex's emission and its continuation, so the sums follow the CPU oracle's order. False
// after MarkInvalid.
bool SampleResampledEmitter(inout PathSample inoutPath,
                            uint pixel,
                            uint sample,
                            uint depth,
                            uint triangle,
                            float3 positionWorld,
                            float3 geometricNormalWorld,
                            float3 normalWorld,
                            float3 viewWorld,
                            Bsdf bsdf,
                            float3 throughput,
                            ReferenceReceiverFrame frame,
                            float emissiveCategoryProbability,
                            inout TargetTotal inoutTotal)
{
    uint emitter;
    float contributionWeight;
    bool hasSelection;
    if (TryResampleEmitter(pixel, sample, depth, frame, emitter, contributionWeight, hasSelection) == false)
    {
        MarkInvalid(inoutPath, depth, LUDUS_REFERENCE_FAILURE_INVALID_LIGHT_SAMPLE);
        return false;
    }
    if (hasSelection == false || (contributionWeight > 0) == false || (emissiveCategoryProbability > 0) == false)
        return true;
    EmitterTarget target;
    float surrogateProbability = 0;
    if (TryEvaluateEmitterTarget(frame, emitter, target) == false ||
        TryEvaluateSurrogate(pixel,
                             sample,
                             depth,
                             frame,
                             emissiveCategoryProbability,
                             emitter,
                             target,
                             inoutTotal,
                             surrogateProbability) == false)
    {
        MarkInvalid(inoutPath, depth, LUDUS_REFERENCE_FAILURE_INVALID_LIGHT_SAMPLE);
        return false;
    }
    float contributionScale = contributionWeight / emissiveCategoryProbability;
    for (uint technique = REFERENCE_DIFFUSE_TECHNIQUE; technique <= REFERENCE_SPECULAR_TECHNIQUE; ++technique)
    {
        uint firstSlot = technique == REFERENCE_DIFFUSE_TECHNIQUE ? PROPOSAL_FIRST_SLOT : LTC_FIRST_SLOT;
        float2 randomNumbers = float2(RandomValue(pixel, sample, GetBounceDimension(depth, firstSlot)),
                                      RandomValue(pixel, sample, GetBounceDimension(depth, firstSlot + 1)));
        ResampledLightSample light =
            SampleTechnique(frame, emitter, target, technique, randomNumbers, surrogateProbability);
        if (IsValidContribution(light.Radiance) == false)
        {
            MarkInvalid(inoutPath, depth, LUDUS_REFERENCE_FAILURE_INVALID_LIGHT_SAMPLE);
            return false;
        }
        float cosine = dot(normalWorld, light.DirectionWorld);
        if (light.HasSample == false || HasPositiveComponent(light.Radiance) == false || cosine <= 0 ||
            dot(geometricNormalWorld, light.DirectionWorld) <= 0)
            continue;
        if (IsValidBsdf(bsdf) == false || IsUnit(normalWorld) == false || IsUnit(viewWorld) == false ||
            IsUnit(light.DirectionWorld) == false)
        {
            MarkInvalid(inoutPath, depth, LUDUS_REFERENCE_FAILURE_INVALID_BSDF_EVALUATION);
            return false;
        }
        float4 evaluation = EvaluateBsdf(bsdf, normalWorld, viewWorld, light.DirectionWorld);
        if (IsFiniteVector(evaluation.xyz) == false || isfinite(evaluation.w) == false || evaluation.w < 0)
        {
            MarkInvalid(inoutPath, depth, LUDUS_REFERENCE_FAILURE_INVALID_BSDF_EVALUATION);
            return false;
        }
        float weight = light.TechniqueWeight * PowerWeight(light.LightDensity, evaluation.w);
        float3 contribution = throughput * evaluation.xyz * light.Radiance *
                              (cosine * weight / light.TechniqueDensity * contributionScale);
        if (IsValidContribution(contribution) == false)
        {
            MarkInvalid(inoutPath, depth, LUDUS_REFERENCE_FAILURE_INVALID_DIRECT_CONTRIBUTION);
            return false;
        }
        if (HasPositiveComponent(contribution) == false)
            continue;
        LightProposal proposal;
        proposal.DirectionWorld = light.DirectionWorld;
        proposal.Radiance = light.Radiance;
        proposal.PositionWorld = light.PositionWorld;
        proposal.ErrorWorld = GetPositionErrorWorld(emitter);
        proposal.NormalWorld = GetGeometricNormalWorld(emitter);
        proposal.DistanceWorld = light.DistanceWorld;
        proposal.Pdf = light.TechniqueDensity;
        proposal.IsDelta = false;
        proposal.HasFiniteEndpoint = true;
        proposal.IsEmissiveCategory = false;
        bool isVisible;
        if (TraceProposal(inoutPath, proposal, triangle, positionWorld, depth, isVisible) == false)
        {
            MarkInvalid(inoutPath,
                        depth,
                        (inoutPath.Status & LUDUS_REFERENCE_CAPACITY_FLAG) != 0
                            ? LUDUS_REFERENCE_FAILURE_TRAVERSAL_CAPACITY_EXCEEDED
                            : LUDUS_REFERENCE_FAILURE_INVALID_SHADOW_SEGMENT);
            return false;
        }
        if (isVisible == true)
            AddTransport(inoutPath, contribution, depth, false);
    }
    return true;
}

// EvaluateReferenceResampledLightDensity: q = P-tilde(t) (p_0 + p_1) for a BSDF direction from the receiver of vertex
// `depth` that reached emitter triangle `triangle`. False for a nonfinite evaluation.
bool TryEvaluateResampledLightDensity(uint pixel,
                                      uint sample,
                                      uint depth,
                                      ReferenceReceiverFrame frame,
                                      float emissiveCategoryProbability,
                                      uint triangle,
                                      float3 directionWorld,
                                      inout TargetTotal inoutTotal,
                                      out float outDensity)
{
    outDensity = 0;
    if ((EvaluateEmissiveTriangleSelectionPmf(triangle) > 0) == false)
        return true;
    EmitterTarget target;
    float surrogateProbability;
    if (TryEvaluateEmitterTarget(frame, triangle, target) == false ||
        TryEvaluateSurrogate(pixel,
                             sample,
                             depth,
                             frame,
                             emissiveCategoryProbability,
                             triangle,
                             target,
                             inoutTotal,
                             surrogateProbability) == false)
        return false;
    float2 densities = EvaluateReferenceLightTechniqueDensities(frame, target.Light, directionWorld);
    float density = surrogateProbability * (densities.x + densities.y);
    if (IsFiniteBits(density) == false || density < 0)
        return false;
    outDensity = density;
    return true;
}

PathSample TracePath(uint pixel, uint sample)
{
    PathSample path;
    path.Emission = 0;
    path.Direct = 0;
    path.Indirect = 0;
    path.Status = 0;
    path.Failure = LUDUS_REFERENCE_FAILURE_NONE;
    if (SurfaceLayout.x != LUDUS_GPU_PREVIEW_SURFACE_WORDS || SurfaceLayout.z != LUDUS_GPU_PREVIEW_KERNEL_VERSION)
    {
        MarkInvalid(path, 0, LUDUS_REFERENCE_FAILURE_INVALID_SCENE_LAYOUT);
        return path;
    }
    Ray ray;
    if (GenerateCameraRay(pixel, sample, ray) == false)
    {
        MarkInvalid(path, 0, LUDUS_REFERENCE_FAILURE_INVALID_CAMERA_RAY);
        return path;
    }
    float3 throughput = 1;
    float3 previousPositionWorld = 0;
    float previousPdf = 0;
    uint lights = GetLightCount();
    // The resampled modes keep the previous vertex's receiver and Z-hat for the BSDF-hit density.
    bool isResampled = IsResampled();
    float emissiveCategoryProbability = GetEmissiveCategoryProbability(lights);
    ReferenceReceiverFrame previousFrame = (ReferenceReceiverFrame)0;
    TargetTotal previousTotal = MakeTargetTotal();
    for (uint depth = 0;;)
    {
        CountQuery(path, false);
        Hit hit = TraceScene(ray, false);
        if (hit.HasExceededStack == true)
        {
            path.Status |= LUDUS_REFERENCE_CAPACITY_FLAG;
            MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_TRAVERSAL_CAPACITY_EXCEEDED);
            return path;
        }
        if (hit.Triangle == NO_TRIANGLE)
        {
            float4 environment = EvaluateEnvironmentWorld(ray.DirectionWorld);
            float lightPdf = lights == 0 ? 0 : environment.w / float(lights);
            float weight = depth == 0 ? 1 : PowerWeight(previousPdf, lightPdf);
            AddTransport(path, throughput * environment.xyz * weight, depth, true);
            if (HasValidTransport(path) == false)
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_RADIANCE);
            return path;
        }
        uint triangle = hit.Triangle;
        uint instance = GetInstance(triangle);
        float u = hit.U;
        float v = hit.V;
        float2 uv = GetTexcoordUv(triangle, u, v);
        float3 positionWorld = GetPositionWorld(triangle, u, v);
        float3 viewWorld = -ray.DirectionWorld;
        float3 geometricNormalWorld = GetGeometricNormalWorld(triangle);
        if (HasPositiveComponent(SurfaceValue(instance, 4).xyz) == true &&
            (IsEmissionTwoSided(instance) == true || dot(geometricNormalWorld, viewWorld) > 0))
        {
            float pdf = 0;
            if (isResampled == false)
            {
                uint emitter;
                float selection = FindEmitter(triangle, emitter) == true ? GetEmitterSelection(emitter, lights) : 0;
                pdf = GetAreaPdf(triangle, previousPositionWorld, positionWorld, selection);
            }
            else if (depth > 0 && TryEvaluateResampledLightDensity(pixel,
                                                                   sample,
                                                                   depth - 1,
                                                                   previousFrame,
                                                                   emissiveCategoryProbability,
                                                                   triangle,
                                                                   ray.DirectionWorld,
                                                                   previousTotal,
                                                                   pdf) == false)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_EMITTER_PDF);
                return path;
            }
            if (isfinite(pdf) == false || pdf < 0)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_EMITTER_PDF);
                return path;
            }
            float weight = depth == 0 ? 1 : PowerWeight(previousPdf, pdf);
            AddTransport(path, throughput * SampleEmission(instance, uv) * weight, depth, true);
        }
        if (depth >= Dimensions.z || dot(geometricNormalWorld, viewWorld) <= 0)
        {
            if (HasValidTransport(path) == false)
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_RADIANCE);
            return path;
        }
        float3 normalWorld = NormalizeOrZero(InterpolateTriangle(triangle, 3, u, v).xyz);
        float4 texel = SampleImage(instance, COLOR_IMAGE, uv);
        if (asuint(SurfaceValue(instance, 7).z) != 0)
            texel *= InterpolateTriangleColor(triangle, u, v);
        float4 baseAndMetallic = SurfaceValue(instance, 0);
        Bsdf bsdf;
        bsdf.BaseColor = texel.xyz * baseAndMetallic.xyz;
        bsdf.Metallic = baseAndMetallic.w;
        bsdf.Roughness = SurfaceValue(instance, 1).x;
        float4 etaAndModel = SurfaceValue(instance, LUDUS_GPU_PREVIEW_SCATTERING_ETA_WORD);
        float4 extinctionAndAlpha = SurfaceValue(instance, LUDUS_GPU_PREVIEW_SCATTERING_K_WORD);
        bsdf.Model = asuint(etaAndModel.w);
        bsdf.Eta = etaAndModel.xyz;
        bsdf.K = extinctionAndAlpha.xyz;
        bsdf.Alpha = extinctionAndAlpha.w;
        float4 surfaceMap = SurfaceValue(instance, 6);
        if (asuint(surfaceMap.w) != 0)
        {
            float4 parametersTexel = SampleImage(instance, SURFACE_IMAGE, uv);
            if (all(isfinite(parametersTexel)) == false || any(parametersTexel < 0) || any(parametersTexel > 1))
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_SURFACE_PARAMETERS);
                return path;
            }
            SurfaceParameters parameters =
                DecodeSurfaceParameters(parametersTexel, bsdf.Metallic, bsdf.Roughness, surfaceMap.x);
            bsdf.Metallic = parameters.Metallic;
            bsdf.Roughness = parameters.Roughness;
            normalWorld = ApplySurfaceNormal(triangle, instance, normalWorld, parameters.NormalTangent, u, v);
        }
        ReferenceReceiverFrame frame = previousFrame;
        TargetTotal targetTotal = MakeTargetTotal();
        if (isResampled == true)
            frame = MakeReferenceReceiverFrame(positionWorld,
                                               normalWorld,
                                               viewWorld,
                                               bsdf.BaseColor,
                                               bsdf.Metallic,
                                               bsdf.Roughness);
        LightProposal proposal = SampleLight(pixel, sample, depth, positionWorld);
        if (isfinite(proposal.Pdf) == false || proposal.Pdf < 0 || IsValidContribution(proposal.Radiance) == false)
        {
            MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_LIGHT_SAMPLE);
            return path;
        }
        if (proposal.IsEmissiveCategory == true && SampleResampledEmitter(path,
                                                                          pixel,
                                                                          sample,
                                                                          depth,
                                                                          triangle,
                                                                          positionWorld,
                                                                          geometricNormalWorld,
                                                                          normalWorld,
                                                                          viewWorld,
                                                                          bsdf,
                                                                          throughput,
                                                                          frame,
                                                                          emissiveCategoryProbability,
                                                                          targetTotal) == false)
            return path;
        bool hasShadow = false;
        float3 pendingDirect = 0;
        uint directDepth = depth;
        float cosine = dot(normalWorld, proposal.DirectionWorld);
        if (proposal.Pdf > 0 && HasPositiveComponent(proposal.Radiance) == true && cosine > 0 &&
            dot(geometricNormalWorld, proposal.DirectionWorld) > 0)
        {
            if (IsValidBsdf(bsdf) == false || IsUnit(normalWorld) == false || IsUnit(viewWorld) == false ||
                IsUnit(proposal.DirectionWorld) == false)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_BSDF_EVALUATION);
                return path;
            }
            float4 evaluation = EvaluateBsdf(bsdf, normalWorld, viewWorld, proposal.DirectionWorld);
            if (IsFiniteVector(evaluation.xyz) == false || isfinite(evaluation.w) == false || evaluation.w < 0)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_BSDF_EVALUATION);
                return path;
            }
            float weight = proposal.IsDelta == true ? 1 : PowerWeight(proposal.Pdf, evaluation.w);
            pendingDirect = throughput * evaluation.xyz * proposal.Radiance * (cosine * weight / proposal.Pdf);
            if (IsValidContribution(pendingDirect) == false)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_DIRECT_CONTRIBUTION);
                return path;
            }
            hasShadow = HasPositiveComponent(pendingDirect);
        }
        float3 samples = float3(RandomValue(pixel, sample, GetBounceDimension(depth, BSDF_LOBE_SLOT)),
                                RandomValue(pixel, sample, GetBounceDimension(depth, BSDF_FIRST_SLOT)),
                                RandomValue(pixel, sample, GetBounceDimension(depth, BSDF_SECOND_SLOT)));
        if (IsValidBsdf(bsdf) == false || IsUnit(normalWorld) == false || IsUnit(viewWorld) == false ||
            any(samples < 0) || any(samples >= 1))
        {
            MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_BSDF_SAMPLE);
            return path;
        }
        float3 outgoingWorld = SampleBsdfDirection(bsdf, normalWorld, viewWorld, samples);
        bool isAccepted = all(outgoingWorld == 0) == false;
        float4 evaluation = 0;
        if (isAccepted == true)
        {
            evaluation = EvaluateBsdf(bsdf, normalWorld, viewWorld, outgoingWorld);
            if (IsFiniteVector(evaluation.xyz) == false || isfinite(evaluation.w) == false || evaluation.w <= 0)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_BSDF_SAMPLE);
                return path;
            }
        }
        bool hasNext = false;
        if (isAccepted == false || dot(geometricNormalWorld, outgoingWorld) <= 0)
            path.Status |= LUDUS_REFERENCE_NULL_FLAG;
        else
        {
            throughput *= evaluation.xyz * (dot(normalWorld, outgoingWorld) / evaluation.w);
            if (IsValidContribution(throughput) == false)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_THROUGHPUT);
                return path;
            }
            if (HasPositiveComponent(throughput) == true)
            {
                previousPositionWorld = positionWorld;
                previousPdf = evaluation.w;
                previousFrame = frame;
                previousTotal = targetTotal;
                ++depth;
                hasNext = true;
                if (depth >= Dimensions.w)
                {
                    float survival = clamp(max(max(throughput.x, throughput.y), throughput.z),
                                           ROULETTE_MINIMUM_SURVIVAL,
                                           ROULETTE_MAXIMUM_SURVIVAL);
                    if (RandomValue(pixel, sample, GetBounceDimension(depth, ROULETTE_SLOT)) >= survival)
                    {
                        path.Status |= LUDUS_REFERENCE_ROULETTE_FLAG;
                        hasNext = false;
                    }
                    else
                        throughput /= survival;
                }
            }
        }
        Ray next;
        if (hasNext == true)
        {
            next.OriginWorld = OffsetOrigin(positionWorld, triangle, outgoingWorld);
            next.DirectionWorld = outgoingWorld;
            next.MinimumDistanceWorld = 0;
            next.MaximumDistanceWorld = Options.x;
            if (IsFiniteVector(next.OriginWorld) == false)
            {
                MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_RAY_ORIGIN);
                return path;
            }
        }
        if (HasValidTransport(path) == false)
        {
            MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_RADIANCE);
            return path;
        }
        if (hasShadow == true)
        {
            bool isVisible;
            if (TraceProposal(path, proposal, triangle, positionWorld, depth, isVisible) == false)
            {
                MarkInvalid(path,
                            depth,
                            (path.Status & LUDUS_REFERENCE_CAPACITY_FLAG) != 0
                                ? LUDUS_REFERENCE_FAILURE_TRAVERSAL_CAPACITY_EXCEEDED
                                : LUDUS_REFERENCE_FAILURE_INVALID_SHADOW_SEGMENT);
                return path;
            }
            if (isVisible == true)
                AddTransport(path, pendingDirect, directDepth, false);
        }
        if (hasNext == false)
            return path;
        if (IsWithinWorld(next.OriginWorld) == false)
        {
            MarkInvalid(path, depth, LUDUS_REFERENCE_FAILURE_INVALID_RAY_ORIGIN);
            return path;
        }
        ray = next;
    }
    return path;
}

// Kahan-compensated committed sum, matching ReferenceSum::Add.
void AddCompensated(inout float3 inoutSum, inout float3 inoutCompensation, float3 value)
{
    float3 adjusted = value - inoutCompensation;
    float3 combined = inoutSum + adjusted;
    inoutCompensation = (combined - inoutSum) - adjusted;
    inoutSum = combined;
}
bool IsValidSum(float3 sum, float3 compensation)
{
    return IsFiniteVector(sum) && IsFiniteVector(compensation) && all(sum >= 0);
}

// One sample for every pixel still at the image's minimum committed count. A pixel
// commits its own sample only when every AOV sum stays finite; a failed pixel keeps
// its previous sums and retries the same addressed sample on a later dispatch.
LUDUS_REFERENCE_COMPUTE_ENTRY void TraceMain(uint3 positionPixel : SV_DispatchThreadID)
{
    if (positionPixel.x >= Dimensions.x || positionPixel.y >= Dimensions.y)
        return;
    uint pixel = positionPixel.y * Dimensions.x + positionPixel.x;
    uint pixels = Dimensions.x * Dimensions.y;
    uint sumIndex = pixel * LUDUS_REFERENCE_SUM_WORDS;
    uint compensationIndex = pixels * LUDUS_REFERENCE_SUM_WORDS + sumIndex;
    float4 emission = accumulation[sumIndex];
    float4 direct = accumulation[sumIndex + 1];
    float4 indirect = accumulation[sumIndex + 2];
    uint samples = asuint(emission.w);
    uint word = 0;
    uint failure = LUDUS_REFERENCE_FAILURE_NONE;
    if (samples == status[0].x)
    {
        PathSample path = TracePath(pixel, samples);
        word = path.Status;
        failure = path.Failure;
        if ((word & LUDUS_REFERENCE_INVALID_FLAG) == 0)
        {
            float3 emissionCompensation = accumulation[compensationIndex].xyz;
            float3 directCompensation = accumulation[compensationIndex + 1].xyz;
            float3 indirectCompensation = accumulation[compensationIndex + 2].xyz;
            float3 emissionSum = emission.xyz;
            float3 directSum = direct.xyz;
            float3 indirectSum = indirect.xyz;
            AddCompensated(emissionSum, emissionCompensation, path.Emission);
            AddCompensated(directSum, directCompensation, path.Direct);
            AddCompensated(indirectSum, indirectCompensation, path.Indirect);
            if (IsValidSum(emissionSum, emissionCompensation) == false ||
                IsValidSum(directSum, directCompensation) == false ||
                IsValidSum(indirectSum, indirectCompensation) == false ||
                IsFiniteVector(emissionSum + directSum + indirectSum) == false)
            {
                word |= LUDUS_REFERENCE_INVALID_FLAG;
                failure = LUDUS_REFERENCE_FAILURE_INVALID_ACCUMULATION;
            }
            else
            {
                word |= LUDUS_REFERENCE_COMMITTED_FLAG;
                emission = float4(emissionSum, asfloat(samples + 1));
                direct.xyz = directSum;
                indirect.xyz = indirectSum;
                accumulation[sumIndex] = emission;
                accumulation[compensationIndex] = float4(emissionCompensation, 0);
                accumulation[compensationIndex + 1] = float4(directCompensation, 0);
                accumulation[compensationIndex + 2] = float4(indirectCompensation, 0);
            }
        }
    }
    indirect.w = asfloat(failure);
    accumulation[sumIndex + 2] = indirect;
    direct.w = asfloat(word);
    accumulation[sumIndex + 1] = direct;
}

// Single-group reduction of the per-pixel dispatch status into the status words.
// Each lane scans a strided pixel subset; lane zero combines the partial results.
groupshared uint4 sharedSamples[LUDUS_REFERENCE_GROUP_WIDTH];
groupshared uint4 sharedQueries[LUDUS_REFERENCE_GROUP_WIDTH];
groupshared uint4 sharedFailures[LUDUS_REFERENCE_GROUP_WIDTH];
LUDUS_REFERENCE_COMPUTE_ENTRY void ReduceMain(uint3 lane : SV_GroupThreadID)
{
    uint pixels = Dimensions.x * Dimensions.y;
    // (minimum samples, maximum samples, committed, invalid)
    uint4 samples = uint4(0xffffffff, 0, 0, 0);
    // (closest, visibility, null BSDF, roulette)
    uint4 queries = 0;
    // (lowest failing pixel, its depth, capacity failures, validation failure code)
    uint4 failures = uint4(LUDUS_REFERENCE_INVALID_PIXEL, 0, 0, 0);
    for (uint pixel = lane.x; pixel < pixels; pixel += LUDUS_REFERENCE_GROUP_WIDTH)
    {
        uint sumIndex = pixel * LUDUS_REFERENCE_SUM_WORDS;
        uint count = asuint(accumulation[sumIndex].w);
        uint word = asuint(accumulation[sumIndex + 1].w);
        samples.x = min(samples.x, count);
        samples.y = max(samples.y, count);
        samples.z += (word & LUDUS_REFERENCE_COMMITTED_FLAG) != 0 ? 1u : 0u;
        queries.x += word & LUDUS_REFERENCE_QUERY_MASK;
        queries.y += (word >> LUDUS_REFERENCE_VISIBILITY_SHIFT) & LUDUS_REFERENCE_QUERY_MASK;
        queries.z += (word & LUDUS_REFERENCE_NULL_FLAG) != 0 ? 1u : 0u;
        queries.w += (word & LUDUS_REFERENCE_ROULETTE_FLAG) != 0 ? 1u : 0u;
        failures.z += (word & LUDUS_REFERENCE_CAPACITY_FLAG) != 0 ? 1u : 0u;
        if ((word & LUDUS_REFERENCE_INVALID_FLAG) != 0)
        {
            samples.w += 1;
            if (failures.x == LUDUS_REFERENCE_INVALID_PIXEL)
            {
                failures.xy = uint2(pixel, word >> LUDUS_REFERENCE_DEPTH_SHIFT);
                failures.w = asuint(accumulation[sumIndex + 2].w);
            }
        }
    }
    sharedSamples[lane.x] = samples;
    sharedQueries[lane.x] = queries;
    sharedFailures[lane.x] = failures;
    GroupMemoryBarrierWithGroupSync();
    if (lane.x != 0)
        return;
    for (uint other = 1; other < LUDUS_REFERENCE_GROUP_WIDTH; ++other)
    {
        uint4 otherSamples = sharedSamples[other];
        uint4 otherFailures = sharedFailures[other];
        samples.x = min(samples.x, otherSamples.x);
        samples.y = max(samples.y, otherSamples.y);
        samples.zw += otherSamples.zw;
        queries += sharedQueries[other];
        failures.z += otherFailures.z;
        if (otherFailures.x < failures.x)
        {
            failures.xy = otherFailures.xy;
            failures.w = otherFailures.w;
        }
    }
    status[0] = samples;
    status[1] = queries;
    status[2] = failures;
}
