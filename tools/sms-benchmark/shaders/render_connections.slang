#include "sms.slang"

LUDUS_SHADER_BINDING(5) ConstantBuffer<RenderParameters> Parameters;
static const uint RENDER_RANDOM_KEY = 0x17ca0571;
static const uint RANDOM_LIGHT_U = 0;
static const uint RANDOM_LIGHT_V = 1;
static const uint RANDOM_COMPONENT = 2;
static const uint RANDOM_POSITION_U = 3;
static const uint RANDOM_POSITION_V = 4;
static const uint RANDOM_BRANCH = 5;
static const uint RANDOM_SMOOTH_U = 6;
static const uint RANDOM_SMOOTH_V = 7;
static const uint RANDOM_INITIAL_U = 8;
static const uint RANDOM_INITIAL_V = 9;
static const uint RANDOM_REFERENCE_COMPONENT = 10;
static const float MINIMUM_OPEN_PROBABILITY = 1e-8;
static const float SQRT_TWO = 1.4142135623730951;

float DrawUniform(uint pixel, uint sampleIndex, uint trial, uint dimension)
{
    return SampleRandomFloat01(uint2(Parameters.Seed, RENDER_RANDOM_KEY),
                               MakeRandomAddress(uint2(pixel, sampleIndex), trial, dimension));
}

float2 DrawNormal(uint pixel, uint sampleIndex, uint trial, uint dimension)
{
    float first = max(MINIMUM_OPEN_PROBABILITY, DrawUniform(pixel, sampleIndex, trial, dimension));
    float second = max(MINIMUM_OPEN_PROBABILITY, DrawUniform(pixel, sampleIndex, trial, dimension + 1));
    return float2(ComputeStandardNormalQuantile(first), ComputeStandardNormalQuantile(second));
}

float3 GetLightPosition(float2 coordinatesUv)
{
    return SceneData[LIGHT_CENTER_RECORD].xyz + (coordinatesUv.x - 0.5) * SceneData[LIGHT_DERIVATIVE_U_RECORD].xyz +
           (coordinatesUv.y - 0.5) * SceneData[LIGHT_DERIVATIVE_V_RECORD].xyz;
}

float ComputeFloorPdfUv(float3 receiverWorld, float2 coordinatesUv)
{
    float3 positionWorld = SceneData[REFLECTOR_CENTER_RECORD].xyz +
                           (coordinatesUv.x - 0.5) * SceneData[REFLECTOR_DERIVATIVE_U_RECORD].xyz +
                           (coordinatesUv.y - 0.5) * SceneData[REFLECTOR_DERIVATIVE_V_RECORD].xyz;
    float3 toReflectorWorld = positionWorld - receiverWorld;
    float3 directionWorld = NormalizeOrZero(toReflectorWorld);
    float floorCosine = max(0, dot(SceneData[FLOOR_NORMAL_RECORD].xyz, directionWorld));
    float reflectorCosine = max(0, dot(SceneData[BASE_NORMAL_RECORD].xyz, -directionWorld));
    return floorCosine / PI * SceneData[AREA_RADIANCE_RECORD].x * reflectorCosine /
           dot(toReflectorWorld, toReflectorWorld);
}

float4 SampleConnectionMis(uint pixel, uint sampleIndex, float3 receiverWorld)
{
    float2 coordinatesUv = float2(DrawUniform(pixel, sampleIndex, 0, RANDOM_POSITION_U),
                                  DrawUniform(pixel, sampleIndex, 0, RANDOM_POSITION_V));
    if (DrawUniform(pixel, sampleIndex, 0, RANDOM_COMPONENT) < 0.5)
    {
        // The original floor is horizontal and Y-up. A cosine-weighted floor
        // direction plus first-hit projection defines its UV subdensity.
        float radius = sqrt(coordinatesUv.x);
        float angle = 2 * PI * coordinatesUv.y;
        float3 directionWorld = float3(radius * cos(angle), sqrt(max(0, 1 - coordinatesUv.x)), radius * sin(angle));
        float4 hit = IntersectRectangles(receiverWorld, directionWorld);
        if (hit.y != 1)
            return float4(0);
        coordinatesUv = hit.zw;
    }
    return float4(coordinatesUv, 0.5 + 0.5 * ComputeFloorPdfUv(receiverWorld, coordinatesUv), 1);
}

