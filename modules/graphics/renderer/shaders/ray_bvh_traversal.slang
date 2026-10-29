// Float32 traversal of the renderer's packed binned-SAH BVH, shared by the GPU-compute
// RT1 reference kernel and render-graph ComputeBvh ray passes. It ports the CPU oracle's
// TraceReferenceOracle (reference_scene.cpp) node and leaf order, near-child-first stack,
// inclusive far endpoint and lowest-triangle tie rule; see renderer-systems.md for the
// float32 departures from that float64 oracle.
// Thanks to Sven Woop, Carsten Benthin and Ingo Wald, "Watertight Ray/Triangle
// Intersection", JCGT 2(1), 2013, section 3,
// https://jcgt.org/published/0002/01/05/paper.pdf, for the permuted/sheared edge
// test; this port omits the paper's float64 fallback for exactly-zero edges.
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based
// Rendering", 3rd edition, section 3.9.2, "Conservative Ray-Bounds Intersections",
// https://pbr-book.org/3ed-2018/Shapes/Managing_Rounding_Error, for widening the
// float32 slab exit by 1 + 2 * gamma(3).
//
// The including kernel supplies what ray_bvh_nodes.slang reads (SceneValue, Offsets and Counts) and
// IsBvhCandidateAccepted, its material opacity test for a candidate triangle.
#include "ray_bvh_nodes.slang"

// 1 + 2 * gamma(3), gamma(n) = n * epsilon / (1 - n * epsilon), epsilon = 2^-24.
static const float BVH_SLAB_EXIT_SCALE = 1.00000036;

bool IsBvhCandidateAccepted(uint triangle, float2 barycentric);

