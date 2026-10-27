#include "projected_solid_angle.slang"
#include "projected_solid_angle_parity_layout.h"
#include "raster_bindings.slang"

// Test-only kernel for projected_solid_angle_parity_tests.cpp: runs FoundationMath's shader port of
// projected solid angle sampling on host-supplied polygons so the host can compare it with the C++
// implementation. projected_solid_angle_parity_layout.h owns the case ABI.

LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> Cases;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<float4> Results;

[shader("compute")][numthreads(LUDUS_PROJECTED_SOLID_ANGLE_PARITY_GROUP_SIZE, 1, 1)] void
ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    uint caseBase = index * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_CASE;
    ProjectedPolygonInput input;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        input.PositionsTangent[i] = Cases[caseBase + i].xyz;
    }
    float4 parameters = Cases[caseBase + MAX_PROJECTED_POLYGON_INPUT_VERTICES];
    input.VertexCount = uint(parameters.x);
    ProjectedSolidAnglePolygon polygon;
    bool isPrepared = TryPrepareProjectedSolidAnglePolygon(input, polygon);
    float3 direction;
    bool isSampled = TrySampleProjectedSolidAnglePolygon(polygon, parameters.yz, direction);
    uint resultBase = index * LUDUS_PROJECTED_SOLID_ANGLE_PARITY_WORDS_PER_RESULT;
    Results[resultBase] = float4(isPrepared == true ? 1.0 : 0.0,
                                 polygon.ProjectedSolidAngle,
                                 polygon.IsMirrored == true ? 1.0 : 0.0,
                                 float(polygon.VertexCount));
    Results[resultBase + 1] = float4(isSampled == true ? 1.0 : 0.0, direction);
    Results[resultBase + 2] = float4(EvaluateProjectedSolidAnglePolygonDensity(polygon, direction),
                                     polygon.IsCentralCase == true ? 1.0 : 0.0,
                                     0.0,
                                     0.0);
}
