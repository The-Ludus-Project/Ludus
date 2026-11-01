// Test-only kernel for steinberg_physical_optics_parity_tests.cpp: runs the steinberg-physical-optics template's
// scattering core (render_graphs/steinberg_physical_optics/main_shaders/wave_optics.slang) on cases from the host, as
// shade.slang's MakeInteraction combines it, so the host can compare every result with the CPU oracle
// (steinberg_physical_optics_oracle.cpp). steinberg_physical_optics_parity_layout.h owns the ABI.
#include "math.slang"
#include "raster_bindings.slang"
#include "steinberg_physical_optics_parity_layout.h"
#include "wave_optics.slang"

LUDUS_SHADER_BINDING(LUDUS_PHYSICAL_OPTICS_PARITY_CASES_BINDING) RWStructuredBuffer<uint4> Cases;
LUDUS_SHADER_BINDING(LUDUS_PHYSICAL_OPTICS_PARITY_RESULTS_BINDING) RWStructuredBuffer<uint4> Results;

float4 ReadCase(uint caseIndex, uint record)
{
    return asfloat(Cases[caseIndex * LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS + record]);
}

uint ReadCaseWord(uint caseIndex, uint record, uint word)
{
    return Cases[caseIndex * LUDUS_PHYSICAL_OPTICS_PARITY_CASE_RECORDS + record][word];
}

void WriteResult(uint caseIndex, uint record, float4 value)
{
    Results[caseIndex * LUDUS_PHYSICAL_OPTICS_PARITY_RESULT_RECORDS + record] = asuint(value);
}

