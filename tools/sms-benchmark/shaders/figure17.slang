#include "layout.slang"
#include "math.slang"
#include "raster_bindings.slang"

// Thanks to Tizian Zeltner, Iliyan Georgiev and Wenzel Jakob, "Specular Manifold Sampling for Rendering
// High-Frequency Caustics and Glints", SIGGRAPH 2020, sections 4.5/5.5 and Figure 17,
// https://doi.org/10.1145/3386569.3392408. Port their level-zero slope interpolation, shading frame,
// Beckmann NDF and rational Smith masking approximation. This is a material/geometry parity kernel;
// it does not implement SMS or establish path-tracing throughput.
LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> Inputs;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<float4> Results;
LUDUS_SHADER_BINDING(2) StructuredBuffer<float4> Slopes;
LUDUS_SHADER_BINDING(3) StructuredBuffer<float4> SceneData;

static const float RAY_PARALLEL_COSINE = 1e-12;
static const float RAY_MINIMUM_DISTANCE_WORLD = 1e-5;
static const float MAXIMUM_DISTANCE_WORLD = 1e30;
static const float SMITH_NO_MASKING_THRESHOLD = 1.6;
static const float SMITH_NUMERATOR_LINEAR = 3.535;
static const float SMITH_NUMERATOR_QUADRATIC = 2.181;
static const float SMITH_DENOMINATOR_LINEAR = 2.276;
static const float SMITH_DENOMINATOR_QUADRATIC = 2.577;
static const float FRESNEL_COMPLEX_MULTIPLIER = 4;
static const float MICROFACET_REFLECTION_MULTIPLIER = 4;

float2 EvaluateSlopes(float2 coordinates)
{
    uint resolution = uint(SceneData[BASE_TANGENT_RECORD].w);
    float2 positionTexel = clamp(frac(coordinates) * resolution - 0.5, 0, float(resolution - 1));
    uint2 lowerTexel = uint2(floor(positionTexel));
    uint2 upperTexel = min(lowerTexel + 1, resolution - 1);
    float2 weight = positionTexel - lowerTexel;
    float2 first = lerp(Slopes[lowerTexel.y * resolution + lowerTexel.x].xy,
                        Slopes[lowerTexel.y * resolution + upperTexel.x].xy,
                        weight.x);
    float2 second = lerp(Slopes[upperTexel.y * resolution + lowerTexel.x].xy,
                         Slopes[upperTexel.y * resolution + upperTexel.x].xy,
                         weight.x);
    return lerp(first, second, weight.y);
}

float ComputeSmithBeckmann(float cosine, float alpha)
{
    float sine = sqrt(max(0, 1 - cosine * cosine));
    if (sine == 0)
        return 1;
    float a = abs(cosine) / (alpha * sine);
    if (a >= SMITH_NO_MASKING_THRESHOLD)
        return 1;
    float square = a * a;
    return (SMITH_NUMERATOR_LINEAR * a + SMITH_NUMERATOR_QUADRATIC * square) /
           (1 + SMITH_DENOMINATOR_LINEAR * a + SMITH_DENOMINATOR_QUADRATIC * square);
}

float3 ComputeConductorFresnel(float cosine)
{
    float3 eta = SceneData[IOR_ETA_RECORD].xyz;
    float3 extinction = SceneData[IOR_EXTINCTION_RECORD].xyz;
    float sineSquared = 1 - cosine * cosine;
    float3 difference = eta * eta - extinction * extinction - sineSquared;
    float3 magnitude = sqrt(difference * difference + FRESNEL_COMPLEX_MULTIPLIER * eta * eta * extinction * extinction);
    float3 realPart = sqrt(max(0, (magnitude + difference) * 0.5));
    float3 first = magnitude + cosine * cosine;
    float3 second = 2 * cosine * realPart;
    float3 perpendicular = (first - second) / (first + second);
    float3 third = cosine * cosine * magnitude + sineSquared * sineSquared;
    float3 fourth = second * sineSquared;
    return 0.5 * perpendicular * (1 + (third - fourth) / (third + fourth));
}

