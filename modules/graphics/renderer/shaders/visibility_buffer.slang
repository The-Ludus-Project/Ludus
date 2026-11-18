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

/// Largest distance, in pixels at the closest approach, between the pixel-centre camera ray and the stored triangle.
/// The raster covers a pixel centre with a triangle whose vertices it snapped to a sub-pixel grid (1/256 pixel on
/// current hardware), so the ray passes within that distance of the triangle. A mismatched identity, camera or extent
/// places the triangle pixels away.
static const float VISIBILITY_COVERAGE_TOLERANCE_PIXELS = 1.0 / 16;

/// Closest approach of the launch's pixel-centre camera ray to the segment from startWorld to endWorld. outSegment is
/// the parameter in [0,1] of the segment's closest point, outMissSquared its squared distance from the ray, and
/// outFootprintSquared the squared pixel footprint there: the separation of the neighbouring pixel-centre rays at that
/// distance along the ray, the smaller of the two image axes, which holds for perspective and orthographic cameras.
void MeasureVisibilityEdgeMiss(RayLaunch launch,
                               float3 startWorld,
                               float3 endWorld,
                               out float outSegment,
                               out float outMissSquared,
                               out float outFootprintSquared)
{
    const Ray ray = launch.CameraRay;
    const float3 edgeWorld = endWorld - startWorld;
    const float3 startOffsetWorld = startWorld - ray.OriginWorld;
    const float edgeAlongRay = dot(edgeWorld, ray.DirectionWorld);
    // The squared distance from the ray's line is a convex quadratic in the segment parameter, minimised at the
    // unconstrained solution and clamped to the segment; an edge parallel to the ray takes its start.
    const float denominator = dot(edgeWorld, edgeWorld) - edgeAlongRay * edgeAlongRay;
    outSegment = 0;
    if (denominator > 0)
        outSegment =
            saturate((edgeAlongRay * dot(startOffsetWorld, ray.DirectionWorld) - dot(edgeWorld, startOffsetWorld)) /
                     denominator);
    const float3 pointWorld = startWorld + edgeWorld * outSegment;
    const float distanceWorld = dot(pointWorld - ray.OriginWorld, ray.DirectionWorld);
    const float3 rayPointWorld = ray.OriginWorld + ray.DirectionWorld * distanceWorld;
    const float3 missWorld = pointWorld - rayPointWorld;
    outMissSquared = dot(missWorld, missWorld);
    const Ray right = MakeCameraRay(launch, float2(1.5, 0.5));
    const Ray below = MakeCameraRay(launch, float2(0.5, 1.5));
    const float3 rightOffsetWorld = right.OriginWorld + right.DirectionWorld * distanceWorld - rayPointWorld;
    const float3 belowOffsetWorld = below.OriginWorld + below.DirectionWorld * distanceWorld - rayPointWorld;
    outFootprintSquared = min(dot(rightOffsetWorld, rightOffsetWorld), dot(belowOffsetWorld, belowOffsetWorld));
}
/// True when an edge's miss is within VISIBILITY_COVERAGE_TOLERANCE_PIXELS and, relative to its footprint, smaller
/// than the best accepted so far. Compares the ratios by cross-multiplication, so no footprint divides.
bool IsCloserVisibilityEdge(float missSquared,
                            float footprintSquared,
                            bool hasBest,
                            float bestMissSquared,
                            float bestFootprintSquared)
{
    if (isfinite(missSquared) == false || isfinite(footprintSquared) == false || footprintSquared <= 0 ||
        missSquared > VISIBILITY_COVERAGE_TOLERANCE_PIXELS * VISIBILITY_COVERAGE_TOLERANCE_PIXELS * footprintSquared)
        return false;
    return hasBest == false || missSquared * bestFootprintSquared < bestMissSquared * footprintSquared;
}
/// The boundary point of a triangle nearest the launch's pixel-centre camera ray, as barycentric weights of corners
/// one and two, when the ray passes within VISIBILITY_COVERAGE_TOLERANCE_PIXELS of the boundary. Returns false, with
/// zero weights, otherwise.
/// Out of line: inlined and run for every pixel, an earlier version of this pure test changed the hybrid reflections
/// kernel's radiance on Metal (Apple M5), although it accepted every pixel.
[noinline]
bool TryFindVisibilityEdgePoint(RayLaunch launch,
                                float3 position0World,
                                float3 position1World,
                                float3 position2World,
                                out float2 outBarycentric)
{
    outBarycentric = float2(0);
    bool hasBest = false;
    float bestMissSquared = 0;
    float bestFootprintSquared = 0;
    float segment;
    float missSquared;
    float footprintSquared;
    // Corner 0 to corner 1.
    MeasureVisibilityEdgeMiss(launch, position0World, position1World, segment, missSquared, footprintSquared);
    if (IsCloserVisibilityEdge(missSquared, footprintSquared, hasBest, bestMissSquared, bestFootprintSquared) == true)
    {
        outBarycentric = float2(segment, 0);
        hasBest = true;
        bestMissSquared = missSquared;
        bestFootprintSquared = footprintSquared;
    }
    // Corner 1 to corner 2.
    MeasureVisibilityEdgeMiss(launch, position1World, position2World, segment, missSquared, footprintSquared);
    if (IsCloserVisibilityEdge(missSquared, footprintSquared, hasBest, bestMissSquared, bestFootprintSquared) == true)
    {
        outBarycentric = float2(1 - segment, segment);
        hasBest = true;
        bestMissSquared = missSquared;
        bestFootprintSquared = footprintSquared;
    }
    // Corner 2 to corner 0.
    MeasureVisibilityEdgeMiss(launch, position2World, position0World, segment, missSquared, footprintSquared);
    if (IsCloserVisibilityEdge(missSquared, footprintSquared, hasBest, bestMissSquared, bestFootprintSquared) == true)
    {
        outBarycentric = float2(0, 1 - segment);
        hasBest = true;
    }
    return hasBest;
}

