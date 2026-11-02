#include "point_search_layout.h"
#include "raster_bindings.slang"

// Parameters = {nodeCount, queryCount, neighborCount, root}. Output query-major
// {original point index, asuint(squared world distance)}. Invalid trees yield
// sentinel indices; decoding must reject them rather than substituting neighbors.
LUDUS_SHADER_BINDING(0) StructuredBuffer<uint4> Parameters;
LUDUS_SHADER_BINDING(1) StructuredBuffer<uint4> Nodes;
LUDUS_SHADER_BINDING(2) StructuredBuffer<float4> Queries;
LUDUS_SHADER_BINDING(3) RWStructuredBuffer<uint2> Results;

uint4 ReadPointSearchNodeWord(uint index)
{
    return Nodes[index];
}
#include "point_search.slang"

static const uint POINT_QUERY_GROUP_SIZE = LUDUS_POINT_SEARCH_GROUP_SIZE;
[shader("compute")][numthreads(POINT_QUERY_GROUP_SIZE, 1, 1)] void
ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    // PreparePointNeighborWorkload validates these counts against every bound
    // allocation before uploading Parameters. No native buffer-length query is needed.
    uint4 parameters = Parameters[0];
    uint query = dispatchThreadId.x;
    uint neighborCount = parameters.z;
    if (query >= parameters.y || neighborCount == 0 || neighborCount > LUDUS_MAX_POINT_NEIGHBORS)
        return;
    PointNeighbor neighbors[LUDUS_MAX_POINT_NEIGHBORS];
    float3 positionWorld = Queries[query].xyz;
    bool isValid = parameters.x > 0 && neighborCount <= parameters.x && all(isfinite(positionWorld)) &&
                   all(abs(positionWorld) <= LUDUS_MAX_POINT_SEARCH_COORDINATE);
    if (isValid == true)
        isValid = FindPointNeighbors(parameters.x, parameters.w, positionWorld, neighborCount, neighbors);
    for (uint i = 0; i < neighborCount; ++i)
        Results[query * neighborCount + i] = isValid == true
                                                 ? uint2(neighbors[i].PointIndex, asuint(neighbors[i].DistanceSquared))
                                                 : uint2(LUDUS_ABSENT_POINT_NODE, 0);
}
