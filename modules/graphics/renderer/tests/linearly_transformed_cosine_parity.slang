#include "linearly_transformed_cosine.slang"
#include "linearly_transformed_cosine_parity_layout.h"
#include "raster_bindings.slang"

// Test-only kernel for projected_solid_angle_parity_tests.cpp: runs FoundationMath's shader port of linearly
// transformed cosines on host-supplied polygons and inverse transforms so the host can compare it with the C++
// implementation. linearly_transformed_cosine_parity_layout.h owns the case ABI.

LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> Cases;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<float4> Results;

[shader("compute")][numthreads(LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_GROUP_SIZE, 1, 1)] void
ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    uint caseBase = index * LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_CASE;
    ProjectedPolygonInput input;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        input.PositionsTangent[i] = Cases[caseBase + i].xyz;
    }
    float4 parameters = Cases[caseBase + MAX_PROJECTED_POLYGON_INPUT_VERTICES];
    input.VertexCount = uint(parameters.x);
    float3 inverseTransform[3];
    [unroll] for (uint i = 0; i < 3; ++i)
    {
        inverseTransform[i] = Cases[caseBase + MAX_PROJECTED_POLYGON_INPUT_VERTICES + 1 + i].xyz;
    }
    LinearlyTransformedCosine distribution;
    bool isMade = TryMakeLinearlyTransformedCosine(inverseTransform, distribution);
    LinearlyTransformedCosinePolygon polygon;
    bool isPrepared = TryPrepareLinearlyTransformedCosinePolygon(input, distribution, polygon);
    float3 direction;
    bool isSampled = TrySampleLinearlyTransformedCosinePolygon(polygon, parameters.yz, direction);
    uint resultBase = index * LUDUS_LINEARLY_TRANSFORMED_COSINE_PARITY_WORDS_PER_RESULT;
    Results[resultBase] = float4(isMade == true ? 1.0 : 0.0,
                                 isPrepared == true ? 1.0 : 0.0,
                                 GetLinearlyTransformedCosinePolygonIntegral(polygon),
                                 float(polygon.TransformedPolygon.VertexCount));
    Results[resultBase + 1] = float4(isSampled == true ? 1.0 : 0.0, direction);
    Results[resultBase + 2] = float4(EvaluateLinearlyTransformedCosinePolygonDensity(polygon, direction),
                                     EvaluateLinearlyTransformedCosine(distribution, direction),
                                     distribution.InverseDeterminant,
                                     0.0);
}
