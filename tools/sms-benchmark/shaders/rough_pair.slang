// Thanks to Shlomi Steinberg, "Slice Sampling: Importance sampling with an
// auxiliary map" (University of Waterloo draft), Sections 4 and 7. This
// bounded fixture uses a reciprocal slope lobe with a monotone coupled map.
// Thanks to Eric Veach, Robust Monte Carlo Methods for Light Transport
// Simulation, Chapters 9–10, https://graphics.stanford.edu/papers/veach_thesis/,
// for the three fixed-endpoint connection strategies and balance MIS.
#include "math.slang"
#include "raster_bindings.slang"

static const uint GROUP_SIZE = 32;
static const uint RANDOM_KEY = 0x834095bc;
static const uint SOLVER_ITERATIONS = 16;
static const float SLOPE_TOLERANCE = 0.000004;
// Midpoints of 23-bit bins are exactly representable in FP32 and avoid
// both singular endpoints of the Gaussian and cosine-slope transforms.
static const uint OPEN_RANDOM_DISCARDED_BITS = 9;
static const float OPEN_RANDOM_BIN_WIDTH = 1.0 / 8388608;

struct PairParameters
{
    uint Count;
    uint Batch;
    uint Offset;
    uint Seed;
    float2 LightPositionWorld;
    float Width;
    float Curvature;
    float HalfExtent;
    float SourceReceiverOffset;
    uint ReplicasPerReceiver;
    uint Padding;
};

struct PairResult
{
    float2 FirstPositionWorld;
    float2 SecondPositionWorld;
    float Density;
    float Radiance;
    float HasFailed;
    float ShiftJacobian;
};

LUDUS_SHADER_BINDING(0) StructuredBuffer<float2> Receivers;
LUDUS_SHADER_BINDING(1) ConstantBuffer<PairParameters> Parameters;
LUDUS_SHADER_BINDING(2) RWStructuredBuffer<float4> Accumulation;
LUDUS_SHADER_BINDING(3) StructuredBuffer<float4> ProbeDraws;
LUDUS_SHADER_BINDING(4) RWStructuredBuffer<float4> Probes;

float2 WarpSlope(float2 slope)
{
    return slope * (1 + Parameters.Curvature * abs(slope));
}

float2 ComputeWarpDerivative(float2 slope)
{
    return 1 + 2 * Parameters.Curvature * abs(slope);
}

float2 UnwarpSlope(float2 value)
{
    return 2 * value / (1 + sqrt(1 + 4 * Parameters.Curvature * abs(value)));
}

float ComputeGaussian(float2 value)
{
    float variance = Parameters.Width * Parameters.Width;
    return exp(-dot(value, value) / (2 * variance)) / (2 * PI * variance);
}

float ComputeProjectedDensity(float2 slope)
{
    float denominator = 1 + dot(slope, slope);
    return 1 / (PI * denominator * denominator);
}

float2 ComputePolarVector(float radius, float azimuth)
{
    float angle = 2 * PI * azimuth;
    return radius * float2(cos(angle), sin(angle));
}

float2 DrawGaussian(float2 uniform)
{
    return Parameters.Width * ComputePolarVector(sqrt(-2 * log(uniform.x)), uniform.y);
}

float2 DrawCosineSlope(float2 uniform)
{
    return ComputePolarVector(sqrt(uniform.x / (1 - uniform.x)), uniform.y);
}

