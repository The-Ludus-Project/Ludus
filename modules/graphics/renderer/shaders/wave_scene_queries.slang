#pragma once
#include "elliptical_cone.slang"
#include "renderer_reference_layout.h"
#include "wave_scene_layout.h"
// SceneContext, TriangleData and the immutable GeometryHierarchy API are supplied by scene_shader.slang.
// Thanks to Steinberg and Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics", CGF45(2),
// e70322 (2026), Section4.2, https://doi.org/10.1111/cgf.70322 . Match wave_scene_queries.cpp's two walks.
/// Outcome of a wave scene query or connection. The values are LUDUS_WAVE_QUERY_* in wave_scene_layout.h, which
/// host parity tests read back.
enum WaveQuery : uint
{
    Ready = LUDUS_WAVE_QUERY_READY,                              ///< Finished; a region query may report a miss.
    Invalid = LUDUS_WAVE_QUERY_INVALID,                          ///< Invalid geometry, cone or hierarchy input.
    Capacity = LUDUS_WAVE_QUERY_CAPACITY,                        ///< A region exceeded LUDUS_WAVE_REGION_TRIANGLES.
    PropagationCapacity = LUDUS_WAVE_QUERY_PROPAGATION_CAPACITY, ///< Too many successive interaction regions.
    EscapeCapacity = LUDUS_WAVE_QUERY_ESCAPE_CAPACITY,           ///< Origin escape needed too many attempts.
};
struct WaveInteractionRegion
{
    EllipticalCone Frustum;
    uint Triangles[LUDUS_WAVE_REGION_TRIANGLES];
    uint TriangleCount;
};
WaveQuery WalkWaveRegion(SceneContext scene,
                         EllipticalCone cone,
                         bool shouldGather,
                         inout float inoutNearestWorld,
                         inout float inoutNearestFarWorld,
                         inout WaveInteractionRegion inoutRegion)
{
    uint nodeCount = GetGeometryHierarchyNodeCount(scene);
    if (nodeCount == 0)
        return WaveQuery::Ready;
    uint stack[LUDUS_REFERENCE_STACK_SIZE];
    uint stackCount = 1;
    stack[0] = 0;
    while (stackCount > 0)
    {
        uint nodeIndex = stack[--stackCount];
        GeometryHierarchyNode node = GetGeometryHierarchyNode(scene, nodeIndex);
        if (nodeIndex >= nodeCount || node.IsValid == false)
            return WaveQuery::Invalid;
        EllipticalCone clipped = cone;
        if (shouldGather == false)
            clipped.FarWorld = inoutNearestWorld;
        bool canIntersect;
        if (TryTestEllipticalConeBounds(clipped, node.MinimumWorld, node.MaximumWorld, canIntersect) == false)
            return WaveQuery::Invalid;
        if (canIntersect == false)
            continue;
        if (node.TriangleCount == 0)
        {
            if (stackCount > LUDUS_REFERENCE_STACK_SIZE - 2)
                return WaveQuery::Invalid;
            stack[stackCount++] = node.Right;
            stack[stackCount++] = node.Left;
            continue;
        }
        for (uint slot = 0; slot < node.TriangleCount; ++slot)
        {
            uint triangleIndex = GetGeometryHierarchyTriangle(scene, node.FirstTriangle + slot);
            TriangleData triangle = GetTriangle(scene, triangleIndex);
            if (triangle.IsValid == false)
                return WaveQuery::Invalid;
            ConeTriangle geometry;
            geometry.FirstWorld = triangle.Position0World;
            geometry.SecondWorld = triangle.Position1World;
            geometry.ThirdWorld = triangle.Position2World;
            ConeTriangleIntersection intersection = (ConeTriangleIntersection)0;
            if (TryIntersectEllipticalConeTriangle(clipped, geometry, intersection) == false)
                return WaveQuery::Invalid;
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
                    return WaveQuery::Capacity;
                inoutRegion.Triangles[inoutRegion.TriangleCount++] = triangleIndex;
            }
        }
    }
    return WaveQuery::Ready;
}
/// Ready includes misses. Failure never publishes a partial region. Sorted IDs provide deterministic parity.
[noinline] WaveQuery
FindWaveInteractionRegion(SceneContext scene, EllipticalCone cone, inout WaveInteractionRegion inoutRegion)
{
    if (IsValidEllipticalCone(cone) == false)
        return WaveQuery::Invalid;
    WaveInteractionRegion candidate = (WaveInteractionRegion)0;
    candidate.Frustum = cone;
    float nearestWorld = cone.FarWorld;
    float nearestFarWorld = cone.FarWorld;
    WaveQuery status = WalkWaveRegion(scene, cone, false, nearestWorld, nearestFarWorld, candidate);
    if (status != WaveQuery::Ready)
        return status;
    candidate.Frustum.NearWorld = nearestWorld;
    candidate.Frustum.FarWorld = min(cone.FarWorld, nearestWorld + 2 * (cone.RadiusWorld + cone.Slope * nearestWorld));
    if (cone.RadiusWorld + cone.Slope * nearestWorld == 0)
        candidate.Frustum.FarWorld = nearestFarWorld;
    status = WalkWaveRegion(scene, candidate.Frustum, true, nearestWorld, nearestFarWorld, candidate);
    if (status != WaveQuery::Ready)
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
    return WaveQuery::Ready;
}
