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
// clipped; and traversal runs on the GPU, over the renderer's packed BVH or, for comparison, as a
// linear scan of the packed triangles (design D5).
// Thanks to Tomas Akenine-Moller, "Fast 3D Triangle-Box Overlap Testing", Journal of Graphics Tools
// 6(1), 2001, pp. 29-33, https://doi.org/10.1080/10867651.2001.10487535 , for the separating-axis
// node test against the shadow triangle: the box axes, the triangle normal and the nine edge cross
// products. Departure: the box grows by a rounding margin so that the test stays conservative for
// the float clip below (D5).
#include "ray_bvh_nodes.slang"
#include "ray_triangle_loop.slang"

/// Rounding allowance of the BVH node test in units of 2^-24 of the coordinate scale, before the
/// 1 / sine growth of the clip's shadow-plane coordinates (design D5).
static const float LINE_BVH_MARGIN_ULPS = 64.0;
static const float LINE_BVH_UNIT_ROUNDOFF = 5.96046448e-8; // 2^-24

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

/// True when the projections of the three vertices, relative to the box center, on axis lie wholly
/// outside the box's projected radius.
bool IsLineShadowSeparated(float3 axis, float3 vertex0, float3 vertex1, float3 vertex2, float3 halfExtentWorld)
{
    float projection0 = dot(axis, vertex0);
    float projection1 = dot(axis, vertex1);
    float projection2 = dot(axis, vertex2);
    float radius = dot(abs(axis), halfExtentWorld);
    return min(projection0, min(projection1, projection2)) > radius ||
           max(projection0, max(projection1, projection2)) < -radius;
}

/// True when a cross product of a box axis with edgeWorld separates the triangle from the box.
bool IsLineShadowSeparatedByEdge(float3 edgeWorld,
                                 float3 vertex0,
                                 float3 vertex1,
                                 float3 vertex2,
                                 float3 halfExtentWorld)
{
    for (uint axis = 0; axis < 3; ++axis)
    {
        float3 unit = float3(0, 0, 0);
        unit[axis] = 1;
        if (IsLineShadowSeparated(cross(unit, edgeWorld), vertex0, vertex1, vertex2, halfExtentWorld) == true)
            return true;
    }
    return false;
}

/// Conservative overlap of the shadow triangle (apex, start, end) with a node box, by the
/// Akenine-Moller separating-axis test. The half extents grow by marginScale times the largest
/// coordinate magnitude of the query (queryScaleWorld) and the box, so float rounding in this test
/// and in ClipLineOccluder never culls a box whose triangles the linear scan would clip.
bool OverlapsLineShadowBox(float3 apexWorld,
                           float3 startWorld,
                           float3 endWorld,
                           float3 planeNormalWorld,
                           float queryScaleWorld,
                           float marginScale,
                           float3 minimumWorld,
                           float3 maximumWorld)
{
    if (any(minimumWorld > maximumWorld))
        return false;
    float3 centerWorld = (minimumWorld + maximumWorld) * 0.5;
    float3 boxScaleWorld = max(abs(minimumWorld), abs(maximumWorld));
    float scaleWorld = max(queryScaleWorld, max(boxScaleWorld.x, max(boxScaleWorld.y, boxScaleWorld.z)));
    float3 halfExtentWorld = (maximumWorld - minimumWorld) * 0.5 + marginScale * scaleWorld;
    float3 vertex0 = apexWorld - centerWorld;
    float3 vertex1 = startWorld - centerWorld;
    float3 vertex2 = endWorld - centerWorld;
    // The three box axes: the triangle's bounds against the box.
    if (any(min(vertex0, min(vertex1, vertex2)) > halfExtentWorld) ||
        any(max(vertex0, max(vertex1, vertex2)) < -halfExtentWorld))
        return false;
    if (IsLineShadowSeparated(planeNormalWorld, vertex0, vertex1, vertex2, halfExtentWorld) == true)
        return false;
    return IsLineShadowSeparatedByEdge(vertex1 - vertex0, vertex0, vertex1, vertex2, halfExtentWorld) == false &&
           IsLineShadowSeparatedByEdge(vertex2 - vertex1, vertex0, vertex1, vertex2, halfExtentWorld) == false &&
           IsLineShadowSeparatedByEdge(vertex0 - vertex2, vertex0, vertex1, vertex2, halfExtentWorld) == false;
}

/// Candidate outcomes of one occluder triangle.
static const uint LINE_CANDIDATE_NONE = 0;
static const uint LINE_CANDIDATE_OCCLUDER = 1;
static const uint LINE_CANDIDATE_MASKED = 2;
static const uint LINE_CANDIDATE_FAILED = 3;

