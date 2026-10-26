// Thanks to Khronos, Vulkan Ray Traversal specification (Ray Query),
// https://docs.vulkan.org/spec/latest/chapters/raytraversal.html:
// candidates are accepted only after the fixed-mip opacity test, for both
// closest-hit and visibility queries. Native AS ownership is kept in RHI.
// SPIRV-Cross translates this fixed kernel to MSL 2.4 intersection_query because
// Ludus's pinned Slang direct-Metal target lacks TraceRayInline.
[[vk::binding(0,0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1,0)]] StructuredBuffer<float4> rays;
[[vk::binding(2,0)]] RWStructuredBuffer<float4> hits;
[[vk::binding(3,0)]] StructuredBuffer<float4> opacity;
[[vk::binding(4,0)]] ConstantBuffer<uint4> parameters;

static const uint RAY_OPACITY_INSTANCE_WORD_COUNT = 3;
uint GetPrimitiveIndex(uint instance, uint geometry, uint primitive)
{
    if (geometry != 0)
    {
        uint uvOffset = uint(opacity[instance * RAY_OPACITY_INSTANCE_WORD_COUNT].x);
        primitive += asuint(opacity[uvOffset - 1].x);
    }
    return primitive;
}

float alphaTexel(uint scalar)
{
    return opacity[scalar >> 2][scalar & 3];
}
int address(int coordinate, int extent, bool repeat)
{
    if (!repeat) return clamp(coordinate,0,extent-1);
    int remainder = coordinate % extent;
    return remainder < 0 ? remainder+extent : remainder;
}
float alphaAt(int2 coordinate, uint offset, int2 size, uint flags)
{
    int2 p = int2(address(coordinate.x,size.x,(flags & 1) != 0),
                 address(coordinate.y,size.y,(flags & 2) != 0));
    return alphaTexel(offset+uint(p.y*size.x+p.x));
}
bool accepted(uint instance, uint primitive, float2 barycentrics)
{
    float4 header = opacity[instance*3];
    float4 settings = opacity[instance*3+2];
    if (settings.z == 0) return true;
    float alpha = 1;
    int2 size = int2(header.zw);
    if (size.x > 0 && size.y > 0)
    {
        uint uvOffset = uint(header.x)+primitive*2;
        float4 ab = opacity[uvOffset];
        float2 c = opacity[uvOffset+1].xy;
        float2 uv = ab.xy*(1-barycentrics.x-barycentrics.y)+ab.zw*barycentrics.x+c*barycentrics.y;
        float4 transform = opacity[instance*3+1];
        uv = uv*transform.xy+transform.zw;
        uint flags = uint(settings.w);
        uv.x = (flags & 1) != 0 ? frac(uv.x) : saturate(uv.x);
        uv.y = (flags & 2) != 0 ? frac(uv.y) : saturate(uv.y);
        uint offset = uint(header.y);
        if ((flags & 4) != 0)
        {
            float2 pixel = uv*float2(size)-0.5;
            int2 p = int2(floor(pixel));
            float2 t = frac(pixel);
            alpha = lerp(lerp(alphaAt(p,offset,size,flags),alphaAt(p+int2(1,0),offset,size,flags),t.x),
                         lerp(alphaAt(p+int2(0,1),offset,size,flags),alphaAt(p+1,offset,size,flags),t.x),t.y);
        }
        else alpha = alphaAt(int2(floor(uv*float2(size))),offset,size,flags);
    }
    return alpha*settings.x >= settings.y;
}
[numthreads(64,1,1)]
void queryMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= parameters.x) return;
    float4 origin = rays[id.x*3];
    float4 direction = rays[id.x*3+1];
    uint4 options = asuint(rays[id.x*3+2]);
    RayDesc ray;
    ray.Origin = origin.xyz;
    ray.TMin = origin.w;
    ray.Direction = direction.xyz;
    ray.TMax = direction.w;
    uint flags = RAY_FLAG_NONE;
    if (options.y != 0) flags |= RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene,flags,options.x,ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            accepted(query.CandidateInstanceIndex(),
                     GetPrimitiveIndex(query.CandidateInstanceIndex(), query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex()),
                     query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    float4 attributes = float4(0);
    uint4 identity = uint4(0);
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        attributes = float4(query.CommittedRayT(),query.CommittedTriangleBarycentrics(),0);
        identity = uint4(GetPrimitiveIndex(query.CommittedInstanceIndex(), query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex()),
                         query.CommittedInstanceIndex(),1,0);
    }
    hits[id.x*2] = attributes;
    hits[id.x*2+1] = asfloat(identity);
}