bool SolvePair(float2 firstSeed,
               float2 secondSeed,
               float2 receiverWorld,
               float2 lightWorld,
               out float2 outFirstWorld,
               out float2 outSecondWorld)
{
    // Eliminate the two positions given their separation. The remaining
    // equation is strictly increasing and bracketed between zero and the
    // separation of the uncoupled roots. Safeguard Newton within this bracket.
    float2 uncoupled = receiverWorld - lightWorld + UnwarpSlope(firstSeed) - UnwarpSlope(secondSeed);
    float2 lower = min(uncoupled, 0);
    float2 upper = max(uncoupled, 0);
    float2 middleSlope = uncoupled / 3;
    for (uint iteration = 0; iteration < SOLVER_ITERATIONS; ++iteration)
    {
        float2 middleWarped = WarpSlope(middleSlope);
        float2 firstSlope = UnwarpSlope(firstSeed - middleWarped);
        float2 secondSlope = UnwarpSlope(secondSeed + middleWarped);
        outFirstWorld = receiverWorld + firstSlope;
        outSecondWorld = lightWorld + secondSlope;
        float2 residual = middleSlope - (outFirstWorld - outSecondWorld);
        bool2 isConverged = abs(residual) <= SLOPE_TOLERANCE;
        if (all(isConverged))
        {
            return true;
        }
        lower = select(residual < 0, middleSlope, lower);
        upper = select(residual > 0, middleSlope, upper);
        float2 derivative = 1 + ComputeWarpDerivative(middleSlope) *
                                    (1 / ComputeWarpDerivative(firstSlope) + 1 / ComputeWarpDerivative(secondSlope));
        float2 candidate = middleSlope - residual / derivative;
        bool2 isInterior = (candidate > lower) & (candidate < upper);
        middleSlope = select(isConverged, middleSlope, select(isInterior, candidate, (lower + upper) * 0.5));
    }
    return false;
}

bool IsOnPatch(float2 positionWorld)
{
    return all(abs(positionWorld) <= Parameters.HalfExtent);
}

float ComputePairJacobian(float2 cameraSlope, float2 lightSlope, float2 middleSlope)
{
    float2 middleDerivative = ComputeWarpDerivative(middleSlope);
    float2 cameraDerivative = ComputeWarpDerivative(cameraSlope);
    float2 lightDerivative = ComputeWarpDerivative(lightSlope);
    float2 determinant = cameraDerivative * lightDerivative + middleDerivative * (cameraDerivative + lightDerivative);
    return determinant.x * determinant.y;
}

float3 EvaluatePair(float2 firstWorld, float2 secondWorld, float2 receiverWorld)
{
    float2 cameraSlope = firstWorld - receiverWorld;
    float2 lightSlope = secondWorld - Parameters.LightPositionWorld;
    float2 middleSlope = firstWorld - secondWorld;
    float2 middleWarped = WarpSlope(middleSlope);
    float firstLobe = ComputeGaussian(WarpSlope(cameraSlope) + middleWarped);
    float secondLobe = ComputeGaussian(WarpSlope(lightSlope) - middleWarped);
    float cameraDensity = ComputeProjectedDensity(cameraSlope);
    float lightDensity = ComputeProjectedDensity(lightSlope);
    float middleDensity = ComputeProjectedDensity(middleSlope);
    float2 middleDerivative = ComputeWarpDerivative(middleSlope);
    float physical = firstLobe * secondLobe * cameraDensity * lightDensity * middleDensity * PI * PI * PI;
    float sliceDensity = firstLobe * secondLobe * ComputePairJacobian(cameraSlope, lightSlope, middleSlope);
    float bdptDensity = (cameraDensity * lightDensity + middleDerivative.x * middleDerivative.y *
                                                            (cameraDensity * firstLobe + lightDensity * secondLobe)) /
                        3;
    return float3(physical, sliceDensity, bdptDensity);
}

PairResult SamplePair(bool isSlice, float4 uniforms, float split, float2 receiverWorld)
{
    PairResult result = {};
    if (isSlice)
    {
        if (SolvePair(DrawGaussian(uniforms.xy),
                      DrawGaussian(uniforms.zw),
                      receiverWorld,
                      Parameters.LightPositionWorld,
                      result.FirstPositionWorld,
                      result.SecondPositionWorld) == false)
        {
            result.HasFailed = 1;
            return result;
        }
    }
    else if (split < 1.0 / 3)
    {
        result.FirstPositionWorld = receiverWorld + DrawCosineSlope(uniforms.xy);
        result.SecondPositionWorld = Parameters.LightPositionWorld + DrawCosineSlope(uniforms.zw);
    }
    else if (split < 2.0 / 3)
    {
        result.FirstPositionWorld = receiverWorld + DrawCosineSlope(uniforms.xy);
        result.SecondPositionWorld =
            result.FirstPositionWorld -
            UnwarpSlope(DrawGaussian(uniforms.zw) - WarpSlope(result.FirstPositionWorld - receiverWorld));
    }
    else
    {
        result.SecondPositionWorld = Parameters.LightPositionWorld + DrawCosineSlope(uniforms.zw);
        result.FirstPositionWorld = result.SecondPositionWorld -
                                    UnwarpSlope(DrawGaussian(uniforms.xy) -
                                                WarpSlope(result.SecondPositionWorld - Parameters.LightPositionWorld));
    }
    if (IsOnPatch(result.FirstPositionWorld) && IsOnPatch(result.SecondPositionWorld))
    {
        float3 values = EvaluatePair(result.FirstPositionWorld, result.SecondPositionWorld, receiverWorld);
        result.Density = isSlice ? values.y : values.z;
        result.Radiance = values.x / result.Density;
    }
    return result;
}