struct BvhRay
{
    float3 OriginWorld;
    float3 DirectionWorld;
    float MinimumDistanceWorld;
    float MaximumDistanceWorld;
};
struct BvhHit
{
    uint Triangle; // LUDUS_GPU_PREVIEW_INVALID_TRIANGLE for a miss
    float DistanceWorld;
    float2 Barycentric;
    bool HasExceededStack;
};
bool IntersectBvhBox(BvhRay ray, float3 minimumWorld, float3 maximumWorld, float maximumDistanceWorld)
{
    if (any(minimumWorld > maximumWorld))
        return false;
    float entry = ray.MinimumDistanceWorld;
    float exit = maximumDistanceWorld * BVH_SLAB_EXIT_SCALE;
    for (uint axis = 0; axis < 3; ++axis)
    {
        float origin = ray.OriginWorld[axis];
        float direction = ray.DirectionWorld[axis];
        if (direction == 0)
        {
            if (origin < minimumWorld[axis] || origin > maximumWorld[axis])
                return false;
            continue;
        }
        float near = (minimumWorld[axis] - origin) / direction;
        float far = (maximumWorld[axis] - origin) / direction;
        if (near > far)
        {
            float swapped = near;
            near = far;
            far = swapped;
        }
        entry = max(entry, near);
        exit = min(exit, far * BVH_SLAB_EXIT_SCALE);
        if (entry > exit)
            return false;
    }
    return true;
}
// The sheared 2D edge function first.x * second.y - first.y * second.x. Watertightness needs the
// two triangles sharing an edge to compute exactly negated values. Ordinary float arithmetic
// gives that, but Ludus compiles Metal with fast math, which may fuse either product into a
// multiply-add and break the antisymmetry, so rays through a shared edge miss both triangles.
// Evaluating in a canonical vertex order makes both triangles compute the same expression;
// without fusion the result is bit-identical to the direct evaluation.
float ComputeBvhEdgeFunction(float2 first, float2 second)
{
    bool isSwapped = first.x > second.x || (first.x == second.x && first.y > second.y);
    float2 low = isSwapped == true ? second : first;
    float2 high = isSwapped == true ? first : second;
    float value = low.x * high.y - low.y * high.x;
    return isSwapped == true ? -value : value;
}
bool IntersectBvhTriangle(uint triangle,
                          BvhRay ray,
                          float limitWorld,
                          out float outDistanceWorld,
                          out float2 outBarycentric)
{
    outDistanceWorld = 0;
    outBarycentric = 0;
    uint z = 0;
    for (uint axis = 1; axis < 3; ++axis)
    {
        if (abs(ray.DirectionWorld[axis]) > abs(ray.DirectionWorld[z]))
            z = axis;
    }
    uint x = (z + 1) % 3;
    uint y = (x + 1) % 3;
    if (ray.DirectionWorld[z] < 0)
    {
        uint swapped = x;
        x = y;
        y = swapped;
    }
    float shearX = ray.DirectionWorld[x] / ray.DirectionWorld[z];
    float shearY = ray.DirectionWorld[y] / ray.DirectionWorld[z];
    float shearZ = 1 / ray.DirectionWorld[z];
    float3 px;
    float3 py;
    float3 pz;
    for (uint corner = 0; corner < 3; ++corner)
    {
        float3 positionWorld = SceneValue(triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS + corner).xyz;
        float dz = positionWorld[z] - ray.OriginWorld[z];
        px[corner] = (positionWorld[x] - ray.OriginWorld[x]) - shearX * dz;
        py[corner] = (positionWorld[y] - ray.OriginWorld[y]) - shearY * dz;
        pz[corner] = dz * shearZ;
    }
    float3 edge = float3(ComputeBvhEdgeFunction(float2(px[2], py[2]), float2(px[1], py[1])),
                         ComputeBvhEdgeFunction(float2(px[0], py[0]), float2(px[2], py[2])),
                         ComputeBvhEdgeFunction(float2(px[1], py[1]), float2(px[0], py[0])));
    if (any(edge < 0) && any(edge > 0))
        return false;
    float determinant = edge.x + edge.y + edge.z;
    if (determinant == 0)
        return false;
    float distanceWorld = dot(edge, pz) / determinant;
    if (distanceWorld < ray.MinimumDistanceWorld || distanceWorld > limitWorld)
        return false;
    outDistanceWorld = distanceWorld;
    // Near the edge opposite corner 0, edge.x is zero or negligible and the two separately rounded
    // quotients can sum above one (Metal's fast-math division makes that more frequent), which
    // GetSurfaceHit rejects. Bound the third by 1 - second: 1 - second is exact or within half an
    // ulp, so the float32 sum rounds to at most one. The adjustment is within rounding error.
    float second = saturate(edge.y / determinant);
    float third = min(saturate(edge.z / determinant), 1 - second);
    outBarycentric = float2(second, third);
    return true;
}
/// Trace the ray interval [MinimumDistanceWorld, MaximumDistanceWorld] through the packed BVH.
/// Return the nearest accepted hit, preferring the lowest triangle at equal distances, or the
/// first accepted hit for visibility. A miss keeps the invalid-triangle sentinel. A stack
/// overflow sets HasExceededStack; callers must then ignore Triangle and report failure.
BvhHit TraceBvh(BvhRay ray, bool isVisibility)
{
    BvhHit result;
    result.Triangle = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    result.DistanceWorld = 0;
    result.Barycentric = 0;
    result.HasExceededStack = false;
    if (Counts.y == 0)
        return result;
    uint stack[LUDUS_REFERENCE_STACK_SIZE];
    stack[0] = 0;
    uint pending = 1;
    float closestWorld = ray.MaximumDistanceWorld;
    uint chosen = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    while (pending != 0)
    {
        --pending;
        BvhNode node = LoadBvhNode(stack[pending]);
        if (IntersectBvhBox(ray, node.MinimumWorld, node.MaximumWorld, closestWorld) == false)
            continue;
        if (node.Count == 0)
        {
            if (pending > BVH_MAXIMUM_PENDING)
            {
                result.HasExceededStack = true;
                return result;
            }
            // Near child last so it pops first; its closest hit then culls the far child.
            bool isReversed = ray.DirectionWorld[node.SplitAxis] < 0;
            stack[pending] = isReversed == true ? node.Left : node.Right;
            stack[pending + 1] = isReversed == true ? node.Right : node.Left;
            pending += 2;
            continue;
        }
        for (uint slot = node.First; slot < node.First + node.Count; ++slot)
        {
            uint triangle = GetBvhLeafTriangle(slot);
            float distanceWorld;
            float2 barycentric;
            if (IntersectBvhTriangle(triangle, ray, closestWorld, distanceWorld, barycentric) == false ||
                (distanceWorld == closestWorld && triangle >= chosen))
                continue;
            if (IsBvhCandidateAccepted(triangle, barycentric) == false)
                continue;
            closestWorld = distanceWorld;
            chosen = triangle;
            result.Triangle = triangle;
            result.DistanceWorld = distanceWorld;
            result.Barycentric = barycentric;
            if (isVisibility == true)
                return result;
        }
    }
    return result;
}
