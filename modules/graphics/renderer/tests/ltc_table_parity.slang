#include "ltc_table_parity_layout.h"
#include "raster_bindings.slang"

// Test-only kernel for projected_solid_angle_parity_tests.cpp: runs the renderer's shader lookup of its LTC table,
// bound as a buffer, so the host can compare it with src/ltc_table.cpp. ltc_table_parity_layout.h owns the case ABI.

LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> Cases;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<float4> Results;
// The table follows the lower bindings: Metal numbers buffers in declaration order.
#define LUDUS_LTC_TABLE_BINDING 2
#include "ltc_table.slang"

[shader("compute")][numthreads(LUDUS_LTC_TABLE_PARITY_GROUP_SIZE, 1, 1)] void
ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    float4 parameters = Cases[index * LUDUS_LTC_TABLE_PARITY_WORDS_PER_CASE];
    float3 f0 = Cases[index * LUDUS_LTC_TABLE_PARITY_WORDS_PER_CASE + 1].xyz;
    SpecularLtc ltc = LookupSpecularLtc(parameters.x, parameters.yzw, f0);
    uint resultBase = index * LUDUS_LTC_TABLE_PARITY_WORDS_PER_RESULT;
    [unroll] for (uint i = 0; i < 3; ++i)
    {
        Results[resultBase + i] = float4(ltc.InverseTransform[i], 0.0);
    }
    Results[resultBase + 3] = float4(ltc.Norm, ltc.FresnelNorm, 0.0, 0.0);
}
