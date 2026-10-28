#pragma once
// Private line visibility traversal behind FindVisibleLineIntervals (scene_shader.slang), which owns
// the public types, budgets, input validation and failure latching.
//
// Thanks to Niels Billen and Philip Dutre, "Line Sampling for Direct Illumination", Computer Graphics
// Forum 35(4) (EGSR 2016), pp. 93-102, https://doi.org/10/f84z2h , section 4: the visible parts of a
// line are found by intersecting the shadow triangle spanned by the receiver and the line with every
// occluder, then taking the complement of the occluded intervals. Restated here for the packed World
// triangles, in the shadow triangle's (alpha, beta) coordinates so that one clip region serves every
// occluder (point-line design D2).
// Thanks to Katherine Salesin and Wojciech Jarosz, "Combining Point and Line Samples for Direct
// Illumination", Computer Graphics Forum 38(4) (EGSR 2019), pp. 159-169,
// https://doi.org/10.1111/cgf.13779 : their line strategies consume these visible intervals.
// Departures (design section 2 and D2-D5): intervals are bounded by SCENE_LINE_INTERVAL_CAPACITY and
// overflow fails instead of approximating; occluded intervals closer than
// SCENE_LINE_INTERVAL_MERGE_TOLERANCE merge; each occluder edge is intersected from its
// lexicographically smaller endpoint, so triangles sharing an edge compute a bitwise-identical point
// and leave no crack; alpha-tested triangles are reported through HasMaskedOccluders instead of
// clipped; and traversal is a linear scan of the packed triangles on the GPU, not a CPU BVH.
#include "ray_triangle_loop.slang"

/// True when a precedes b in (x, y, z) lexicographic order.
bool IsLineVertexLess(float3 a, float3 b)
{
    if (a.x != b.x)
        return a.x < b.x;
    if (a.y != b.y)
        return a.y < b.y;
    return a.z < b.z;
}

/// Point where the edge between two vertices on opposite sides of the shadow plane crosses it. The
/// edge is always parameterized from its lexicographically smaller vertex (design D2 step 2).
float3 IntersectLineShadowEdge(float3 aWorld, float aDistance, float3 bWorld, float bDistance)
{
    if (IsLineVertexLess(bWorld, aWorld) == true)
        return bWorld + (aWorld - bWorld) * (bDistance / (bDistance - aDistance));
    return aWorld + (bWorld - aWorld) * (aDistance / (aDistance - bDistance));
}

/// Shadow-triangle coordinates (alpha, beta) of a point in the shadow plane, relative to the apex.
float2 GetLineShadowCoordinates(float3 offsetWorld, float3 edgeAWorld, float3 edgeBWorld, float3 planeNormalWorld)
{
    float normalLengthSquared = dot(planeNormalWorld, planeNormalWorld);
    return float2(dot(cross(offsetWorld, edgeBWorld), planeNormalWorld),
                  dot(cross(edgeAWorld, offsetWorld), planeNormalWorld)) /
           normalLengthSquared;
}