float3 EvaluateBeckmann(float2 coordinates, float3 viewWorld, float3 lightWorld, float alpha, out float3 outNormalWorld)
{
    float2 slope = EvaluateSlopes(coordinates);
    float3 baseTangentWorld = SceneData[BASE_TANGENT_RECORD].xyz;
    float3 baseBitangentWorld = SceneData[BASE_BITANGENT_RECORD].xyz;
    float3 baseNormalWorld = SceneData[BASE_NORMAL_RECORD].xyz;
    float3 normalWorld = NormalizeOrZero(baseNormalWorld - slope.x * baseTangentWorld - slope.y * baseBitangentWorld);
    float3 tangentWorld = NormalizeOrZero(baseTangentWorld - normalWorld * dot(normalWorld, baseTangentWorld));
    float3 bitangentWorld = cross(normalWorld, tangentWorld);
    outNormalWorld = normalWorld;
    float3 viewShading =
        float3(dot(viewWorld, tangentWorld), dot(viewWorld, bitangentWorld), dot(viewWorld, normalWorld));
    float3 lightShading =
        float3(dot(lightWorld, tangentWorld), dot(lightWorld, bitangentWorld), dot(lightWorld, normalWorld));
    if (viewShading.z <= 0 || lightShading.z <= 0 || dot(viewWorld, baseNormalWorld) <= 0 ||
        dot(lightWorld, baseNormalWorld) <= 0)
        return float3(0);
    float3 halfwayShading = NormalizeOrZero(viewShading + lightShading);
    if (halfwayShading.z <= 0)
        return float3(0);
    float cosineSquared = halfwayShading.z * halfwayShading.z;
    float exponent = -dot(halfwayShading.xy, halfwayShading.xy) / (alpha * alpha * cosineSquared);
    float distribution = exp(exponent) / (PI * alpha * alpha * cosineSquared * cosineSquared);
    float masking = ComputeSmithBeckmann(viewShading.z, alpha) * ComputeSmithBeckmann(lightShading.z, alpha);
    return ComputeConductorFresnel(abs(dot(viewShading, halfwayShading))) *
           (distribution * masking / (MICROFACET_REFLECTION_MULTIPLIER * viewShading.z));
}

float4 IntersectRectangles(float3 originWorld, float3 directionWorld)
{
    float distanceWorld = MAXIMUM_DISTANCE_WORLD;
    float identity = -1;
    float2 coordinates = float2(0);
    for (uint rectangle = 0; rectangle < RECTANGLE_COUNT; ++rectangle)
    {
        uint record = INVERSE_TRANSFORM_RECORD + rectangle * INVERSE_TRANSFORM_STRIDE;
        float3 originRectangle = float3(dot(SceneData[record], float4(originWorld, 1)),
                                        dot(SceneData[record + 1], float4(originWorld, 1)),
                                        dot(SceneData[record + 2], float4(originWorld, 1)));
        float3 directionRectangle = float3(dot(SceneData[record].xyz, directionWorld),
                                           dot(SceneData[record + 1].xyz, directionWorld),
                                           dot(SceneData[record + 2].xyz, directionWorld));
        if (abs(directionRectangle.z) <= RAY_PARALLEL_COSINE)
            continue;
        float candidateDistanceWorld = -originRectangle.z / directionRectangle.z;
        float2 positionRectangle = originRectangle.xy + candidateDistanceWorld * directionRectangle.xy;
        if (candidateDistanceWorld > RAY_MINIMUM_DISTANCE_WORLD && candidateDistanceWorld < distanceWorld &&
            all(abs(positionRectangle) <= 1))
        {
            distanceWorld = candidateDistanceWorld;
            identity = float(rectangle);
            coordinates = (positionRectangle + 1) * 0.5;
        }
    }
    return float4(distanceWorld, identity, coordinates);
}

#define FIGURE17_PARITY_ENTRY [shader("compute")][numthreads(PARITY_GROUP_SIZE, 1, 1)]
FIGURE17_PARITY_ENTRY
void ValidateFigure17(uint3 threadIndex : SV_DispatchThreadID)
{
    uint count;
    uint stride;
    Inputs.GetDimensions(count, stride);
    uint index = threadIndex.x;
    if (index >= count / INPUT_RECORD_COUNT)
        return;
    uint inputRecord = index * INPUT_RECORD_COUNT;
    float4 coordinates = Inputs[inputRecord];
    float3 normalWorld;
    float3 value = EvaluateBeckmann(coordinates.xy,
                                    Inputs[inputRecord + 1].xyz,
                                    Inputs[inputRecord + LIGHT_INPUT_RECORD].xyz,
                                    coordinates.z,
                                    normalWorld);
    uint outputRecord = index * OUTPUT_RECORD_COUNT;
    Results[outputRecord] = float4(normalWorld, 0);
    Results[outputRecord + 1] = float4(value, 0);
    Results[outputRecord + 2] = IntersectRectangles(Inputs[inputRecord + RAY_ORIGIN_INPUT_RECORD].xyz,
                                                    Inputs[inputRecord + RAY_DIRECTION_INPUT_RECORD].xyz);
}