// Thanks to Lin et al., Generalized Resampled Importance Sampling:
// Foundations of ReSTIR (SIGGRAPH 2022), for the partial-bijection support
// contract. Here one source proposal is pushed forward explicitly and
// combined with fresh target support by ordinary balance MIS.
bool MapPair(float2 firstWorld,
             float2 secondWorld,
             float2 sourceWorld,
             float2 targetWorld,
             out float2 outFirstWorld,
             out float2 outSecondWorld)
{
    float2 middle = WarpSlope(firstWorld - secondWorld);
    float2 firstSeed = WarpSlope(firstWorld - sourceWorld) + middle;
    float2 secondSeed = WarpSlope(secondWorld - Parameters.LightPositionWorld) - middle;
    return SolvePair(firstSeed, secondSeed, targetWorld, Parameters.LightPositionWorld, outFirstWorld, outSecondWorld);
}

PairResult SampleShiftMixture(float4 uniforms, float component, float2 receiverWorld)
{
    float2 sourceWorld = receiverWorld - float2(Parameters.SourceReceiverOffset, 0);
    bool isShift = component < 0.5;
    float2 drawReceiverWorld = isShift ? sourceWorld : receiverWorld;
    PairResult result = SamplePair(true, uniforms, component, drawReceiverWorld);
    if (result.HasFailed > 0 || result.Density == 0)
    {
        return result;
    }

    float2 mappedFirstWorld;
    float2 mappedSecondWorld;
    if (MapPair(result.FirstPositionWorld,
                result.SecondPositionWorld,
                drawReceiverWorld,
                isShift ? receiverWorld : sourceWorld,
                mappedFirstWorld,
                mappedSecondWorld) == false)
    {
        result.HasFailed = 1;
        return result;
    }
    bool isMappedOnPatch = IsOnPatch(mappedFirstWorld) && IsOnPatch(mappedSecondWorld);
    if (isShift)
    {
        if (isMappedOnPatch == false)
        {
            result.Density = 0;
            result.Radiance = 0;
            return result;
        }
        float sourceJacobian = ComputePairJacobian(result.FirstPositionWorld - sourceWorld,
                                                   result.SecondPositionWorld - Parameters.LightPositionWorld,
                                                   result.FirstPositionWorld - result.SecondPositionWorld);
        float targetJacobian = ComputePairJacobian(mappedFirstWorld - receiverWorld,
                                                   mappedSecondWorld - Parameters.LightPositionWorld,
                                                   mappedFirstWorld - mappedSecondWorld);
        result.ShiftJacobian = sourceJacobian / targetJacobian;
        result.Density /= result.ShiftJacobian;
        result.FirstPositionWorld = mappedFirstWorld;
        result.SecondPositionWorld = mappedSecondWorld;
        result.Radiance = EvaluatePair(mappedFirstWorld, mappedSecondWorld, receiverWorld).x / result.Density;
    }
    else if (isMappedOnPatch == false)
    {
        // The mapped proposal has no mass here. Only the fresh half of the
        // mixture contributes to this sample's density.
        result.Density *= 0.5;
        result.Radiance *= 2;
    }
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
    return (float(word >> OPEN_RANDOM_DISCARDED_BITS) + 0.5) * OPEN_RANDOM_BIN_WIDTH;
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