/// Clip one occluder triangle against the shadow triangle (apex, apex + edgeA, apex + edgeB).
/// Returns false when it does not occlude: it misses the shadow plane, is coplanar within the
/// triangle loop's parallel tolerance, or lies behind the apex or beyond the segment. Otherwise
/// outInterval is its occluded segment parameter range [start, end] in [0, 1].
bool ClipLineOccluder(float3 apexWorld,
                      float3 edgeAWorld,
                      float3 edgeBWorld,
                      float3 planeNormalWorld,
                      float3 position0World,
                      float3 position1World,
                      float3 position2World,
                      out float2 outInterval)
{
    outInterval = float2(0, 0);
    float distance0 = dot(planeNormalWorld, position0World - apexWorld);
    float distance1 = dot(planeNormalWorld, position1World - apexWorld);
    float distance2 = dot(planeNormalWorld, position2World - apexWorld);
    if ((distance0 > 0 && distance1 > 0 && distance2 > 0) || (distance0 < 0 && distance1 < 0 && distance2 < 0))
        return false;
    float edgeLengthWorld = max(length(position1World - position0World),
                                max(length(position2World - position1World), length(position0World - position2World)));
    float coplanarDistance = TRIANGLE_PARALLEL_COSINE_TOLERANCE * length(planeNormalWorld) * edgeLengthWorld;
    if (abs(distance0) <= coplanarDistance && abs(distance1) <= coplanarDistance && abs(distance2) <= coplanarDistance)
        return false;
    // In exact arithmetic a crossing triangle meets the plane in at most two points.
    float3 pointsWorld[2];
    uint count = 0;
    if (distance0 == 0)
        pointsWorld[count++] = position0World;
    if (distance1 == 0 && count < 2)
        pointsWorld[count++] = position1World;
    if (distance2 == 0 && count < 2)
        pointsWorld[count++] = position2World;
    if (distance0 * distance1 < 0 && count < 2)
        pointsWorld[count++] = IntersectLineShadowEdge(position0World, distance0, position1World, distance1);
    if (distance1 * distance2 < 0 && count < 2)
        pointsWorld[count++] = IntersectLineShadowEdge(position1World, distance1, position2World, distance2);
    if (distance2 * distance0 < 0 && count < 2)
        pointsWorld[count++] = IntersectLineShadowEdge(position2World, distance2, position0World, distance0);
    if (count < 2)
        return false;
    float2 coordinates0 =
        GetLineShadowCoordinates(pointsWorld[0] - apexWorld, edgeAWorld, edgeBWorld, planeNormalWorld);
    float2 coordinates1 =
        GetLineShadowCoordinates(pointsWorld[1] - apexWorld, edgeAWorld, edgeBWorld, planeNormalWorld);
    // Half-planes alpha >= 0, beta >= 0, s >= 0 and 1 - s >= 0, with s = alpha + beta. The PL03
    // offsets already move the apex and the endpoints off the receiver and light planes, so the
    // near and far clip distances are zero; the receiver and carrying triangles are excluded by index.
    float4 bounds0 = float4(coordinates0, coordinates0.x + coordinates0.y, 1 - coordinates0.x - coordinates0.y);
    float4 bounds1 = float4(coordinates1, coordinates1.x + coordinates1.y, 1 - coordinates1.x - coordinates1.y);
    float minimumFraction = 0;
    float maximumFraction = 1;
    for (uint k = 0; k < 4; ++k)
    {
        if (bounds0[k] < 0 && bounds1[k] < 0)
            return false;
        if (bounds0[k] < 0)
            minimumFraction = max(minimumFraction, bounds0[k] / (bounds0[k] - bounds1[k]));
        else if (bounds1[k] < 0)
            maximumFraction = min(maximumFraction, bounds0[k] / (bounds0[k] - bounds1[k]));
    }
    if (minimumFraction >= maximumFraction)
        return false;
    float2 nearCoordinates = lerp(coordinates0, coordinates1, minimumFraction);
    float2 farCoordinates = lerp(coordinates0, coordinates1, maximumFraction);
    float nearFraction = nearCoordinates.x + nearCoordinates.y;
    float farFraction = farCoordinates.x + farCoordinates.y;
    // s = 0 only at the apex itself, where the ray to the segment is undefined.
    if (nearFraction <= 0 || farFraction <= 0)
        return false;
    // t = beta / s is projective along the clipped segment, with s > 0, so it is monotonic.
    float nearParameter = saturate(nearCoordinates.y / nearFraction);
    float farParameter = saturate(farCoordinates.y / farFraction);
    outInterval = float2(min(nearParameter, farParameter), max(nearParameter, farParameter));
    return outInterval.y > outInterval.x;
}

/// Insert one occluded interval into the sorted, disjoint set, merging every interval within the
/// tolerance of it. Returns false when the set would exceed SCENE_LINE_INTERVAL_CAPACITY - 1 entries,
/// so that its visible complement always fits in SCENE_LINE_INTERVAL_CAPACITY (design D3).
bool InsertLineOccludedInterval(inout float2 inoutOccluded[SCENE_LINE_INTERVAL_CAPACITY],
                                inout uint inoutCount,
                                float2 interval)
{
    uint first = 0;
    while (first < inoutCount && inoutOccluded[first].y + SCENE_LINE_INTERVAL_MERGE_TOLERANCE < interval.x)
        ++first;
    uint last = first;
    while (last < inoutCount && inoutOccluded[last].x - SCENE_LINE_INTERVAL_MERGE_TOLERANCE <= interval.y)
        ++last;
    if (last == first)
    {
        if (inoutCount + 1 > SCENE_LINE_INTERVAL_CAPACITY - 1)
            return false;
        for (uint i = inoutCount; i > first; --i)
            inoutOccluded[i] = inoutOccluded[i - 1];
        inoutOccluded[first] = interval;
        inoutCount += 1;
        return true;
    }
    // Entries [first, last) overlap the new interval or lie within the tolerance of it.
    inoutOccluded[first] = float2(min(interval.x, inoutOccluded[first].x), max(interval.y, inoutOccluded[last - 1].y));
    uint removed = last - first - 1;
    if (removed > 0)
    {
        for (uint i = last; i < inoutCount; ++i)
            inoutOccluded[i - removed] = inoutOccluded[i];
        inoutCount -= removed;
    }
    return true;
}

