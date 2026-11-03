#pragma once
#include "purelight_weights.slang"
// Portable inference after Figueiredo et al., PureLight (SIGGRAPH Asia 2026), section 3.4,
// https://arxiv.org/abs/2606.04319. Thanks to the authors for the flow and cap construction.
// This independent implementation uses FP32 loops, explicit biases, a sparse zero-initialized dense grid,
// and a rationalized inverse CDF. It does not use the released CUDA checkpoint packing. See NOTICE.txt.

static const uint PURELIGHT_BIN_COUNT = 32;
static const uint PURELIGHT_HEIGHT_COUNT = PURELIGHT_BIN_COUNT + 1;
static const uint PURELIGHT_PARAMETER_COUNT = PURELIGHT_BIN_COUNT + PURELIGHT_HEIGHT_COUNT;
static const uint PURELIGHT_HIDDEN_WIDTH = 128;
static const uint PURELIGHT_HIDDEN_COUNT = 5;
static const uint PURELIGHT_NETWORK_LAYER_COUNT = PURELIGHT_HIDDEN_COUNT + 1;
static const uint PURELIGHT_COUPLING_COUNT = 4;
static const uint PURELIGHT_GRID_LEVEL_COUNT = 8;
static const uint PURELIGHT_GRID_FEATURE_COUNT = 8;
static const uint PURELIGHT_GRID_CORNER_COUNT = 8;
static const uint PURELIGHT_GRID_KEY_STRIDE = 16777216;
static const uint PURELIGHT_GRID_ENCODING_COUNT = PURELIGHT_GRID_LEVEL_COUNT * PURELIGHT_GRID_FEATURE_COUNT;
static const uint PURELIGHT_CONDITION_COUNT = PURELIGHT_GRID_ENCODING_COUNT + PURELIGHT_BIN_COUNT;
static const uint PURELIGHT_INPUT_COUNT = PURELIGHT_CONDITION_COUNT + PURELIGHT_BIN_COUNT;
static const float PURELIGHT_GRID_BASE_RESOLUTION = 8;
static const float PURELIGHT_GRID_LEVEL_SCALE = 1.405;
static const float PURELIGHT_QUARTIC_NORMALIZATION = 15.0 / 16;
static const float PURELIGHT_QUARTIC_CUBIC_FACTOR = 2.0 / 3;
static const float PURELIGHT_QUARTIC_QUINTIC_FACTOR = 0.2;
static const float PURELIGHT_SMOOTHSTEP_FACTOR = 3;
static const float PURELIGHT_KEY_SIDE_THRESHOLD = 0.999;
static const float PURELIGHT_CAP_MAXIMUM_DISTANCE_MULTIPLIER = 15;
static const float PURELIGHT_TANH_SATURATION = 10;
static const float PURELIGHT_QUARTER_PI = PI * 0.5 * 0.5;

float EvaluatePureLightQuarticCdf(float offset)
{
    float value = clamp(offset * float(PURELIGHT_BIN_COUNT), -1, 1);
    float squared = value * value;
    return 0.5 +
           PURELIGHT_QUARTIC_NORMALIZATION * value *
               (1 - PURELIGHT_QUARTIC_CUBIC_FACTOR * squared + PURELIGHT_QUARTIC_QUINTIC_FACTOR * squared * squared);
}

float EvaluatePureLightBlob(float value, uint bin)
{
    float left = float(bin) / float(PURELIGHT_BIN_COUNT) - value;
    float right = float(bin + 1) / float(PURELIGHT_BIN_COUNT) - value;
    return EvaluatePureLightQuarticCdf(right) + EvaluatePureLightQuarticCdf(right - 1) +
           EvaluatePureLightQuarticCdf(right + 1) - EvaluatePureLightQuarticCdf(left) -
           EvaluatePureLightQuarticCdf(left - 1) - EvaluatePureLightQuarticCdf(left + 1);
}

float LoadPureLightGridFeature(uint key, uint feature)
{
    uint low = 0;
    uint high = PURELIGHT_GRID_NODE_COUNT;
    [loop] while (low < high)
    {
        uint middle = low + (high - low) / 2;
        if (PURELIGHT_GRID_KEYS[middle] < key)
            low = middle + 1;
        else
            high = middle;
    }
    if (low >= PURELIGHT_GRID_NODE_COUNT || PURELIGHT_GRID_KEYS[low] != key)
        return 0;
    return GetPureLightModelValue(low * PURELIGHT_GRID_FEATURE_COUNT + feature);
}

