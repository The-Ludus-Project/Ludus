#include "light_transformer_encoder_layout.h"
#include "light_transformer_layout.h"
#include "math.slang"
#include "raster_bindings.slang"

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, section 3.3, https://arxiv.org/abs/2510.18189,
// and Xiaoyang Wu et al., "Point Transformer V3: Simpler, Faster, Stronger",
// CVPR 2024, https://arxiv.org/abs/2312.10035. This independent binary32 inference
// kernel replaces vendor sparse-convolution, scatter and FlashAttention runtimes.
// Geometry-only indices are prepared by the host; every learned layer executes here.
LUDUS_SHADER_BINDING(0) StructuredBuffer<uint4> Dispatch LUDUS_SHADER_TEXTURE_REGISTER(0);
LUDUS_SHADER_BINDING(1) StructuredBuffer<float> Weights LUDUS_SHADER_TEXTURE_REGISTER(1);
LUDUS_SHADER_BINDING(2) StructuredBuffer<uint> Topology LUDUS_SHADER_TEXTURE_REGISTER(2);
LUDUS_SHADER_BINDING(3) RWStructuredBuffer<float> Tensors LUDUS_SHADER_STORAGE_REGISTER(3);

static const uint GROUP_WIDTH = LUDUS_LIGHT_ENCODER_GROUP_WIDTH;
static const uint MAX_CHANNELS = LUDUS_LIGHT_ENCODER_MAX_CHANNELS;
static const uint HEAD_CHANNELS = LUDUS_LIGHT_ENCODER_HEAD_CHANNELS;
static const uint NEIGHBORS = LUDUS_LIGHT_ENCODER_NEIGHBORS;
static const uint PACKET_WORD_CHANNELS = 4;
static const float LOWEST_FINITE = -3.402823466e38;
static const float GELU_INVERSE_ROOT_TWO = 0.7071067811865475;
groupshared float NeighborHidden[MAX_CHANNELS];
groupshared float NeighborMaximum[MAX_CHANNELS];

uint ReadDispatch(uint index)
{
    return Dispatch[index / PACKET_WORD_CHANNELS][index % PACKET_WORD_CHANNELS];
}
float ComputeActivation(float value, uint activation)
{
    if (activation == LUDUS_LIGHT_ENCODER_RELU)
        return max(value, 0);
    if (activation == LUDUS_LIGHT_ENCODER_GELU)
    {
        return value * 0.5 * (1 + ComputeErrorFunction(value * GELU_INVERSE_ROOT_TWO));
    }
    return value;
}