/// Linear scan of every packed World triangle (design D5, PL1). The caller has validated the inputs
/// and offset the apex and endpoints (PL03). The receiver and carrying triangles may be
/// SCENE_INVALID_INDEX. Returns false on capacity overflow or a nonfinite interval. Otherwise
/// outVisibility holds the visible complement with VisibleLengthWorld zero (the caller scales it)
/// and outIsOccluded says whether any opaque occluder removed length.
/// The occluded set is kept in outVisibility.Intervals itself and complemented in place, so the
/// query holds one interval array, not two: with two, Metal lost the interval stores of large
/// merged kernels at capacity 32 (Apple M5, macOS 26.6, PL1).
bool TraceLineTriangleLoop(float3 apexWorld,
                           float3 startWorld,
                           float3 endWorld,
                           uint receiverTriangle,
                           uint segmentTriangle,
                           out LineVisibility outVisibility,
                           out bool outIsOccluded)
{
    ClearLineVisibility(outVisibility);
    outIsOccluded = false;
    float3 edgeAWorld = startWorld - apexWorld;
    float3 edgeBWorld = endWorld - apexWorld;
    float3 planeNormalWorld = cross(edgeAWorld, edgeBWorld);
    uint occludedCount = 0;
    bool hasMaskedOccluders = false;
    bool isOpaqueScene = (Sampling.z & LUDUS_GPU_PREVIEW_OPAQUE_FLAG) != 0;
    for (uint triangle = 0; triangle < Counts.x; ++triangle)
    {
        if (triangle == receiverTriangle || triangle == segmentTriangle)
            continue;
        uint firstWord = triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
        float2 interval;
        if (ClipLineOccluder(apexWorld,
                             edgeAWorld,
                             edgeBWorld,
                             planeNormalWorld,
                             SceneValue(firstWord).xyz,
                             SceneValue(firstWord + 1).xyz,
                             SceneValue(firstWord + 2).xyz,
                             interval) == false)
            continue;
        if (all(isfinite(interval)) == false)
            return false;
        // An opaque scene never reports masked occluders and never reads the alpha options (PL05).
        if (isOpaqueScene == false && asuint(Surface(triangle, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) != 0)
        {
            hasMaskedOccluders = true;
            continue;
        }
        if (InsertLineOccludedInterval(outVisibility.Intervals, occludedCount, interval) == false)
            return false;
    }
    // Gap k lies between occluded intervals k - 1 and k. Going down, each gap reads only entries that
    // it has not yet overwritten; there are occludedCount + 1 <= SCENE_LINE_INTERVAL_CAPACITY gaps.
    for (uint gap = occludedCount + 1; gap > 0; --gap)
    {
        uint k = gap - 1;
        float start = k == 0 ? 0 : outVisibility.Intervals[k - 1].y;
        float end = k == occludedCount ? 1 : outVisibility.Intervals[k].x;
        outVisibility.Intervals[k] = float2(start, end);
    }
    // Keep the gaps longer than the tolerance, in order; later entries are zero.
    uint visibleCount = 0;
    for (uint k = 0; k <= occludedCount; ++k)
    {
        float2 gap = outVisibility.Intervals[k];
        outVisibility.Intervals[k] = float2(0, 0);
        if (gap.y - gap.x > SCENE_LINE_INTERVAL_MERGE_TOLERANCE)
        {
            outVisibility.Intervals[visibleCount] = gap;
            visibleCount += 1;
        }
    }
    outVisibility.IntervalCount = visibleCount;
    outVisibility.HasMaskedOccluders = hasMaskedOccluders;
    outIsOccluded = occludedCount > 0;
    return true;
}