/// The controlled room's panel (benchmark.py CENTER and HALF_EXTENT) with the trained profile, for the headless
/// kernels. The exported graph reads the World's luminaire instead; author it at this placement.
Luminaire MakeRoomLuminaire()
{
    Luminaire luminaire;
    luminaire.CenterWorld = float3(0, 0.97, 0);
    luminaire.HalfExtentWorld = 0.35;
    luminaire.TangentWorld = float3(1, 0, 0);
    luminaire.BitangentWorld = float3(0, 0, 1);
    luminaire.NormalWorld = float3(0, -1, 0);
    luminaire.RadianceScale = PURELIGHT_PROFILE_RADIANCE_SCALE;
    luminaire.BeamWidth = PURELIGHT_PROFILE_BEAM_WIDTH;
    luminaire.BeamSteering = PURELIGHT_PROFILE_BEAM_STEERING;
    luminaire.BeamSpread = PURELIGHT_PROFILE_BEAM_SPREAD;
    luminaire.BeamTwist = PURELIGHT_PROFILE_BEAM_TWIST;
    return luminaire;
}

void EncodePureLightCondition(Luminaire luminaire,
                              float3 positionWorld,
                              float thetaCap,
                              out float outEncoding[PURELIGHT_CONDITION_COUNT])
{
    float maximumDistanceWorld = PURELIGHT_CAP_MAXIMUM_DISTANCE_MULTIPLIER * PURELIGHT_RADIUS_WORLD;
    float3 normalizedPosition = ((positionWorld - luminaire.CenterWorld) / maximumDistanceWorld + 1) * 0.5;
    [loop] for (uint level = 0; level < PURELIGHT_GRID_LEVEL_COUNT; ++level)
    {
        float scale = PURELIGHT_GRID_BASE_RESOLUTION * pow(PURELIGHT_GRID_LEVEL_SCALE, float(level)) - 1;
        uint resolution = uint(ceil(scale)) + 1;
        float3 coordinate = normalizedPosition * scale + 0.5;
        uint3 lower = uint3(floor(coordinate));
        float3 fraction = coordinate - floor(coordinate);
        fraction = fraction * fraction * (PURELIGHT_SMOOTHSTEP_FACTOR - 2 * fraction);
        [loop] for (uint feature = 0; feature < PURELIGHT_GRID_FEATURE_COUNT; ++feature)
        {
            float value = 0;
            [loop] for (uint corner = 0; corner < PURELIGHT_GRID_CORNER_COUNT; ++corner)
            {
                uint3 bits = uint3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
                uint3 vertex = lower + bits;
                float3 weight = lerp(1 - fraction, fraction, float3(bits));
                uint index = (vertex.x + resolution * (vertex.y + resolution * vertex.z)) %
                             (resolution * resolution * resolution);
                uint key = level * PURELIGHT_GRID_KEY_STRIDE + index;
                value += weight.x * weight.y * weight.z * LoadPureLightGridFeature(key, feature);
            }
            outEncoding[level * PURELIGHT_GRID_FEATURE_COUNT + feature] = value;
        }
    }
    float thetaNormalized = (thetaCap / (PI * 0.5) + 1) * 0.5;
    [loop] for (uint bin = 0; bin < PURELIGHT_BIN_COUNT; ++bin) outEncoding[PURELIGHT_GRID_ENCODING_COUNT + bin] =
        EvaluatePureLightBlob(thetaNormalized, bin);
}

