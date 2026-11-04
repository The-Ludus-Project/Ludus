#include "math.slang"
#include "raster_bindings.slang"
#include "rough_pair_transport.slang"

static const uint GROUP_SIZE = 32;
static const uint RANDOM_KEY = 0x834095bc;
struct PairParameters
{
    uint Count;
    uint Batch;
    uint Offset;
    uint Seed;
    float SourceReceiverOffset;
    uint ReplicasPerReceiver;
    uint ReceiverGridWidth;
    uint Padding;
};

LUDUS_SHADER_BINDING(0) StructuredBuffer<float2> Receivers;
LUDUS_SHADER_BINDING(1) ConstantBuffer<PairParameters> Parameters;
LUDUS_SHADER_BINDING(2) RWStructuredBuffer<float4> Accumulation;
LUDUS_SHADER_BINDING(3) StructuredBuffer<float4> ProbeDraws;
LUDUS_SHADER_BINDING(4) RWStructuredBuffer<float4> Probes;

PairResult SampleShiftMixture(float4 uniforms, float component, float2 receiverWorld)
{
    float2 sourceWorld = receiverWorld - float2(Parameters.SourceReceiverOffset, 0);
    if (component < 0.5)
    {
        return ShiftPairSample(SamplePair(true, uniforms, component, sourceWorld), sourceWorld, receiverWorld);
    }
    PairResult result = SamplePair(true, uniforms, component, receiverWorld);
    bool hasFailed;
    float weight = ComputeFreshMisWeight(result, receiverWorld, sourceWorld, hasFailed);
    if (weight > 0)
    {
        result.Density *= 0.5 / weight;
        result.Radiance *= 2 * weight;
    }
    result.HasFailed = max(result.HasFailed, hasFailed ? 1 : 0);
    return result;
}

enum PairProposal : uint
{
    Slice,
    Bidirectional,
    ShiftMixture,
};

float DrawUniform(uint pixel, uint sampleIndex, uint dimension)
{
    uint word = SampleRandomUInt32(uint2(Parameters.Seed, RANDOM_KEY),
                                   MakeRandomAddress(uint2(pixel, sampleIndex), 0, dimension));
    return ComputeRoughPairOpenUniform(word);
}

void AccumulatePair(uint pixel, PairProposal proposal)
{
    if (pixel >= Parameters.Count)
    {
        return;
    }
    float2 receiverWorld = Receivers[pixel / Parameters.ReplicasPerReceiver];
    float4 total = 0;
    for (uint index = 0; index < Parameters.Batch; ++index)
    {
        uint sampleIndex = Parameters.Offset + index;
        float4 uniforms = float4(DrawUniform(pixel, sampleIndex, 0),
                                 DrawUniform(pixel, sampleIndex, 1),
                                 DrawUniform(pixel, sampleIndex, 2),
                                 DrawUniform(pixel, sampleIndex, 3));
        float component = DrawUniform(pixel, sampleIndex, 4);
        PairResult result = proposal == PairProposal::ShiftMixture
                                ? SampleShiftMixture(uniforms, component, receiverWorld)
                                : SamplePair(proposal == PairProposal::Slice, uniforms, component, receiverWorld);
        total +=
            float4(result.Radiance, result.Radiance * result.Radiance, result.Density > 0 ? 0 : 1, result.HasFailed);
    }
    Accumulation[pixel] += total;
}

[numthreads(GROUP_SIZE, 1, 1)] void RenderRoughPairSlice(uint3 invocation : SV_DispatchThreadID) {
    AccumulatePair(invocation.x, PairProposal::Slice);
}

    [numthreads(GROUP_SIZE, 1, 1)] void RenderRoughPairBdpt(uint3 invocation : SV_DispatchThreadID)
{
    AccumulatePair(invocation.x, PairProposal::Bidirectional);
}

[numthreads(GROUP_SIZE, 1, 1)] void RenderRoughPairShift(uint3 invocation : SV_DispatchThreadID) {
    AccumulatePair(invocation.x, PairProposal::ShiftMixture);
}

void WriteProbe(uint index, PairResult result)
{
    Probes[index] = float4(result.FirstPositionWorld, result.SecondPositionWorld);
    Probes[index + 1] = float4(result.Density, result.Radiance, result.HasFailed, result.ShiftJacobian);
}

[numthreads(GROUP_SIZE, 1, 1)] void ProbeRoughPair(uint3 invocation : SV_DispatchThreadID) {
    uint index = invocation.x;
    if (index < Parameters.Count)
    {
        float4 uniforms = ProbeDraws[2 * index];
        float split = ProbeDraws[2 * index + 1].x;
        const uint recordsPerProbe = 7;
        uint outputIndex = recordsPerProbe * index;
        float2 receiverWorld = Receivers[index];
        WriteProbe(outputIndex, SamplePair(true, uniforms, split, receiverWorld));
        WriteProbe(outputIndex + 2, SamplePair(false, uniforms, split, receiverWorld));
        PairResult shifted = SampleShiftMixture(uniforms, split, receiverWorld);
        if (shifted.ShiftJacobian > 0)
        {
            float2 restoredFirstWorld;
            float2 restoredSecondWorld;
            if (MapPair(shifted.FirstPositionWorld,
                        shifted.SecondPositionWorld,
                        receiverWorld,
                        receiverWorld - float2(Parameters.SourceReceiverOffset, 0),
                        restoredFirstWorld,
                        restoredSecondWorld) == false)
            {
                shifted.HasFailed = 1;
            }
            Probes[outputIndex + 6] = float4(restoredFirstWorld, restoredSecondWorld);
        }
        WriteProbe(outputIndex + 4, shifted);
    }
}