/// Clip packed triangle `triangle` against the shadow triangle. An occluder returns its interval; an
/// alpha-tested triangle that would occlude is reported as masked instead (PL05); a nonfinite interval
/// fails. Both traversals call this, so they compute bitwise-identical intervals per triangle.
uint ClipLineCandidate(uint triangle,
                       float3 apexWorld,
                       float3 edgeAWorld,
                       float3 edgeBWorld,
                       float3 planeNormalWorld,
                       bool isOpaqueScene,
                       out float2 outInterval)
{
    uint firstWord = triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
    if (ClipLineOccluder(apexWorld,
                         edgeAWorld,
                         edgeBWorld,
                         planeNormalWorld,
                         SceneValue(firstWord).xyz,
                         SceneValue(firstWord + 1).xyz,
                         SceneValue(firstWord + 2).xyz,
                         outInterval) == false)
        return LINE_CANDIDATE_NONE;
    if (all(isfinite(outInterval)) == false)
        return LINE_CANDIDATE_FAILED;
    // An opaque scene never reports masked occluders and never reads the alpha options (PL05).
    if (isOpaqueScene == false && asuint(Surface(triangle, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) != 0)
        return LINE_CANDIDATE_MASKED;
    return LINE_CANDIDATE_OCCLUDER;
}

/// True when the running occluded set leaves no visible interval longer than the merge tolerance.
/// Later insertions only merge into it, so the traversal can stop (design D5).
bool IsLineFullyOccluded(float2 firstOccluded, uint occludedCount)
{
    return occludedCount == 1 && firstOccluded.x <= SCENE_LINE_INTERVAL_MERGE_TOLERANCE &&
           1 - firstOccluded.y <= SCENE_LINE_INTERVAL_MERGE_TOLERANCE;
}

/// Find the visible complement of the opaque occluders (design D2-D5). By default the traversal walks
/// the packed BVH, visiting nodes whose boxes overlap the shadow triangle; with
/// LUDUS_SCENE_LINE_TRIANGLE_LOOP (scene_interface.line_traversal "TriangleLoop") it scans every packed
/// triangle in index order. Both clip each candidate identically and stop once the segment is fully
/// occluded. The caller has validated the inputs and offset the apex and endpoints (PL03). The
/// receiver and carrying triangles may be SCENE_INVALID_INDEX. Returns false on capacity overflow, a
/// nonfinite interval, an exhausted BVH stack or a pack with triangles but no nodes. Otherwise
/// outVisibility holds the visible complement with VisibleLengthWorld zero (the caller scales it),
/// HasMaskedOccluders only when some length is visible, and outIsOccluded says whether any opaque
/// occluder removed length.
/// The occluded set is kept in outVisibility.Intervals itself and complemented in place, so the
/// query holds one interval array, not two: with two, Metal lost the interval stores of large
/// merged kernels at capacity 32 (Apple M5, macOS 26.6, PL1).
bool TraceLineVisibility(float3 apexWorld,
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
    bool isFullyOccluded = false;
    bool isOpaqueScene = (Sampling.z & LUDUS_GPU_PREVIEW_OPAQUE_FLAG) != 0;
#ifndef LUDUS_SCENE_LINE_TRIANGLE_LOOP
    if (Counts.y == 0 && Counts.x != 0)
        return false;
    // The clip's shadow-plane coordinates lose precision as 1 / sine of the apex angle, which the
    // caller bounds below by TRIANGLE_PARALLEL_COSINE_TOLERANCE.
    float sine = length(planeNormalWorld) / (length(edgeAWorld) * length(edgeBWorld));
    float marginScale = LINE_BVH_MARGIN_ULPS * LINE_BVH_UNIT_ROUNDOFF / sine;
    float3 queryScaleWorld = max(abs(apexWorld), max(abs(startWorld), abs(endWorld)));
    float queryScale = max(queryScaleWorld.x, max(queryScaleWorld.y, queryScaleWorld.z));
    uint stack[LUDUS_REFERENCE_STACK_SIZE];
    stack[0] = 0;
    uint pending = Counts.y == 0 ? 0 : 1;
#else
    // One pass over a single range of every triangle in index order.
    uint pending = 1;
#endif
    while (pending != 0 && isFullyOccluded == false)
    {
        --pending;
#ifndef LUDUS_SCENE_LINE_TRIANGLE_LOOP
        BvhNode node = LoadBvhNode(stack[pending]);
        if (OverlapsLineShadowBox(apexWorld,
                                  startWorld,
                                  endWorld,
                                  planeNormalWorld,
                                  queryScale,
                                  marginScale,
                                  node.MinimumWorld,
                                  node.MaximumWorld) == false)
            continue;
        if (node.Count == 0)
        {
            if (pending > BVH_MAXIMUM_PENDING)
                return false;
            stack[pending] = node.Right;
            stack[pending + 1] = node.Left;
            pending += 2;
            continue;
        }
        uint firstSlot = node.First;
        uint endSlot = node.First + node.Count;
#else
        uint firstSlot = 0;
        uint endSlot = Counts.x;
#endif
        for (uint slot = firstSlot; slot < endSlot && isFullyOccluded == false; ++slot)
        {
#ifndef LUDUS_SCENE_LINE_TRIANGLE_LOOP
            uint triangle = GetBvhLeafTriangle(slot);
#else
            uint triangle = slot;
#endif
            if (triangle == receiverTriangle || triangle == segmentTriangle)
                continue;
            float2 interval;
            uint candidate = ClipLineCandidate(triangle,
                                               apexWorld,
                                               edgeAWorld,
                                               edgeBWorld,
                                               planeNormalWorld,
                                               isOpaqueScene,
                                               interval);
            if (candidate == LINE_CANDIDATE_FAILED)
                return false;
            if (candidate == LINE_CANDIDATE_MASKED)
                hasMaskedOccluders = true;
            if (candidate != LINE_CANDIDATE_OCCLUDER)
                continue;
            if (InsertLineOccludedInterval(outVisibility.Intervals, occludedCount, interval) == false)
                return false;
            isFullyOccluded = IsLineFullyOccluded(outVisibility.Intervals[0], occludedCount);
        }
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
    // With nothing visible no line point is drawn, so no masked confirmation is needed (PL05); this
    // also keeps the flag independent of where the traversal stopped.
    outVisibility.HasMaskedOccluders = hasMaskedOccluders == true && visibleCount > 0;
    outIsOccluded = occludedCount > 0;
    return true;
}