void EvaluatePureLightNetwork(uint coupling,
                              float value,
                              in float encoding[PURELIGHT_CONDITION_COUNT],
                              out float outParameters[PURELIGHT_PARAMETER_COUNT])
{
    float current[PURELIGHT_HIDDEN_WIDTH];
    float next[PURELIGHT_HIDDEN_WIDTH];
    [loop] for (uint inputIndex = 0; inputIndex < PURELIGHT_INPUT_COUNT; ++inputIndex) current[inputIndex] =
        inputIndex < PURELIGHT_BIN_COUNT ? EvaluatePureLightBlob(value, inputIndex)
                                         : encoding[inputIndex - PURELIGHT_BIN_COUNT];
    [loop] for (uint layer = 0; layer < PURELIGHT_NETWORK_LAYER_COUNT; ++layer)
    {
        uint layerIndex = coupling * PURELIGHT_NETWORK_LAYER_COUNT + layer;
        uint offset = PURELIGHT_NETWORK_OFFSETS[layerIndex];
        uint outputCount = layer == PURELIGHT_HIDDEN_COUNT ? PURELIGHT_PARAMETER_COUNT : PURELIGHT_HIDDEN_WIDTH;
        uint inputCount = PURELIGHT_HIDDEN_WIDTH;
        [loop] for (uint outputIndex = 0; outputIndex < outputCount; ++outputIndex)
        {
            float sum = GetPureLightModelValue(offset + outputCount * inputCount + outputIndex);
            [loop] for (uint inputIndex = 0; inputIndex < inputCount; ++inputIndex) sum +=
                current[inputIndex] * GetPureLightModelValue(offset + outputIndex * inputCount + inputIndex);
            next[outputIndex] = layer == PURELIGHT_HIDDEN_COUNT
                                    ? tanh(clamp(sum, -PURELIGHT_TANH_SATURATION, PURELIGHT_TANH_SATURATION))
                                    : max(sum, 0);
        }
        [loop] for (uint outputIndex = 0; outputIndex < outputCount; ++outputIndex) current[outputIndex] =
            next[outputIndex];
    }
    [loop] for (uint outputIndex = 0; outputIndex < PURELIGHT_PARAMETER_COUNT; ++outputIndex)
        outParameters[outputIndex] = current[outputIndex];
}

float TransformPureLightSpline(float value,
                               in float parameters[PURELIGHT_PARAMETER_COUNT],
                               bool isInverse,
                               out float outLogDensity)
{
    float widths[PURELIGHT_BIN_COUNT];
    float heights[PURELIGHT_HEIGHT_COUNT];
    float widthSum = 0;
    [loop] for (uint bin = 0; bin < PURELIGHT_BIN_COUNT; ++bin)
    {
        widths[bin] = exp(parameters[PURELIGHT_HEIGHT_COUNT + bin]);
        widthSum += widths[bin];
    }
    [loop] for (uint bin = 0; bin < PURELIGHT_HEIGHT_COUNT; ++bin) heights[bin] = exp(parameters[bin]);
    float massSum = 0;
    [loop] for (uint bin = 0; bin < PURELIGHT_BIN_COUNT; ++bin)
    {
        widths[bin] /= widthSum;
        massSum += 0.5 * (heights[bin] + heights[bin + 1]) * widths[bin];
    }
    float position = 0;
    float cumulative = 0;
    outLogDensity = 0;
    [loop] for (uint bin = 0; bin < PURELIGHT_BIN_COUNT; ++bin)
    {
        float left = heights[bin] / massSum;
        float delta = (heights[bin + 1] - heights[bin]) / massSum;
        float width = widths[bin];
        float mass = width * (left + 0.5 * delta);
        float upper = isInverse == true ? cumulative + mass : position + width;
        if (value < upper || bin + 1 == PURELIGHT_BIN_COUNT)
        {
            float fraction;
            if (isInverse == true)
            {
                float massOffset = (value - cumulative) / width;
                fraction = saturate(2 * massOffset / (left + sqrt(max(left * left + 2 * delta * massOffset, 0))));
            }
            else
                fraction = (value - position) / width;
            outLogDensity = log(left + delta * fraction);
            return isInverse == true ? position + width * fraction
                                     : cumulative + width * (left * fraction + 0.5 * delta * fraction * fraction);
        }
        position += width;
        cumulative += mass;
    }
    return value;
}

