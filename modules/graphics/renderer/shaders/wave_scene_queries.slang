#pragma once
#include "elliptical_cone.slang"
#include "renderer_reference_layout.h"
#include "wave_scene_layout.h"
// SceneContext, TriangleData and the immutable GeometryHierarchy API are supplied by scene_shader.slang.
// Thanks to Steinberg and Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics", CGF45(2),
// e70322 (2026), Section4.2, https://doi.org/10.1111/cgf.70322 . Match wave_scene_queries.cpp's two walks.
static const uint WAVE_QUERY_READY = 0;
static const uint WAVE_QUERY_INVALID = 1;
static const uint WAVE_QUERY_CAPACITY = 2;
static const uint WAVE_QUERY_PROPAGATION_CAPACITY = 3;
static const uint WAVE_QUERY_ESCAPE_CAPACITY = 4;
struct WaveInteractionRegion
{
    EllipticalCone Frustum;
    uint Triangles[LUDUS_WAVE_REGION_TRIANGLES];
    uint TriangleCount;
};
uint WalkWaveRegion(SceneContext scene,
                    EllipticalCone cone,
                    bool shouldGather,
                    inout float inoutNearestWorld,
                    inout float inoutNearestFarWorld,
                    inout WaveInteractionRegion inoutRegion)
{
    uint nodeCount = GetGeometryHierarchyNodeCount(scene);
    if (nodeCount == 0)
        return WAVE_QUERY_READY;
    uint stack[LUDUS_REFERENCE_STACK_SIZE];
    uint stackCount = 1;
    stack[0] = 0;
    while (stackCount > 0)
    {
        uint nodeIndex = stack[--stackCount];
        GeometryHierarchyNode node = GetGeometryHierarchyNode(scene, nodeIndex);
        if (nodeIndex >= nodeCount || node.IsValid == false)
            return WAVE_QUERY_INVALID;
        EllipticalCone clipped = cone;
        if (shouldGather == false)
            clipped.FarWorld = inoutNearestWorld;
        bool canIntersect;
        if (TryTestEllipticalConeBounds(clipped, node.MinimumWorld, node.MaximumWorld, canIntersect) == false)
            return WAVE_QUERY_INVALID;
        if (canIntersect == false)
            continue;
        if (node.TriangleCount == 0)
        {
            if (stackCount > LUDUS_REFERENCE_STACK_SIZE - 2)
                return WAVE_QUERY_INVALID;
            stack[stackCount++] = node.Right;
            stack[stackCount++] = node.Left;
            continue;
        }
        for (uint slot = 0; slot < node.TriangleCount; ++slot)
        {
            uint triangleIndex = GetGeometryHierarchyTriangle(scene, node.FirstTriangle + slot);
            TriangleData triangle = GetTriangle(scene, triangleIndex);
            if (triangle.IsValid == false)
                return WAVE_QUERY_INVALID;
            ConeTriangle geometry;
            geometry.FirstWorld = triangle.Position0World;
            geometry.SecondWorld = triangle.Position1World;
            geometry.ThirdWorld = triangle.Position2World;
            ConeTriangleIntersection intersection = (ConeTriangleIntersection)0;
            if (TryIntersectEllipticalConeTriangle(clipped, geometry, intersection) == false)
                return WAVE_QUERY_INVALID;
            if (intersection.IsHit == false)
                continue;
            if (intersection.NearWorld < inoutNearestWorld)
            {
                inoutNearestWorld = intersection.NearWorld;
                inoutNearestFarWorld = intersection.FarWorld;
            }
            if (shouldGather == true)
            {
                if (inoutRegion.TriangleCount == LUDUS_WAVE_REGION_TRIANGLES)
                    return WAVE_QUERY_CAPACITY;
                inoutRegion.Triangles[inoutRegion.TriangleCount++] = triangleIndex;
            }
        }
    }
    return WAVE_QUERY_READY;
}
/// Ready includes misses. Failure never publishes a partial region. Sorted IDs provide deterministic parity.
[noinline] uint
FindWaveInteractionRegion(SceneContext scene, EllipticalCone cone, inout WaveInteractionRegion inoutRegion)
{
    if (IsValidEllipticalCone(cone) == false)
        return WAVE_QUERY_INVALID;
    WaveInteractionRegion candidate = (WaveInteractionRegion)0;
    candidate.Frustum = cone;
    float nearestWorld = cone.FarWorld;
    float nearestFarWorld = cone.FarWorld;
    uint status = WalkWaveRegion(scene, cone, false, nearestWorld, nearestFarWorld, candidate);
    if (status != WAVE_QUERY_READY)
        return status;
    candidate.Frustum.NearWorld = nearestWorld;
    candidate.Frustum.FarWorld = min(cone.FarWorld, nearestWorld + 2 * (cone.RadiusWorld + cone.Slope * nearestWorld));
    if (cone.RadiusWorld + cone.Slope * nearestWorld == 0)
        candidate.Frustum.FarWorld = nearestFarWorld;
    status = WalkWaveRegion(scene, candidate.Frustum, true, nearestWorld, nearestFarWorld, candidate);
    if (status != WAVE_QUERY_READY)
        return status;
    // Insertion sort is bounded by the region, never by total scene size.
    for (uint index = 1; index < candidate.TriangleCount; ++index)
    {
        uint triangleIndex = candidate.Triangles[index];
        uint position = index;
        while (position > 0 && candidate.Triangles[position - 1] > triangleIndex)
        {
            candidate.Triangles[position] = candidate.Triangles[position - 1];
            --position;
        }
        candidate.Triangles[position] = triangleIndex;
    }
    inoutRegion = candidate;
    return WAVE_QUERY_READY;
}
