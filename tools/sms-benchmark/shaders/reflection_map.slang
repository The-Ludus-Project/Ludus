#include "affine_slice.slang"

static const float HALF_VECTOR_HEMISPHERE_MINIMUM = 1e-6;

struct DifferentiatedVector
{
    float3 ValueWorld;
    float3 DerivativeUWorld;
    float3 DerivativeVWorld;
};

DifferentiatedVector NormalizeDifferentiated(DifferentiatedVector value)
{
    float inverseLength = 1 / length(value.ValueWorld);
    float3 unit = value.ValueWorld * inverseLength;
    DifferentiatedVector result;
    result.ValueWorld = unit;
    result.DerivativeUWorld = (value.DerivativeUWorld - unit * dot(unit, value.DerivativeUWorld)) * inverseLength;
    result.DerivativeVWorld = (value.DerivativeVWorld - unit * dot(unit, value.DerivativeVWorld)) * inverseLength;
    return result;
}

DifferentiatedVector MakeDifferentiatedDirection(float3 endpointWorld, float3 positionWorld)
{
    DifferentiatedVector result;
    result.ValueWorld = endpointWorld - positionWorld;
    result.DerivativeUWorld = -SceneData[REFLECTOR_DERIVATIVE_U_RECORD].xyz;
    result.DerivativeVWorld = -SceneData[REFLECTOR_DERIVATIVE_V_RECORD].xyz;
    return NormalizeDifferentiated(result);
}

void EvaluateSlopeDerivatives(float2 coordinatesUv,
                              out float2 outSlope,
                              out float2 outDerivativeU,
                              out float2 outDerivativeV)
{
    uint resolution = uint(SceneData[BASE_TANGENT_RECORD].w);
    float2 rawPositionTexel = frac(coordinatesUv) * resolution - 0.5;
    float2 positionTexel = clamp(rawPositionTexel, 0, float(resolution - 1));
    uint2 lowerTexel = uint2(floor(positionTexel));
    uint2 upperTexel = min(lowerTexel + 1, resolution - 1);
    float2 weight = positionTexel - lowerTexel;
    float2 a = Slopes[lowerTexel.y * resolution + lowerTexel.x].xy;
    float2 b = Slopes[lowerTexel.y * resolution + upperTexel.x].xy;
    float2 c = Slopes[upperTexel.y * resolution + lowerTexel.x].xy;
    float2 d = Slopes[upperTexel.y * resolution + upperTexel.x].xy;
    outSlope = lerp(lerp(a, b, weight.x), lerp(c, d, weight.x), weight.y);
    outDerivativeU = lerp(b - a, d - c, weight.y) * resolution;
    outDerivativeV = lerp(c - a, d - b, weight.x) * resolution;
    if (rawPositionTexel.x <= 0 || rawPositionTexel.x >= float(resolution - 1))
        outDerivativeU = float2(0);
    if (rawPositionTexel.y <= 0 || rawPositionTexel.y >= float(resolution - 1))
        outDerivativeV = float2(0);
}

struct ReflectionMap
{
    float2 Residual;
    float2 DerivativeU;
    float2 DerivativeV;
    bool IsValid;
};