float4 SampleSliceConnection(uint pixel, uint sampleIndex, uint guide)
{
    uint resolution = Parameters.Resolution;
    uint offset = guide * (resolution + 1) * (resolution + 1);
    // Broaden only the proposal to tolerate interpolation error in the inverse
    // map. Physical Beckmann alpha remains Parameters.Alpha in the integrand.
    float guideAlpha = max(Parameters.Alpha, Parameters.MinimumGuideAlpha);
    if (DrawUniform(pixel, sampleIndex, 0, RANDOM_COMPONENT) < DEFENSIVE_FRACTION)
    {
        float2 coordinatesUv = float2(DrawUniform(pixel, sampleIndex, 0, RANDOM_POSITION_U),
                                      DrawUniform(pixel, sampleIndex, 0, RANDOM_POSITION_V));
        float density = EvaluateAffineDensity(coordinatesUv, guideAlpha, resolution, offset, guide);
        return float4(coordinatesUv, density, 1);
    }
    float2 residual = DrawNormal(pixel, sampleIndex, 0, RANDOM_POSITION_U) * (guideAlpha / SQRT_TWO);
    uint rootCount;
    float totalWeight;
    return SampleAffineSlice(residual,
                             DrawUniform(pixel, sampleIndex, 0, RANDOM_BRANCH),
                             guideAlpha,
                             resolution,
                             offset,
                             rootCount,
                             totalWeight,
                             guide);
}

float4 SampleReferenceConnection(uint pixel, uint sampleIndex, uint guide, float3 receiverWorld)
{
    // The outer selector has its own random dimension: conditioning the same
    // selector reused by either child would change that child's mixture law.
    bool shouldUseSlice = DrawUniform(pixel, sampleIndex, 0, RANDOM_REFERENCE_COMPONENT) < 0.5;
    float4 sample;
    if (shouldUseSlice == true)
        sample = SampleSliceConnection(pixel, sampleIndex, guide);
    else
        sample = SampleConnectionMis(pixel, sampleIndex, receiverWorld);
    if (sample.w <= 0)
        return float4(0);
    float otherDensity;
    if (shouldUseSlice == true)
        otherDensity = 0.5 + 0.5 * ComputeFloorPdfUv(receiverWorld, sample.xy);
    else
    {
        uint resolution = Parameters.Resolution;
        uint offset = guide * (resolution + 1) * (resolution + 1);
        otherDensity = EvaluateAffineDensity(sample.xy,
                                             max(Parameters.Alpha, Parameters.MinimumGuideAlpha),
                                             resolution,
                                             offset,
                                             guide);
    }
    sample.z = 0.5 * (sample.z + otherDensity);
    return sample;
}

bool DrawSmsRoot(uint pixel,
                 uint sampleIndex,
                 uint trial,
                 float3 receiverWorld,
                 float3 lightWorld,
                 float2 residual,
                 out float2 outCoordinatesUv)
{
    float2 initialUv = float2(DrawUniform(pixel, sampleIndex, trial, RANDOM_INITIAL_U),
                              DrawUniform(pixel, sampleIndex, trial, RANDOM_INITIAL_V));
    float2 normalDraw = DrawNormal(pixel, sampleIndex, trial, RANDOM_SMOOTH_U);
    return SolveTwoStage(receiverWorld, lightWorld, residual, initialUv, normalDraw, outCoordinatesUv);
}

float3 SampleSmsConnection(uint pixel,
                           uint sampleIndex,
                           float3 receiverWorld,
                           float3 color,
                           float2 lightCoordinatesUv,
                           out float3 outDiagnostics)
{
    outDiagnostics = float3(1, 0, 0);
    float3 lightWorld = GetLightPosition(lightCoordinatesUv);
    float2 residual = DrawNormal(pixel, sampleIndex, 0, RANDOM_POSITION_U) * (Parameters.Alpha / SQRT_TWO);
    float2 coordinatesUv;
    if (DrawSmsRoot(pixel, sampleIndex, 0, receiverWorld, lightWorld, residual, coordinatesUv) == false)
    {
        outDiagnostics.z = 1;
        return float3(0);
    }
    ReflectionMap map = EvaluateReflectionMap(receiverWorld, lightWorld, coordinatesUv, false);
    float jacobian = abs(ComputeDeterminant(map.DerivativeU, map.DerivativeV));
    if (jacobian < MINIMUM_MAP_DETERMINANT)
    {
        outDiagnostics.z = 1;
        return float3(0);
    }
    uint inverseProbability = 0;
    for (uint trial = 1; trial <= MAXIMUM_BERNOULLI_TRIALS; ++trial)
    {
        float2 trialUv;
        bool hasRoot = DrawSmsRoot(pixel, sampleIndex, trial, receiverWorld, lightWorld, residual, trialUv);
        outDiagnostics.x += 1;
        if (hasRoot == true && length(trialUv - coordinatesUv) < REHIT_DISTANCE_UV)
        {
            inverseProbability = trial;
            break;
        }
    }
    if (inverseProbability == 0)
    {
        outDiagnostics.y = 1;
        return float3(0);
    }
    // Densities are per slope area and per UV area, respectively. The light
    // endpoint is uniform in its UV square in every method.
    float guideDensity = exp(-dot(residual, residual) / (Parameters.Alpha * Parameters.Alpha)) /
                         (PI * Parameters.Alpha * Parameters.Alpha);
    return EvaluateConnection(receiverWorld, color, coordinatesUv, lightCoordinatesUv, Parameters.Alpha) *
           (float(inverseProbability) / (guideDensity * jacobian));
}

