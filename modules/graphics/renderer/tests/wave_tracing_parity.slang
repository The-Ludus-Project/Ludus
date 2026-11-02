#include "math.slang"
#include "raster_bindings.slang"
#include "wave.slang"
#include "wave_tracing_parity_layout.h"
LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Cases;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint4> Results;

[shader("compute")][numthreads(LUDUS_WAVE_PARITY_CASE_COUNT, 1, 1)] void
WaveTracingParityMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    if (index >= LUDUS_WAVE_PARITY_CASE_COUNT)
        return;
    uint base = index * LUDUS_WAVE_PARITY_CASE_RECORDS;
    float4 first = asfloat(Cases[base]);
    float4 second = asfloat(Cases[base + 1]);
    float4 third = asfloat(Cases[base + 2]);
    ScalarWaveAperture aperture;
    aperture.WavelengthWorld = first.x;
    aperture.SigmaWorldSquared = first.yz;
    aperture.HalfExtentWorld = float2(first.w, second.x);
    aperture.Resolution = Cases[base + 1].y;
    float3 receiverApertureWorld = float3(second.zw, third.x);
    float2 field = float2(0);
    for (uint y = 0; y < aperture.Resolution; ++y)
    {
        for (uint x = 0; x < aperture.Resolution; ++x)
        {
            uint cell = y * aperture.Resolution + x;
            float transmission =
                asfloat(Cases[base + LUDUS_WAVE_PARITY_HEADER_RECORDS + cell / LUDUS_WAVE_PARITY_COMPONENTS]
                             [cell % LUDUS_WAVE_PARITY_COMPONENTS]);
            field += transmission *
                     EvaluateWaveCell(aperture, GetWaveCellPosition(aperture, uint2(x, y)), receiverApertureWorld);
        }
    }
    uint destination = index * LUDUS_WAVE_PARITY_RESULT_RECORDS;
    Results[destination] =
        asuint(float4(field, dot(field, field), CanResolveWavePhase(aperture, receiverApertureWorld) == true ? 1 : 0));
    float rho = third.y + third.z * aperture.WavelengthWorld / (2 * PI);
    Results[destination + 1] = asuint(ComputeWaveMoments(aperture.SigmaWorldSquared, rho));
}