[shader("compute")][numthreads(GROUP_WIDTH, 1, 1)] void ComputeMain(uint3 groupId : SV_GroupID,
                                                                    uint lane : SV_GroupIndex) {
    uint kind = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_KIND);
    uint point = groupId.x + groupId.y * ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_GROUPS_X);
    uint count = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_COUNT);
    if (point >= count)
        return;
    uint inputs = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_INPUTS);
    uint outputs = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_OUTPUTS);
    uint source = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_SOURCE);
    uint destination = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_DESTINATION);
    uint weights = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_WEIGHTS);
    uint auxiliaryWeights = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_AUXILIARY_WEIGHTS);
    uint indices = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_INDICES);
    uint pairs = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_PAIRS);
    uint other = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_OTHER);
    uint activation = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_ACTIVATION);
    uint parameter = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_PARAMETER);
    if (kind == LUDUS_LIGHT_ENCODER_INPUT)
    {
        for (uint channel = lane; channel < outputs; channel += GROUP_WIDTH)
            Tensors[destination + point * outputs + channel] = asfloat(Topology[indices + point * outputs + channel]);
        return;
    }
    if (kind == LUDUS_LIGHT_ENCODER_PACK)
    {
        // Header/tree/positions are host-prepared geometry, never neural features.
        if (point == 0)
            for (uint channel = lane; channel < parameter; channel += GROUP_WIDTH)
                Tensors[channel] = asfloat(Topology[indices + channel]);
        uint decoderOffset = ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_DECODER_DESTINATION);
        if (point == 0 && decoderOffset != 0)
            for (uint channel = lane; channel < LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT; channel += GROUP_WIDTH)
                Tensors[decoderOffset + channel] =
                    asfloat(Topology[ReadDispatch(LUDUS_LIGHT_ENCODER_DISPATCH_DECODER_SOURCE) + channel]);
        uint anchor = point * LUDUS_LIGHT_ENCODER_ANCHOR_WORDS * PACKET_WORD_CHANNELS + destination;
        if (lane < PACKET_WORD_CHANNELS)
            Tensors[anchor + lane] = asfloat(Topology[pairs + point * PACKET_WORD_CHANNELS + lane]);
        for (uint channel = lane; channel < outputs; channel += GROUP_WIDTH)
            Tensors[anchor + PACKET_WORD_CHANNELS + channel] = Tensors[source + point * outputs + channel];
        return;
    }
    if (kind == LUDUS_LIGHT_ENCODER_NEIGHBOR)
    {
        uint center = Topology[indices + point * (NEIGHBORS + 1) + NEIGHBORS];
        for (uint channel = lane; channel < outputs; channel += GROUP_WIDTH)
            NeighborMaximum[channel] = LOWEST_FINITE;
        GroupMemoryBarrierWithGroupSync();
        for (uint neighbor = 0; neighbor <= NEIGHBORS; ++neighbor)
        {
            uint index = Topology[indices + point * (NEIGHBORS + 1) + neighbor];
            for (uint channel = lane; channel < outputs; channel += GROUP_WIDTH)
            {
                float value = Weights[weights + inputs * outputs + channel];
                for (uint input = 0; input < inputs; ++input)
                {
                    float feature = Tensors[source + index * inputs + input];
                    if (neighbor < NEIGHBORS)
                        feature -= Tensors[source + center * inputs + input];
                    value += Weights[weights + channel * inputs + input] * feature;
                }
                NeighborHidden[channel] = max(value, 0);
            }
            GroupMemoryBarrierWithGroupSync();
            for (uint channel = lane; channel < outputs; channel += GROUP_WIDTH)
            {
                float value = Weights[auxiliaryWeights + outputs * outputs + channel];
                for (uint input = 0; input < outputs; ++input)
                    value += Weights[auxiliaryWeights + channel * outputs + input] * NeighborHidden[input];
                NeighborMaximum[channel] = max(NeighborMaximum[channel], max(value, 0));
            }
            GroupMemoryBarrierWithGroupSync();
        }
        for (uint channel = lane; channel < outputs; channel += GROUP_WIDTH)
            Tensors[destination + point * outputs + channel] = NeighborMaximum[channel];
        return;
    }
    if (kind == LUDUS_LIGHT_ENCODER_ATTENTION)
    {
        // A lane owns one complete head. Online softmax avoids a patch-squared
        // score buffer, subgroup-size assumptions and global floating atomics.
        uint heads = parameter;
        if (lane >= heads)
            return;
        uint width = inputs / heads;
        uint channelBegin = lane * width;
        float maximum = LOWEST_FINITE;
        float denominator = 0;
        float aggregate[HEAD_CHANNELS];
        for (uint channel = 0; channel < width; ++channel)
            aggregate[channel] = 0;
        uint begin = Topology[indices + point * 2];
        uint neighbors = Topology[indices + point * 2 + 1];
        for (uint neighbor = 0; neighbor < neighbors; ++neighbor)
        {
            uint index = Topology[indices + begin + neighbor];
            float score = 0;
            for (uint channel = 0; channel < width; ++channel)
                score += Tensors[source + point * inputs * 3 + channelBegin + channel] *
                         Tensors[source + index * inputs * 3 + inputs + channelBegin + channel];
            score *= rsqrt(float(width));
            float nextMaximum = max(maximum, score);
            float previousWeight = exp(maximum - nextMaximum);
            float weight = exp(score - nextMaximum);
            denominator = denominator * previousWeight + weight;
            for (uint channel = 0; channel < width; ++channel)
                aggregate[channel] =
                    aggregate[channel] * previousWeight +
                    weight * Tensors[source + index * inputs * 3 + inputs * 2 + channelBegin + channel];
            maximum = nextMaximum;
        }
        for (uint channel = 0; channel < width; ++channel)
            Tensors[destination + point * outputs + channelBegin + channel] = aggregate[channel] / denominator;
        return;
    }
    float mean = 0;
    float inverseDeviation = 0;
    if (kind == LUDUS_LIGHT_ENCODER_NORMALIZE)
    {
        for (uint input = 0; input < inputs; ++input)
            mean += Tensors[source + point * inputs + input];
        mean /= inputs;
        float variance = 0;
        for (uint input = 0; input < inputs; ++input)
        {
            float difference = Tensors[source + point * inputs + input] - mean;
            variance += difference * difference;
        }
        inverseDeviation = rsqrt(variance / inputs + LUDUS_LIGHT_ENCODER_NORM_EPSILON);
    }
    for (uint channel = lane; channel < outputs; channel += GROUP_WIDTH)
    {
        float value = 0;
        if (kind == LUDUS_LIGHT_ENCODER_AFFINE)
        {
            value = Weights[weights + inputs * outputs + channel];
            for (uint input = 0; input < inputs; ++input)
                value += Weights[weights + channel * inputs + input] * Tensors[source + point * inputs + input];
        }
        else if (kind == LUDUS_LIGHT_ENCODER_CONVOLUTION)
        {
            value = Weights[weights + parameter * inputs * outputs + channel];
            uint begin = Topology[indices + point];
            uint end = Topology[indices + point + 1];
            for (uint neighbor = begin; neighbor < end; ++neighbor)
            {
                uint index = Topology[pairs + neighbor * 2];
                uint kernel = Topology[pairs + neighbor * 2 + 1];
                for (uint input = 0; input < inputs; ++input)
                    value += Weights[weights + (channel * parameter + kernel) * inputs + input] *
                             Tensors[source + index * inputs + input];
            }
        }
        else if (kind == LUDUS_LIGHT_ENCODER_NORMALIZE)
            value =
                (Tensors[source + point * inputs + channel] - mean) * inverseDeviation * Weights[weights + channel] +
                Weights[weights + inputs + channel];
        else if (kind == LUDUS_LIGHT_ENCODER_ADD)
            value = Tensors[source + point * inputs + channel] + Tensors[other + point * inputs + channel];
        else if (kind == LUDUS_LIGHT_ENCODER_GATHER)
            value = Tensors[source + Topology[indices + point] * inputs + channel] +
                    Tensors[other + point * inputs + channel];
        else if (kind == LUDUS_LIGHT_ENCODER_POOL)
        {
            value = LOWEST_FINITE;
            uint begin = Topology[indices + point];
            uint end = Topology[indices + point + 1];
            for (uint neighbor = begin; neighbor < end; ++neighbor)
            {
                uint index = Topology[pairs + neighbor];
                float projected = Weights[weights + inputs * outputs + channel];
                for (uint input = 0; input < inputs; ++input)
                    projected += Weights[weights + channel * inputs + input] * Tensors[source + index * inputs + input];
                value = max(value, projected);
            }
            uint normalization = weights + (inputs + 1) * outputs;
            value = value * Weights[normalization + channel] + Weights[normalization + outputs + channel];
        }
        Tensors[destination + point * outputs + channel] = ComputeActivation(value, activation);
    }
}