ReflectionMap EvaluateReflectionMap(float3 receiverWorld, float3 lightWorld, float2 coordinatesUv, bool isSmoothed)
{
    float2 slope;
    float2 slopeDerivativeU;
    float2 slopeDerivativeV;
    EvaluateSlopeDerivatives(coordinatesUv, slope, slopeDerivativeU, slopeDerivativeV);
    if (isSmoothed == true)
    {
        slope = SceneData[SMOOTH_SLOPE_RECORD].xy;
        slopeDerivativeU = float2(0);
        slopeDerivativeV = float2(0);
    }
    float3 baseTangentWorld = SceneData[BASE_TANGENT_RECORD].xyz;
    float3 baseBitangentWorld = SceneData[BASE_BITANGENT_RECORD].xyz;
    DifferentiatedVector normal;
    normal.ValueWorld = SceneData[BASE_NORMAL_RECORD].xyz - slope.x * baseTangentWorld - slope.y * baseBitangentWorld;
    normal.DerivativeUWorld = -slopeDerivativeU.x * baseTangentWorld - slopeDerivativeU.y * baseBitangentWorld;
    normal.DerivativeVWorld = -slopeDerivativeV.x * baseTangentWorld - slopeDerivativeV.y * baseBitangentWorld;
    normal = NormalizeDifferentiated(normal);
    DifferentiatedVector tangent;
    tangent.ValueWorld = baseTangentWorld - normal.ValueWorld * dot(normal.ValueWorld, baseTangentWorld);
    tangent.DerivativeUWorld = -normal.DerivativeUWorld * dot(normal.ValueWorld, baseTangentWorld) -
                               normal.ValueWorld * dot(normal.DerivativeUWorld, baseTangentWorld);
    tangent.DerivativeVWorld = -normal.DerivativeVWorld * dot(normal.ValueWorld, baseTangentWorld) -
                               normal.ValueWorld * dot(normal.DerivativeVWorld, baseTangentWorld);
    tangent = NormalizeDifferentiated(tangent);
    DifferentiatedVector bitangent;
    bitangent.ValueWorld = cross(normal.ValueWorld, tangent.ValueWorld);
    bitangent.DerivativeUWorld =
        cross(normal.DerivativeUWorld, tangent.ValueWorld) + cross(normal.ValueWorld, tangent.DerivativeUWorld);
    bitangent.DerivativeVWorld =
        cross(normal.DerivativeVWorld, tangent.ValueWorld) + cross(normal.ValueWorld, tangent.DerivativeVWorld);
    float3 positionWorld = SceneData[REFLECTOR_CENTER_RECORD].xyz +
                           (coordinatesUv.x - 0.5) * SceneData[REFLECTOR_DERIVATIVE_U_RECORD].xyz +
                           (coordinatesUv.y - 0.5) * SceneData[REFLECTOR_DERIVATIVE_V_RECORD].xyz;
    DifferentiatedVector view = MakeDifferentiatedDirection(receiverWorld, positionWorld);
    DifferentiatedVector light = MakeDifferentiatedDirection(lightWorld, positionWorld);
    DifferentiatedVector halfway;
    halfway.ValueWorld = view.ValueWorld + light.ValueWorld;
    halfway.DerivativeUWorld = view.DerivativeUWorld + light.DerivativeUWorld;
    halfway.DerivativeVWorld = view.DerivativeVWorld + light.DerivativeVWorld;
    halfway = NormalizeDifferentiated(halfway);
    float denominator = dot(halfway.ValueWorld, normal.ValueWorld);
    ReflectionMap result;
    result.Residual = float2(0);
    result.DerivativeU = float2(0);
    result.DerivativeV = float2(0);
    result.IsValid = denominator > HALF_VECTOR_HEMISPHERE_MINIMUM;
    if (result.IsValid == false)
        return result;
    result.Residual =
        float2(dot(halfway.ValueWorld, tangent.ValueWorld), dot(halfway.ValueWorld, bitangent.ValueWorld)) /
        denominator;
    float2 numeratorDerivativeU = float2(
        dot(halfway.DerivativeUWorld, tangent.ValueWorld) + dot(halfway.ValueWorld, tangent.DerivativeUWorld),
        dot(halfway.DerivativeUWorld, bitangent.ValueWorld) + dot(halfway.ValueWorld, bitangent.DerivativeUWorld));
    float2 numeratorDerivativeV = float2(
        dot(halfway.DerivativeVWorld, tangent.ValueWorld) + dot(halfway.ValueWorld, tangent.DerivativeVWorld),
        dot(halfway.DerivativeVWorld, bitangent.ValueWorld) + dot(halfway.ValueWorld, bitangent.DerivativeVWorld));
    float denominatorDerivativeU =
        dot(halfway.DerivativeUWorld, normal.ValueWorld) + dot(halfway.ValueWorld, normal.DerivativeUWorld);
    float denominatorDerivativeV =
        dot(halfway.DerivativeVWorld, normal.ValueWorld) + dot(halfway.ValueWorld, normal.DerivativeVWorld);
    result.DerivativeU = (numeratorDerivativeU - result.Residual * denominatorDerivativeU) / denominator;
    result.DerivativeV = (numeratorDerivativeV - result.Residual * denominatorDerivativeV) / denominator;
    return result;
}

