#include "elliptical_cone.slang"
#include "math.slang"
#include "raster_bindings.slang"
#include "wave.slang"
#include "wave_tracing_parity_layout.h"
LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Cases;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint4> Results;
#include "wave_scene_query_fixture.slang"

[shader("compute")][numthreads(LUDUS_WAVE_PARITY_CASE_COUNT, 1, 1)] void
WaveTracingParityMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    if (index >= LUDUS_WAVE_PARITY_CASE_COUNT)
        return;
    if (Cases[0].w == LUDUS_WAVE_PARITY_REGION_TAG || Cases[0].w == LUDUS_WAVE_PARITY_CONNECTION_TAG)
    {
        EvaluateWaveSceneQueryFixture(index);
        return;
    }
    uint base = index * LUDUS_WAVE_PARITY_CASE_RECORDS;
    if (Cases[base + 2].w == LUDUS_WAVE_PARITY_CONE_TAG)
    {
        float4 origin = asfloat(Cases[base]);
        float4 direction = asfloat(Cases[base + 1]);
        float4 axis = asfloat(Cases[base + 2]);
        float4 limits = asfloat(Cases[base + 3]);
        EllipticalCone cone;
        cone.OriginWorld = origin.xyz;
        cone.RadiusWorld = origin.w;
        cone.DirectionWorld = direction.xyz;
        cone.Slope = direction.w;
        cone.AxisXWorld = axis.xyz;
        cone.MinorToMajor = limits.x;
        cone.NearWorld = limits.y;
        cone.FarWorld = limits.z;
        ConeTriangle triangle;
        triangle.FirstWorld = asfloat(Cases[base + 4]).xyz;
        triangle.SecondWorld = asfloat(Cases[base + 5]).xyz;
        triangle.ThirdWorld = asfloat(Cases[base + 6]).xyz;
        ConeTriangleIntersection intersection = (ConeTriangleIntersection)0;
        bool isValid = TryIntersectEllipticalConeTriangle(cone, triangle, intersection);
        uint destination = index * LUDUS_WAVE_PARITY_RESULT_RECORDS;
        Results[destination] = asuint(float4(intersection.NearWorld,
                                             intersection.FarWorld,
                                             intersection.IsHit == true ? 1 : 0,
                                             isValid == true ? 1 : 0));
        Results[destination + 1] = asuint(float4(intersection.NearBarycentric, 0));
        return;
    }
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
