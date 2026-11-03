#include "reflection_map.slang"

// An independent reference: shifted Hammersley points for the two-dimensional
// surface/direction charts, independently uniform area-light endpoints, and
// balance MIS. It does not use an affine guide, a Newton root or a re-hit PDF.
LUDUS_SHADER_BINDING(5) ConstantBuffer<QmcParameters> QuadratureParameters;
static const uint QMC_RANDOM_KEY = 0x17beef91;
static const uint QMC_SHIFT_SAMPLE_ADDRESS = 0xffffffff;

float ComputeReferenceFloorPdf(float3 receiverWorld, float2 coordinatesUv)
{
    float3 positionWorld = SceneData[REFLECTOR_CENTER_RECORD].xyz +
                           (coordinatesUv.x - 0.5) * SceneData[REFLECTOR_DERIVATIVE_U_RECORD].xyz +
                           (coordinatesUv.y - 0.5) * SceneData[REFLECTOR_DERIVATIVE_V_RECORD].xyz;
    float3 displacementWorld = positionWorld - receiverWorld;
    float3 directionWorld = NormalizeOrZero(displacementWorld);
    float floorCosine = max(0, dot(SceneData[FLOOR_NORMAL_RECORD].xyz, directionWorld));
    float reflectorCosine = max(0, dot(SceneData[BASE_NORMAL_RECORD].xyz, -directionWorld));
    return floorCosine / PI * SceneData[AREA_RADIANCE_RECORD].x * reflectorCosine /
           dot(displacementWorld, displacementWorld);
}

FIGURE17_PARITY_ENTRY
void IntegrateConnections(uint3 threadIndex : SV_DispatchThreadID)
{
    uint block = threadIndex.x % QuadratureParameters.BlocksPerPixel;
    uint pixelInBatch = threadIndex.x / QuadratureParameters.BlocksPerPixel;
    if (pixelInBatch >= QuadratureParameters.PixelCount)
        return;
    uint pixel = QuadratureParameters.PixelOffset + pixelInBatch;
    float4 receiver = Inputs[pixel * INPUT_RECORD_COUNT];
    float3 color = Inputs[pixel * INPUT_RECORD_COUNT + 1].xyz;
    float3 total = float3(0);
    if (receiver.w > 0)
    {
        uint2 key = uint2(QuadratureParameters.Seed, QMC_RANDOM_KEY);
        RandomAddress shiftAddress = MakeRandomAddress(uint2(pixel, QMC_SHIFT_SAMPLE_ADDRESS), 0, 0);
        uint4 shiftBits = Philox4x32(PackRandomCounter(shiftAddress, 0), key);
        float2 shift = float2(RandomFloat01(shiftBits.x), RandomFloat01(shiftBits.y));
        for (uint localSample = 0; localSample < QuadratureParameters.SamplesPerBlock; ++localSample)
        {
            uint sampleIndex = block * QuadratureParameters.SamplesPerBlock + localSample;
            if (sampleIndex >= QuadratureParameters.SampleCount)
                break;
            float2 point = frac(float2((float(sampleIndex) + 0.5) / QuadratureParameters.SampleCount,
                                       RandomFloat01(reversebits(sampleIndex))) +
                                shift);
            RandomAddress lightAddress = MakeRandomAddress(uint2(pixel, sampleIndex), 0, 0);
            uint4 lightBits = Philox4x32(PackRandomCounter(lightAddress, 0), key);
            float2 lightCoordinatesUv = float2(RandomFloat01(lightBits.z), RandomFloat01(lightBits.w));
            // Deterministic allocation to both charts: each gets its own
            // balance weight, so their two expectations sum to the integral.
            float areaPdf = ComputeReferenceFloorPdf(receiver.xyz, point);
            total += EvaluateConnection(receiver.xyz, color, point, lightCoordinatesUv, QuadratureParameters.Alpha) /
                     (1 + areaPdf);
            float radius = sqrt(point.x);
            float angle = 2 * PI * point.y;
            float3 directionWorld = float3(radius * cos(angle), sqrt(max(0, 1 - point.x)), radius * sin(angle));
            float4 hit = IntersectRectangles(receiver.xyz, directionWorld);
            if (hit.y == 1)
            {
                float cosinePdf = ComputeReferenceFloorPdf(receiver.xyz, hit.zw);
                total +=
                    EvaluateConnection(receiver.xyz, color, hit.zw, lightCoordinatesUv, QuadratureParameters.Alpha) /
                    (1 + cosinePdf);
            }
        }
    }
    Results[threadIndex.x] = float4(total, 0);
}
