// Test-only adapter supplies exactly the immutable geometry API used by the production wave scene query.
// The CPU uploads its BVH and triangle records; all region traversal/intersection executes in the GPU kernel.
struct SceneContext
{
    uint Reserved;
};
struct GeometryHierarchyNode
{
    float3 MinimumWorld;
    float3 MaximumWorld;
    uint Left;
    uint Right;
    uint FirstTriangle;
    uint TriangleCount;
    bool IsValid;
};
struct TriangleData
{
    float3 Position0World;
    float3 Position1World;
    float3 Position2World;
    bool IsValid;
};
uint GetGeometryHierarchyNodeCount(SceneContext scene)
{
    return Cases[0].y;
}
GeometryHierarchyNode GetGeometryHierarchyNode(SceneContext scene, uint index)
{
    GeometryHierarchyNode node = (GeometryHierarchyNode)0;
    node.IsValid = index < Cases[0].y;
    if (node.IsValid == true)
    {
        uint base = Cases[0].x + index * 3;
        node.MinimumWorld = asfloat(Cases[base]).xyz;
        node.MaximumWorld = asfloat(Cases[base + 1]).xyz;
        node.Left = Cases[base].w;
        node.Right = Cases[base + 1].w;
        node.FirstTriangle = Cases[base + 2].x;
        node.TriangleCount = Cases[base + 2].y;
    }
    return node;
}
uint GetGeometryHierarchyTriangle(SceneContext scene, uint slot)
{
    return slot < Cases[1].y ? Cases[Cases[1].x + slot / 4][slot % 4] : 0xffffffff;
}
TriangleData GetTriangle(SceneContext scene, uint index)
{
    TriangleData triangle = (TriangleData)0;
    triangle.IsValid = index < Cases[1].y;
    if (triangle.IsValid == true)
    {
        uint base = Cases[0].z + index * 3;
        triangle.Position0World = asfloat(Cases[base]).xyz;
        triangle.Position1World = asfloat(Cases[base + 1]).xyz;
        triangle.Position2World = asfloat(Cases[base + 2]).xyz;
    }
    return triangle;
}
#include "wave_scene_queries.slang"
bool IsWaveTriangleCovered(SceneContext scene, uint triangleIndex, float2 barycentric)
{
    return true;
}
#include "wave_connection.slang"
void EvaluateWaveSceneQueryFixture(uint index)
{
    uint base = LUDUS_WAVE_PARITY_REGION_CONE_FIRST + index * LUDUS_WAVE_PARITY_REGION_CONE_RECORDS;
    EllipticalCone cone;
    cone.OriginWorld = asfloat(Cases[base]).xyz;
    cone.RadiusWorld = asfloat(Cases[base]).w;
    cone.DirectionWorld = asfloat(Cases[base + 1]).xyz;
    cone.Slope = asfloat(Cases[base + 1]).w;
    cone.AxisXWorld = asfloat(Cases[base + 2]).xyz;
    float3 limits = asfloat(Cases[base + 3]).xyz;
    cone.MinorToMajor = limits.x;
    cone.NearWorld = limits.y;
    cone.FarWorld = limits.z;
    if (Cases[0].w == LUDUS_WAVE_PARITY_CONNECTION_TAG)
    {
        float2 field = float2(0);
        WaveQuery status = EvaluateWaveConnection((SceneContext)0,
                                                  cone.OriginWorld,
                                                  cone.DirectionWorld,
                                                  cone.FarWorld,
                                                  asfloat(Cases[base + 3]).w,
                                                  field);
        uint destination = index * LUDUS_WAVE_PARITY_RESULT_RECORDS;
        Results[destination] = uint4(asuint(field), asuint(dot(field, field)), uint(status));
        Results[destination + 1] = uint4(0);
        return;
    }
    WaveInteractionRegion region = (WaveInteractionRegion)0;
    WaveQuery status = FindWaveInteractionRegion((SceneContext)0, cone, region);
    uint hash = 0;
    for (uint slot = 0; slot < region.TriangleCount; ++slot)
        hash = hash * LUDUS_WAVE_PARITY_REGION_HASH_FACTOR + region.Triangles[slot];
    uint destination = index * LUDUS_WAVE_PARITY_RESULT_RECORDS;
    Results[destination] =
        uint4(asuint(region.Frustum.NearWorld), asuint(region.Frustum.FarWorld), region.TriangleCount, uint(status));
    Results[destination + 1] = uint4(hash, 0, 0, 0);
}
