#pragma once
// Thanks to Christopher A. Burns and Warren A. Hunt, "The Visibility Buffer: A Cache-Friendly
// Approach to Deferred Shading", JCGT 2(2), 2013, https://jcgt.org/published/0002/02/04/ .
// This variant stores a mesh-local triangle and extraction instance in normalized RGBA bytes.
static const uint VISIBILITY_BYTE_MASK = 255;
static const uint VISIBILITY_BYTE_BITS = 8;
static const uint VISIBILITY_HALF_BITS = 16;
static const uint VISIBILITY_MAX_PRIMITIVE = 0xffffff;
static const uint VISIBILITY_MAX_INSTANCE_COUNT = VISIBILITY_BYTE_MASK - 1;
static const float VISIBILITY_BYTE_SCALE = 255.0;

/// Decode the exact low/high 16-bit primitive components supplied at raster vertex location 6.
uint GetRasterPrimitiveIndex(float2 primitive)
{
    uint2 halves = uint2(round(primitive));
    return halves.x | (halves.y << VISIBILITY_HALF_BITS);
}
/// Pack instance/primitive to RGB primitive bytes and A=instance+1. Clear RGBA=0 is background.
/// Out-of-range IDs produce an invalid sentinel which reconstruction reports as failure.
/// Use opaque writes, no sRGB conversion, filtering, blending, mipmaps or MSAA resolve.
float4 EncodeVisibility(uint instanceIndex, uint primitiveIndex)
{
    if (instanceIndex >= VISIBILITY_MAX_INSTANCE_COUNT || primitiveIndex > VISIBILITY_MAX_PRIMITIVE)
        return float4(1, 0, 1, 1);
    uint4 bytes = uint4(primitiveIndex & VISIBILITY_BYTE_MASK,
                        (primitiveIndex >> VISIBILITY_BYTE_BITS) & VISIBILITY_BYTE_MASK,
                        (primitiveIndex >> VISIBILITY_HALF_BITS) & VISIBILITY_BYTE_MASK,
                        instanceIndex + 1);
    return float4(bytes) / VISIBILITY_BYTE_SCALE;
}

#ifdef LUDUS_RENDER_GRAPH_VISIBILITY_INPUTS
LUDUS_SHADER_BINDING(LUDUS_GRAPH_VISIBILITY_BINDING) Texture2D<float4> GraphVisibility;
LUDUS_SHADER_BINDING(LUDUS_GRAPH_VISIBILITY_DEPTH_BINDING) Texture2D<float> GraphVisibilityDepth;
static const float VISIBILITY_BARYCENTRIC_TOLERANCE = 0.001;

/// Resolve the stored instance/local primitive to packed ray-scene triangle without traversal.
/// Identity table is immutable with the scene; material slots and overrides need no G-buffer field.
uint FindVisibilityTriangle(SceneContext scene, uint instanceIndex, uint primitiveIndex)
{
    uint first = 0;
    uint count = Counts.x;
    uint table = Counts.x * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    while (count != 0)
    {
        uint step = count / 2;
        uint middle = first + step;
        uint2 key = asuint(SceneValue(table + middle).xy);
        if (key.x < instanceIndex || (key.x == instanceIndex && key.y < primitiveIndex))
        {
            first = middle + 1;
            count -= step + 1;
        }
        else
            count = step;
    }
    if (scene.Version != SCENE_INTERFACE_VERSION || first >= Counts.x)
        return SCENE_INVALID_INDEX;
    uint3 record = asuint(SceneValue(table + first).xyz);
    return record.x == instanceIndex && record.y == primitiveIndex ? record.z : SCENE_INVALID_INDEX;
}

/// Load a center-sampled raster first hit and reconstruct its complete surface without a ray query.
/// Returns MISS for clear pixels, SURFACE for a valid hit, FAILURE for invalid identity/depth/geometry.
/// Failure latches queries.HasFailed. The query budget is unchanged. Source textures, scene and
/// launch must share generation, camera and extent; the graph runtime enforces the producer pairing.
uint LoadVisibilitySurface(SceneContext scene,
                           RayLaunch launch,
                           inout RayQueryContext inoutQueries,
                           out SurfaceHit outHit)
{
    outHit = (SurfaceHit)0;
    if (inoutQueries.HasFailed == true || any(launch.ExtentPixel == 0) ||
        any(launch.PositionPixel >= launch.ExtentPixel))
    {
        inoutQueries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    uint4 bytes = uint4(round(saturate(GraphVisibility.Load(int3(launch.PositionPixel, 0))) * VISIBILITY_BYTE_SCALE));
    if (bytes.w == 0)
        return SCENE_QUERY_MISS;
    uint primitiveIndex = bytes.x | (bytes.y << VISIBILITY_BYTE_BITS) | (bytes.z << VISIBILITY_HALF_BITS);
    uint triangleIndex = FindVisibilityTriangle(scene, bytes.w - 1, primitiveIndex);
    float depth = GraphVisibilityDepth.Load(int3(launch.PositionPixel, 0));
    if (triangleIndex == SCENE_INVALID_INDEX || isfinite(depth) == false || depth < 0 || depth > 1)
    {
        inoutQueries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    float2 positionUv = (float2(launch.PositionPixel) + 0.5) / float2(launch.ExtentPixel);
    float4 positionHomogeneousWorld =
        mul(launch.CameraClipToWorld, float4(positionUv.x * 2 - 1, 1 - positionUv.y * 2, depth, 1));
    if (any(isfinite(positionHomogeneousWorld) == false) || positionHomogeneousWorld.w == 0)
    {
        inoutQueries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    float3 positionWorld = positionHomogeneousWorld.xyz / positionHomogeneousWorld.w;
    TriangleData triangle = GetTriangle(scene, triangleIndex);
    float3 edge1World = triangle.Position1World - triangle.Position0World;
    float3 edge2World = triangle.Position2World - triangle.Position0World;
    float3 normalWorld = cross(edge1World, edge2World);
    float denominator = dot(normalWorld, normalWorld);
    float3 offsetWorld = positionWorld - triangle.Position0World;
    float2 barycentric =
        float2(dot(cross(offsetWorld, edge2World), normalWorld), dot(cross(edge1World, offsetWorld), normalWorld)) /
        denominator;
    if (denominator <= 0 || any(isfinite(barycentric) == false) ||
        any(barycentric < -VISIBILITY_BARYCENTRIC_TOLERANCE) ||
        barycentric.x + barycentric.y > 1 + VISIBILITY_BARYCENTRIC_TOLERANCE)
    {
        inoutQueries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    barycentric = max(barycentric, 0);
    barycentric /= max(1, barycentric.x + barycentric.y);
    outHit = GetSurfaceHit(scene, triangleIndex, barycentric);
    outHit.IncomingRay = launch.CameraRay;
    outHit.DistanceWorld = dot(outHit.PositionWorld - launch.CameraRay.OriginWorld, launch.CameraRay.DirectionWorld);
    outHit.IsFrontFace = dot(outHit.GeometricNormalWorld, -launch.CameraRay.DirectionWorld) > 0;
    if (outHit.IsValid == false || isfinite(outHit.DistanceWorld) == false || outHit.DistanceWorld < 0)
    {
        inoutQueries.HasFailed = true;
        return SCENE_QUERY_FAILURE;
    }
    return SCENE_QUERY_SURFACE;
}
#endif
