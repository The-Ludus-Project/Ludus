// Thanks to Apple, "Discover ray tracing with Metal", WWDC20 session 10012,
// triangle intersection functions (12:16), and "Get to know Metal function
// pointers", session 10013, visible function tables:
// https://developer.apple.com/videos/play/wwdc2020/10012/
// https://developer.apple.com/videos/play/wwdc2020/10013/
// The native MSL adapter supplies intersector/table operations; packed material
// acceptance stays here so native stages and inline queries share the same test.
#include "metal_ray_pipeline_layout.h"

struct NativeRayFunctionTableToken
{
    uint Value;
};
struct NativeRayIntersectionTableToken
{
    uint Value;
};
LUDUS_SHADER_BINDING(LUDUS_METAL_RAY_FUNCTION_TABLE_BINDING)
StructuredBuffer<NativeRayFunctionTableToken> NativeRayFunctions;
LUDUS_SHADER_BINDING(LUDUS_METAL_RAY_INTERSECTION_TABLE_BINDING)
StructuredBuffer<NativeRayIntersectionTableToken> NativeRayIntersections;

[noinline] uint LudusNativeResolveTriangle(uint primitive, uint geometry) {
    if ((Sampling.z & LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG) != 0 && geometry != 0)
        primitive += asuint(SceneValue(Offsets.y).y);
    return asuint(SceneValue(Offsets.y + primitive).x);
}

    [noinline] bool LudusNativeAcceptTriangle(uint primitive, uint geometry, float2 barycentric)
{
    if ((Sampling.z & LUDUS_GPU_PREVIEW_OPAQUE_FLAG) != 0 ||
        ((Sampling.z & LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG) != 0 && geometry == 0))
        return true;
    uint triangle = LudusNativeResolveTriangle(primitive, geometry);
    return asuint(Surface(triangle, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) == 0 ||
           Texture(triangle, TexcoordUv(triangle, barycentric)).w * Surface(triangle, 2).w *
                   InterpolateTriangleColor(triangle, barycentric).w >=
               Surface(triangle, 1).y;
}

// Opaque compiler tokens are legalized only by the independently verified MSL
// adapter. They are never uploaded as buffers, dereferenced, or exposed in the
// public shader layout. The resource registry owns the actual native tables.
__target_intrinsic(metal, "LudusTraceNative($0, $1, $2, $3, $4, $5, $6, LUDUS_NATIVE_CONTEXT)") uint4
    TraceMetalRayPipeline(RaytracingAccelerationStructure scene,
                          StructuredBuffer<NativeRayIntersectionTableToken> intersections,
                          float3 originWorld,
                          float3 directionWorld,
                          float nearDistanceWorld,
                          float farDistanceWorld,
                          bool isVisibility);

Hit TraceNative(float3 originWorld,
                float3 directionWorld,
                float nearDistanceWorld,
                float farDistanceWorld,
                bool isVisibility = false)
{
    Hit hit;
    // Graph invocations always use a positive depth. Retain the independently
    // compiled alpha and primitive mapping helpers for the native intersection
    // function, whose references are introduced after Slang emits MSL.
    if (Dimensions.z == 0)
    {
        hit.Triangle = LudusNativeResolveTriangle(0, 0);
        hit.DistanceWorld = LudusNativeAcceptTriangle(0, 0, float2(0)) ? nearDistanceWorld : farDistanceWorld;
        hit.Barycentric = 0;
        return hit;
    }
    uint4 result = TraceMetalRayPipeline(rayScene,
                                         NativeRayIntersections,
                                         originWorld,
                                         directionWorld,
                                         nearDistanceWorld,
                                         farDistanceWorld,
                                         isVisibility);
    hit.Triangle = result.x;
    hit.DistanceWorld = asfloat(result.y);
    hit.Barycentric = asfloat(result.zw);
    return hit;
}

bool TraceNativeVisibility(float3 originWorld, float3 directionWorld, float nearDistanceWorld, float farDistanceWorld)
{
    return TraceNative(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld, true).Triangle !=
           LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
}
