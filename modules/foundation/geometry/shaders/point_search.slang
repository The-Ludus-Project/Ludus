#include "point_search_layout.h"

// The including kernel defines uint4 ReadPointSearchNodeWord(uint index).
// Keeping resource ownership in the kernel avoids target-specific pointer types.
// Balanced point-tree query, matching point_search.cpp. Keeps original point IDs;
// the caller can gather any embedding width without storing latent features here.
struct PointNeighbor
{
    uint PointIndex;
    float DistanceSquared;
};
bool IsPointNearer(PointNeighbor left, PointNeighbor right)
{
    return left.DistanceSquared < right.DistanceSquared ||
           (left.DistanceSquared == right.DistanceSquared && left.PointIndex < right.PointIndex);
}
bool FindPointNeighbors(uint nodeCount,
                        uint root,
                        float3 positionWorld,
                        uint neighborCount,
                        out PointNeighbor outNeighbors[LUDUS_MAX_POINT_NEIGHBORS])
{
    uint nodeStack[LUDUS_POINT_SEARCH_STACK_SIZE];
    float distanceStack[LUDUS_POINT_SEARCH_STACK_SIZE];
    uint stackSize = 1;
    uint found = 0;
    uint visited = 0;
    nodeStack[0] = root;
    distanceStack[0] = 0;
    while (stackSize > 0)
    {
        --stackSize;
        uint index = nodeStack[stackSize];
        float minimumDistanceSquared = distanceStack[stackSize];
        if (found == neighborCount && minimumDistanceSquared > outNeighbors[found - 1].DistanceSquared)
            continue;
        if (index >= nodeCount || visited >= nodeCount)
            return false;
        ++visited;
        uint4 point = ReadPointSearchNodeWord(index * LUDUS_POINT_SEARCH_NODE_WORDS);
        uint4 children = ReadPointSearchNodeWord(index * LUDUS_POINT_SEARCH_NODE_WORDS + 1);
        if (children.z > 2)
            return false;
        float3 nodePositionWorld = asfloat(point.xyz);
        if (point.w >= nodeCount || all(isfinite(nodePositionWorld)) == false ||
            any(abs(nodePositionWorld) > LUDUS_MAX_POINT_SEARCH_COORDINATE) == true)
            return false;
        float3 offsetWorld = positionWorld - nodePositionWorld;
        PointNeighbor neighbor = {point.w, dot(offsetWorld, offsetWorld)};
        uint insertion = found;
        if (insertion < neighborCount)
            ++found;
        else if (IsPointNearer(neighbor, outNeighbors[insertion - 1]) == true)
            --insertion;
        else
            insertion = neighborCount;
        if (insertion < neighborCount)
        {
            while (insertion > 0 && IsPointNearer(neighbor, outNeighbors[insertion - 1]) == true)
            {
                outNeighbors[insertion] = outNeighbors[insertion - 1];
                --insertion;
            }
            outNeighbors[insertion] = neighbor;
        }
        float delta = positionWorld[children.z] - nodePositionWorld[children.z];
        uint nearNode = delta < 0 ? children.x : children.y;
        uint farNode = delta < 0 ? children.y : children.x;
        if (farNode != LUDUS_ABSENT_POINT_NODE)
        {
            if (stackSize >= LUDUS_POINT_SEARCH_STACK_SIZE)
                return false;
            nodeStack[stackSize] = farNode;
            distanceStack[stackSize++] = delta * delta;
        }
        if (nearNode != LUDUS_ABSENT_POINT_NODE)
        {
            if (stackSize >= LUDUS_POINT_SEARCH_STACK_SIZE)
                return false;
            nodeStack[stackSize] = nearNode;
            distanceStack[stackSize++] = 0;
        }
    }
    return found == neighborCount;
}