float2 TransformPureLightFlow(Luminaire luminaire,
                              float3 positionWorld,
                              float thetaCap,
                              float2 value,
                              bool isInverse,
                              out float outLogDensity)
{
    float encoding[PURELIGHT_CONDITION_COUNT];
    EncodePureLightCondition(luminaire, positionWorld, thetaCap, encoding);
    outLogDensity = 0;
    [loop] for (uint step = 0; step < PURELIGHT_COUPLING_COUNT; ++step)
    {
        uint coupling = isInverse == true ? PURELIGHT_COUPLING_COUNT - 1 - step : step;
        uint offIndex = coupling % 2;
        uint onIndex = 1 - offIndex;
        float parameters[PURELIGHT_PARAMETER_COUNT];
        EvaluatePureLightNetwork(coupling, value[offIndex], encoding, parameters);
        float logDensity;
        value[onIndex] = TransformPureLightSpline(value[onIndex], parameters, isInverse, logDensity);
        outLogDensity += logDensity;
    }
    return value;
}

float3 SamplePureLightCap(Luminaire luminaire, float3 positionWorld, float2 seed, out float outDensitySolidAngle)
{
    float3 offsetWorld = luminaire.CenterWorld - positionWorld;
    float distanceWorld = length(offsetWorld);
    float cosineCap = sqrt(1 - PURELIGHT_RADIUS_WORLD * PURELIGHT_RADIUS_WORLD / (distanceWorld * distanceWorld));
    float thetaCap = acos(cosineCap);
    float logDensity;
    float2 value = TransformPureLightFlow(luminaire, positionWorld, thetaCap, seed, true, logDensity);
    float2 square = 2 * value - 1;
    float radius;
    float angle;
    if (square.x == 0 && square.y == 0)
    {
        radius = 0;
        angle = 0;
    }
    else if (abs(square.x) > abs(square.y))
    {
        radius = square.x;
        angle = PURELIGHT_QUARTER_PI * square.y / square.x;
    }
    else
    {
        radius = square.y;
        angle = PI * 0.5 - PURELIGHT_QUARTER_PI * square.x / square.y;
    }
    float2 disk = radius * float2(cos(angle), sin(angle));
    float difference = 1 - cosineCap;
    float cosine = 1 - radius * radius * difference;
    float sineOverRadius = sqrt(difference * (2 - radius * radius * difference));
    float3 axisWorld = offsetWorld / distanceWorld;
    float3 helperWorld = abs(axisWorld.z) < PURELIGHT_KEY_SIDE_THRESHOLD ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangentWorld = normalize(cross(helperWorld, axisWorld));
    float3 bitangentWorld = cross(axisWorld, tangentWorld);
    outDensitySolidAngle = exp(logDensity) / (2 * PI * difference);
    return sineOverRadius * (disk.x * tangentWorld + disk.y * bitangentWorld) + cosine * axisWorld;
}

LuminaireLightSample SamplePortablePureLight(Luminaire luminaire, LuminaireReceiver receiver, float2 seed)
{
    float distanceToCenterWorld = length(receiver.PositionWorld - luminaire.CenterWorld);
    if (receiver.IsLit == false || distanceToCenterWorld <= PURELIGHT_RADIUS_WORLD ||
        distanceToCenterWorld > PURELIGHT_CAP_MAXIMUM_DISTANCE_MULTIPLIER * PURELIGHT_RADIUS_WORLD)
        return (LuminaireLightSample)0;
    float densitySolidAngle;
    float3 directionWorld = SamplePureLightCap(luminaire, receiver.PositionWorld, seed, densitySolidAngle);
    float cosine = -dot(directionWorld, luminaire.NormalWorld);
    if (cosine <= 0)
        return (LuminaireLightSample)0;
    float2 slopePanel =
        -float2(dot(directionWorld, luminaire.TangentWorld), dot(directionWorld, luminaire.BitangentWorld)) / cosine;
    float2 positionPanel = receiver.PositionPanelTangent - receiver.HeightPanel * slopePanel;
    uint tileIndex = FindLuminaireTile(positionPanel);
    if (tileIndex == LUMINAIRE_INVALID_TILE)
        return (LuminaireLightSample)0;
    LuminaireLightSample result = MakeLuminaireLightSample(luminaire, receiver, tileIndex, slopePanel);
    LuminaireTile tile = GetLuminaireTile(luminaire, tileIndex);
    float radianceDensity = EvaluateEmissionDensity(tile, ComputeStandardSeed(tile, positionPanel, slopePanel));
    result.RadianceOverDensity =
        luminaire.RadianceScale * tile.Strength * tile.Tint * radianceDensity / densitySolidAngle;
    return result;
}