[shader("compute")][numthreads(LUDUS_PHYSICAL_OPTICS_PARITY_GROUP_SIZE, 1, 1)] void
PhysicalOpticsParityMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint caseIndex = dispatchThreadId.x;
    if (caseIndex >= LUDUS_PHYSICAL_OPTICS_PARITY_CASE_COUNT)
        return;
    float4 propagation = ReadCase(caseIndex, 0);
    float4 transverse = ReadCase(caseIndex, 1);
    float4 normal = ReadCase(caseIndex, 5);
    float4 tangent = ReadCase(caseIndex, 6);
    float4 outgoing = ReadCase(caseIndex, 7);
    float4 lengths = ReadCase(caseIndex, 8);
    float4 envelope = ReadCase(caseIndex, 9);
    uint lobe = ReadCaseWord(caseIndex, 3, 3);
    bool isWave = ReadCaseWord(caseIndex, 4, 3) != 0;
    float wavelengthNanometers = propagation.w;
    Beam incident;
    incident.PropagationWorld = propagation.xyz;
    incident.TransverseXWorld = transverse.xyz;
    incident.DistanceWorld = transverse.w;
    incident.Stokes = ReadCase(caseIndex, 2);
    incident.ShapeX = ReadCase(caseIndex, 3).xyz;
    incident.ShapeY = ReadCase(caseIndex, 4).xyz;
    SurfaceFrame frame = MakeSurfaceFrame(normal.xyz, tangent.xyz);
    float3 incidentWorld = -incident.PropagationWorld;
    float3 outgoingWorld = outgoing.xyz;
    float wavenumber = 2 * PI * NANOMETERS_PER_WORLD_UNIT / wavelengthNanometers;
    float incidentCosine = dot(incidentWorld, frame.NormalWorld);
    float direct = ComputeDirectFraction(wavenumber, incidentCosine, outgoing.w);
    float2 halfVector = ComputeSurfaceHalfVector(frame, incidentWorld, outgoingWorld);

    Interaction interaction;
    interaction.Diffraction.X = 1;
    interaction.Diffraction.Y = 1;
    interaction.Diffraction.Half = 1;
    interaction.HasFresnel = true;
    interaction.HasShapeDeformation = false;
    interaction.Eta = MakeComplex(normal.w, tangent.w);
    interaction.Weight = envelope.z;
    if (lobe == LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_SCATTERED || lobe == LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_GRATING)
    {
        SurfaceCoherence coherenceX = MakeCoherentLimit();
        SurfaceCoherence coherenceY = MakeCoherentLimit();
        SurfaceCoherence coherenceHalf = MakeCoherentLimit();
        if (isWave == true)
        {
            coherenceX = ComputeSurfaceCoherence(incident, incident.ShapeX, frame, wavenumber, lengths.y);
            coherenceY = ComputeSurfaceCoherence(incident, incident.ShapeY, frame, wavenumber, lengths.y);
            coherenceHalf = ComputeSurfaceCoherence(incident,
                                                    0.5 * (incident.ShapeX + incident.ShapeY),
                                                    frame,
                                                    wavenumber,
                                                    lengths.y);
        }
        if (lobe == LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_GRATING)
        {
            Grating grating;
            grating.AxisSurface = float2(0, 1);
            grating.PeriodWorld = lengths.z;
            grating.HeightWorld = lengths.w;
            grating.MaximumOrder = ReadCaseWord(caseIndex, 9, 0);
            float wavelengthOverPeriod = wavelengthNanometers / (lengths.z * NANOMETERS_PER_WORLD_UNIT);
            int lastOrder = int(min(grating.MaximumOrder, OPTICS_GRATING_MAXIMUM_ORDER));
            float phaseAmplitude = ComputeGratingPhaseAmplitude(grating,
                                                                wavenumber,
                                                                incidentCosine,
                                                                dot(outgoingWorld, frame.NormalWorld));
            interaction.Diffraction.X = EvaluateGratingDiffraction(grating,
                                                                   halfVector,
                                                                   coherenceX,
                                                                   wavelengthOverPeriod,
                                                                   phaseAmplitude,
                                                                   -lastOrder,
                                                                   lastOrder);
            interaction.Diffraction.Y = EvaluateGratingDiffraction(grating,
                                                                   halfVector,
                                                                   coherenceY,
                                                                   wavelengthOverPeriod,
                                                                   phaseAmplitude,
                                                                   -lastOrder,
                                                                   lastOrder);
            interaction.Diffraction.Half = EvaluateGratingDiffraction(grating,
                                                                      halfVector,
                                                                      coherenceHalf,
                                                                      wavelengthOverPeriod,
                                                                      phaseAmplitude,
                                                                      -lastOrder,
                                                                      lastOrder);
        }
        else
        {
            float correlation = wavenumber * lengths.x;
            float inverseCorrelationSquared = 1 / (correlation * correlation);
            interaction.Diffraction.X =
                EvaluateGaussianSurfaceDiffraction(halfVector, coherenceX, inverseCorrelationSquared);
            interaction.Diffraction.Y =
                EvaluateGaussianSurfaceDiffraction(halfVector, coherenceY, inverseCorrelationSquared);
            interaction.Diffraction.Half =
                EvaluateGaussianSurfaceDiffraction(halfVector, coherenceHalf, inverseCorrelationSquared);
            interaction.Weight *= 1 - direct;
            interaction.HasShapeDeformation = isWave;
        }
    }
    else if (lobe == LUDUS_PHYSICAL_OPTICS_PARITY_LOBE_DIFFUSE)
    {
        interaction.HasFresnel = false;
    }
    else
    {
        interaction.Weight *= direct;
    }
    Beam scattered = ScatterBeam(incident, outgoingWorld, interaction);
    float density = ConvertHalfVectorDensity(EvaluateBivariateNormal(halfVector, float3(envelope.y, 0, envelope.y)),
                                             frame,
                                             incidentWorld);
    WriteResult(caseIndex, 0, scattered.Stokes);
    WriteResult(caseIndex, 1, float4(scattered.ShapeX, interaction.Diffraction.X));
    WriteResult(caseIndex, 2, float4(scattered.ShapeY, interaction.Diffraction.Y));
    WriteResult(caseIndex,
                3,
                float4(interaction.Diffraction.Half,
                       direct,
                       density,
                       ComputeCoherenceLengthWorld(incident, wavelengthNanometers)));
    WriteResult(caseIndex, 4, float4(scattered.TransverseXWorld, 0));
}
