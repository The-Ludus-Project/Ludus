#pragma once
// Reference-compute adapter for the same immutable scene API used by authored optical graphs.
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
    return Counts.y;
}
GeometryHierarchyNode GetGeometryHierarchyNode(SceneContext scene, uint index)
{
    GeometryHierarchyNode result = (GeometryHierarchyNode)0;
    result.IsValid = index < Counts.y;
    if (result.IsValid == true)
    {
        BvhNode node = LoadBvhNode(index);
        result.MinimumWorld = node.MinimumWorld;
        result.MaximumWorld = node.MaximumWorld;
        result.Left = node.Left;
        result.Right = node.Right;
        result.FirstTriangle = node.First;
        result.TriangleCount = node.Count;
    }
    return result;
}
uint GetGeometryHierarchyTriangle(SceneContext scene, uint slot)
{
    return slot < Counts.x ? GetBvhLeafTriangle(slot) : NO_TRIANGLE;
}
TriangleData GetTriangle(SceneContext scene, uint index)
{
    TriangleData result = (TriangleData)0;
    result.IsValid = index < Counts.x;
    if (result.IsValid == true)
    {
        uint base = index * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
        result.Position0World = SceneValue(base).xyz;
        result.Position1World = SceneValue(base + 1).xyz;
        result.Position2World = SceneValue(base + 2).xyz;
    }
    return result;
}
bool IsWaveTriangleCovered(SceneContext scene, uint triangleIndex, float2 barycentric)
{
    return IsBvhCandidateAccepted(triangleIndex, barycentric);
}
#include "wave_connection.slang"