float3
EvaluateConnection(float3 receiverWorld, float3 color, float2 coordinatesUv, float2 lightCoordinatesUv, float alpha)
{
    float3 positionWorld = SceneData[REFLECTOR_CENTER_RECORD].xyz +
                           (coordinatesUv.x - 0.5) * SceneData[REFLECTOR_DERIVATIVE_U_RECORD].xyz +
                           (coordinatesUv.y - 0.5) * SceneData[REFLECTOR_DERIVATIVE_V_RECORD].xyz;
    float3 lightPositionWorld = SceneData[LIGHT_CENTER_RECORD].xyz +
                                (lightCoordinatesUv.x - 0.5) * SceneData[LIGHT_DERIVATIVE_U_RECORD].xyz +
                                (lightCoordinatesUv.y - 0.5) * SceneData[LIGHT_DERIVATIVE_V_RECORD].xyz;
    float3 toReceiverWorld = receiverWorld - positionWorld;
    float3 toLightWorld = lightPositionWorld - positionWorld;
    float3 viewWorld = NormalizeOrZero(toReceiverWorld);
    float3 lightWorld = NormalizeOrZero(toLightWorld);
    float floorCosine = max(0, dot(SceneData[FLOOR_NORMAL_RECORD].xyz, -viewWorld));
    float reflectorCosine = max(0, dot(SceneData[BASE_NORMAL_RECORD].xyz, viewWorld));
    float emitterCosine = max(0, dot(SceneData[LIGHT_NORMAL_AREA_RECORD].xyz, -lightWorld));
    float solidAngleUv = SceneData[AREA_RADIANCE_RECORD].x * reflectorCosine / dot(toReceiverWorld, toReceiverWorld);
    float emitted = SceneData[AREA_RADIANCE_RECORD].y * SceneData[LIGHT_NORMAL_AREA_RECORD].w * emitterCosine /
                    dot(toLightWorld, toLightWorld);
    float3 normalWorld;
    return color / PI * EvaluateBeckmann(coordinatesUv, viewWorld, lightWorld, alpha, normalWorld) *
           (floorCosine * solidAngleUv * emitted);
}

FIGURE17_PARITY_ENTRY
void ValidateFigure17Transport(uint3 threadIndex : SV_DispatchThreadID)
{
    uint count;
    uint stride;
    Inputs.GetDimensions(count, stride);
    uint index = threadIndex.x;
    if (index >= count / INPUT_RECORD_COUNT)
        return;
    uint record = index * INPUT_RECORD_COUNT;
    float4 first = Inputs[record];
    float3 receiverWorld = Inputs[record + 1].xyz;
    float2 lightCoordinatesUv = Inputs[record + LIGHT_INPUT_RECORD].xy;
    float3 lightWorld = SceneData[LIGHT_CENTER_RECORD].xyz +
                        (lightCoordinatesUv.x - 0.5) * SceneData[LIGHT_DERIVATIVE_U_RECORD].xyz +
                        (lightCoordinatesUv.y - 0.5) * SceneData[LIGHT_DERIVATIVE_V_RECORD].xyz;
    ReflectionMap map = EvaluateReflectionMap(receiverWorld, lightWorld, first.xy, false);
    uint outputRecord = index * OUTPUT_RECORD_COUNT;
    Results[outputRecord] = float4(map.Residual, map.DerivativeU);
    Results[outputRecord + 1] = float4(map.DerivativeV, map.IsValid == true ? 1 : 0, 0);
    Results[outputRecord + 2] = float4(EvaluateConnection(receiverWorld,
                                                          Inputs[record + RAY_ORIGIN_INPUT_RECORD].xyz,
                                                          first.xy,
                                                          lightCoordinatesUv,
                                                          first.z),
                                       0);
}