/// Load a center-sampled raster first hit and reconstruct its complete surface without a ray query.
/// Returns MISS for clear pixels, SURFACE for a valid hit, FAILURE for invalid identity/depth/geometry.
/// Failure latches queries.HasFailed. The query budget is unchanged. Source textures, scene and
/// launch must share generation, camera and extent; the graph runtime enforces the producer pairing.
SceneQuery LoadVisibilitySurfaceWithoutCapture(SceneContext scene,
                                               RayLaunch launch,
                                               inout RayQueryContext inoutQueries,
                                               out SurfaceHit outHit)
{
    outHit = (SurfaceHit)0;
    if (inoutQueries.HasFailed == true || any(launch.ExtentPixel == 0) ||
        any(launch.PositionPixel >= launch.ExtentPixel))
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_INVALID_ARGUMENT);
        return SceneQuery::Failure;
    }
    uint4 bytes = uint4(round(saturate(GraphVisibility.Load(int3(launch.PositionPixel, 0))) * VISIBILITY_BYTE_SCALE));
    if (bytes.w == 0)
        return SceneQuery::Miss;
    uint primitiveIndex = bytes.x | (bytes.y << VISIBILITY_BYTE_BITS) | (bytes.z << VISIBILITY_HALF_BITS);
    uint triangleIndex = FindVisibilityTriangle(scene, bytes.w - 1, primitiveIndex);
    float depth = GraphVisibilityDepth.Load(int3(launch.PositionPixel, 0));
    if (triangleIndex == SCENE_INVALID_INDEX || isfinite(depth) == false || depth < 0 || depth > 1)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return SceneQuery::Failure;
    }
    TriangleData triangle = GetTriangle(scene, triangleIndex);
    float3 edge1World = triangle.Position1World - triangle.Position0World;
    float3 edge2World = triangle.Position2World - triangle.Position0World;
    float3 normalWorld = cross(edge1World, edge2World);
    float denominator = dot(normalWorld, normalWorld);
    if (denominator <= 0 || isfinite(denominator) == false)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return SceneQuery::Failure;
    }
    // The stored identity and the pixel centre locate the first hit: where the pixel-centre camera ray meets the
    // stored triangle. Depth does not: conventional float32 depth places the point centimetres along that ray at tens
    // of metres, and at grazing incidence nearly all of that error lies in the triangle's plane, where it moves the
    // point off the triangle at its edges.
    const float3 directionWorld = launch.CameraRay.DirectionWorld;
    const float cosine = dot(directionWorld, normalWorld);
    float2 barycentric = float2(0);
    bool isInside = false;
    if (cosine != 0)
    {
        const float rayDistance = dot(triangle.Position0World - launch.CameraRay.OriginWorld, normalWorld) / cosine;
        const float3 offsetWorld =
            launch.CameraRay.OriginWorld + directionWorld * rayDistance - triangle.Position0World;
        barycentric =
            float2(dot(cross(offsetWorld, edge2World), normalWorld), dot(cross(edge1World, offsetWorld), normalWorld)) /
            denominator;
        isInside = isfinite(rayDistance) == true && rayDistance >= 0 && all(isfinite(barycentric)) == true &&
                   all(barycentric >= 0) == true && barycentric.x + barycentric.y <= 1;
    }
    // The raster covered the pixel centre with this triangle, so a ray that misses it, as rasterizer vertex snapping
    // and rounding allow at edges, passes just outside its boundary; the hit is the boundary point nearest the ray.
    if (isInside == false && TryFindVisibilityEdgePoint(launch,
                                                        triangle.Position0World,
                                                        triangle.Position1World,
                                                        triangle.Position2World,
                                                        barycentric) == false)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return SceneQuery::Failure;
    }
    outHit = GetSurfaceHit(scene, triangleIndex, barycentric);
    outHit.IncomingRay = launch.CameraRay;
    outHit.DistanceWorld = dot(outHit.PositionWorld - launch.CameraRay.OriginWorld, launch.CameraRay.DirectionWorld);
    outHit.IsFrontFace = dot(outHit.GeometricNormalWorld, -launch.CameraRay.DirectionWorld) > 0;
    if (outHit.IsValid == false || isfinite(outHit.DistanceWorld) == false || outHit.DistanceWorld < 0)
    {
        FailSceneQuery(inoutQueries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return SceneQuery::Failure;
    }
    return SceneQuery::Surface;
}
/// Load the raster first hit; capture variants record it as a RasterFirstHit event so paths
/// that start from the visibility buffer still begin at the camera.
SceneQuery
LoadVisibilitySurface(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries, out SurfaceHit outHit)
{
#    ifdef LUDUS_RAY_PATH_CAPTURE
    SceneQuery status = LoadVisibilitySurfaceWithoutCapture(scene, launch, inoutQueries, outHit);
    uint outcome = status == SceneQuery::Surface ? LUDUS_RAY_PATH_OUTCOME_HIT
                   : status == SceneQuery::Miss  ? LUDUS_RAY_PATH_OUTCOME_MISS
                                                 : LUDUS_RAY_PATH_OUTCOME_FAILURE;
    rayPathRejected = 0;
    AppendRayPathEvent(LUDUS_RAY_PATH_KIND_RASTER_FIRST_HIT,
                       launch.CameraRay.OriginWorld,
                       launch.CameraRay.DirectionWorld,
                       launch.CameraRay.MinDistanceWorld,
                       launch.CameraRay.MaxDistanceWorld,
                       outcome,
                       status == SceneQuery::Surface ? outHit.DistanceWorld : launch.CameraRay.MaxDistanceWorld,
                       status == SceneQuery::Surface ? outHit.TriangleIndex : LUDUS_GPU_PREVIEW_INVALID_TRIANGLE);
    return status;
#    else
    return LoadVisibilitySurfaceWithoutCapture(scene, launch, inoutQueries, outHit);
#    endif
}
#endif
