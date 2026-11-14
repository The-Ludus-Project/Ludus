#include "raster_bindings.slang"

// R16a/R17a: a compute-stage inline ray query whose scene is an ordinary RayScene binding, read by
// an ordered graph pass through a declared AccelerationStructureRead. Traversal is forced opaque;
// opacity filtering belongs to the fixed RT0 query kernel (ray_query.slang).
// Thanks to Khronos, Vulkan Ray Traversal specification (Ray Query),
// https://docs.vulkan.org/spec/latest/chapters/raytraversal.html, for the committed-hit queries.
LUDUS_SHADER_BINDING(0) RaytracingAccelerationStructure Scene;
LUDUS_SHADER_BINDING(1) StructuredBuffer<float4> Rays LUDUS_SHADER_TEXTURE_REGISTER(1);
LUDUS_SHADER_BINDING(2) RWStructuredBuffer<uint4> Hits LUDUS_SHADER_STORAGE_REGISTER(2);

static const uint GRAPH_RAY_COUNT = 4;
static const uint RAY_VECTOR_COUNT = 2;
static const uint ALL_INSTANCES_MASK = 255;

// Out of line like the renderer's TraceNative: the Metal shader build rejects a ray query
// inlined into the kernel body. Returns (hit, instance, primitive, distance bits); a miss is zero.
[noinline] uint4 TraceGraphRay(float4 originWorld, float4 directionWorld)
{
    RayDesc ray;
    ray.Origin = originWorld.xyz;
    ray.TMin = originWorld.w;
    ray.Direction = directionWorld.xyz;
    ray.TMax = directionWorld.w;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, RAY_FLAG_FORCE_OPAQUE, ALL_INSTANCES_MASK, ray);
    while (query.Proceed() == true)
    {
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return uint4(0);
    return uint4(1, query.CommittedInstanceIndex(), query.CommittedPrimitiveIndex(), asuint(query.CommittedRayT()));
}

// Each ray is two float4 values: world origin with minimum distance, then unit world direction
// with maximum distance.
[shader("compute")][numthreads(4, 1, 1)] void GraphQuery(uint3 id : SV_DispatchThreadID) {
    if (id.x >= GRAPH_RAY_COUNT)
        return;
    Hits[id.x] = TraceGraphRay(Rays[id.x * RAY_VECTOR_COUNT], Rays[id.x * RAY_VECTOR_COUNT + 1]);
}