void RenderConnectionMethod<let method : uint>(uint3 threadIndex)
{
    if (threadIndex.x >= Parameters.PixelCount)
        return;
    uint pixel = Parameters.PixelOffset + threadIndex.x;
    uint inputRecord = pixel * INPUT_RECORD_COUNT;
    uint outputRecord = pixel * OUTPUT_RECORD_COUNT;
    float4 receiver = Inputs[inputRecord];
    float3 color = Inputs[inputRecord + 1].xyz;
    uint guide = uint(Inputs[inputRecord + 1].w);
    float4 accumulation = Results[outputRecord];
    float4 secondMoment = Results[outputRecord + 1];
    float4 diagnostics = Results[outputRecord + 2];
    uint beginningSample = uint(accumulation.w);
    for (uint offset = 0; offset < Parameters.Batch; ++offset)
    {
        uint sampleIndex = beginningSample + offset;
        float3 contribution = float3(0);
        if (receiver.w > 0)
        {
            float2 lightCoordinatesUv = float2(DrawUniform(pixel, sampleIndex, 0, RANDOM_LIGHT_U),
                                               DrawUniform(pixel, sampleIndex, 0, RANDOM_LIGHT_V));
            if (method == METHOD_SMS)
            {
                float3 samplerDiagnostics;
                contribution = SampleSmsConnection(pixel,
                                                   sampleIndex,
                                                   receiver.xyz,
                                                   color,
                                                   lightCoordinatesUv,
                                                   samplerDiagnostics);
                diagnostics.xyz += samplerDiagnostics;
            }
            else
            {
                float4 sample;
                if (method == METHOD_SLICE)
                    sample = SampleSliceConnection(pixel, sampleIndex, guide);
                else if (method == METHOD_REFERENCE)
                    sample = SampleReferenceConnection(pixel, sampleIndex, guide, receiver.xyz);
                else
                    sample = SampleConnectionMis(pixel, sampleIndex, receiver.xyz);
                if (sample.w > 0 && sample.z > 0)
                    contribution =
                        EvaluateConnection(receiver.xyz, color, sample.xy, lightCoordinatesUv, Parameters.Alpha) /
                        sample.z;
                else
                    diagnostics.z += 1;
            }
        }
        if (any(isnan(contribution)) || any(isinf(contribution)) || any(contribution < 0))
        {
            diagnostics.w += 1;
            contribution = float3(0);
        }
        accumulation.xyz += contribution;
        secondMoment.xyz += contribution * contribution;
        secondMoment.w += any(contribution > 0) ? 1 : 0;
    }
    accumulation.w += float(Parameters.Batch);
    Results[outputRecord] = accumulation;
    Results[outputRecord + 1] = secondMoment;
    Results[outputRecord + 2] = diagnostics;
}

FIGURE17_PARITY_ENTRY
void RenderConnectionsMis(uint3 threadIndex : SV_DispatchThreadID)
{
    RenderConnectionMethod<METHOD_CONNECTION_MIS>(threadIndex);
}

FIGURE17_PARITY_ENTRY
void RenderConnectionsSlice(uint3 threadIndex : SV_DispatchThreadID)
{
    RenderConnectionMethod<METHOD_SLICE>(threadIndex);
}

FIGURE17_PARITY_ENTRY
void RenderConnectionsSms(uint3 threadIndex : SV_DispatchThreadID)
{
    RenderConnectionMethod<METHOD_SMS>(threadIndex);
}

FIGURE17_PARITY_ENTRY
void RenderConnectionsReference(uint3 threadIndex : SV_DispatchThreadID)
{
    RenderConnectionMethod<METHOD_REFERENCE>(threadIndex);
}
