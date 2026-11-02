#include "light_transformer_layout.h"
#include "point_search_layout.h"
#include "raster_bindings.slang"

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, section 3.3, https://arxiv.org/abs/2510.18189.
// Independent inference implementation of the released query decoder, including
// its V(K(latent)) projection. Dropout is disabled; exported BatchNorm is folded.
// Metadata/tree and feature packets keep this decoder within four storage bindings.
LUDUS_SHADER_BINDING(0) StructuredBuffer<uint4> Nodes;
LUDUS_SHADER_BINDING(1) StructuredBuffer<float> Weights;
LUDUS_SHADER_BINDING(2) StructuredBuffer<float4> Features;
LUDUS_SHADER_BINDING(3) RWStructuredBuffer<float4> Results;

uint4 ReadPointSearchNodeWord(uint index)
{
    return Nodes[index + 1];
}
#include "point_search.slang"

static const uint DIMENSION = LUDUS_LIGHT_TRANSFORMER_DIMENSION;
static const uint NEIGHBORS = LUDUS_LIGHT_TRANSFORMER_NEIGHBORS;
static const uint NEIGHBOR_TILE_SIZE = NEIGHBORS / 2;
static const uint ATTENTION_CHANNELS = LUDUS_LIGHT_TRANSFORMER_ATTENTION_CHANNELS;
static const uint FEED_FORWARD_CHANNELS = LUDUS_LIGHT_TRANSFORMER_FEED_FORWARD_CHANNELS;
static const uint WORD_CHANNELS = LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS;
static const uint POSITION_CHANNELS = LUDUS_LIGHT_TRANSFORMER_POSITION_CHANNELS;
static const uint ANCHOR_WORDS = 1 + DIMENSION / WORD_CHANNELS;
static const uint QUERY_WORDS = LUDUS_LIGHT_TRANSFORMER_QUERY_WORDS;
static const uint OUTPUT_CHANNELS = LUDUS_LIGHT_TRANSFORMER_OUTPUT_CHANNELS;
groupshared uint NeighborIndices[NEIGHBORS];
groupshared uint IsValidQuery;
groupshared float Query[DIMENSION];
groupshared float Temporary[DIMENSION];
groupshared float QueryProjection[DIMENSION];
// Tile keys to fit Vulkan's minimum 16 KiB workgroup-memory guarantee.
groupshared float Keys[NEIGHBOR_TILE_SIZE * DIMENSION];
groupshared float PositionHidden[NEIGHBORS * ATTENTION_CHANNELS];
groupshared float AttentionHidden[NEIGHBORS * ATTENTION_CHANNELS];
groupshared float Work[FEED_FORWARD_CHANNELS];
groupshared float QueryMean;
groupshared float QueryInverseDeviation;

float ReadAnchorFeature(uint anchor, uint channel)
{
    return Features[anchor * ANCHOR_WORDS + 1 + channel / WORD_CHANNELS][channel % WORD_CHANNELS];
}
float ReadQueryFeature(uint query, uint channel)
{
    return Features[Nodes[0].y * ANCHOR_WORDS + query * QUERY_WORDS + channel / POSITION_CHANNELS]
                   [channel % POSITION_CHANNELS];
}
float ReadPositionEmbedding(uint offset, uint neighbor, uint channel)
{
    float value = Weights[offset + ATTENTION_CHANNELS * DIMENSION + channel];
    for (uint i = 0; i < ATTENTION_CHANNELS; ++i)
        value += Weights[offset + channel * ATTENTION_CHANNELS + i] * PositionHidden[neighbor * ATTENTION_CHANNELS + i];
    return value;
}
void ComputeQueryStatistics(uint lane)
{
    if (lane == 0)
    {
        float mean = 0;
        for (uint i = 0; i < DIMENSION; ++i)
            mean += Query[i];
        mean /= DIMENSION;
        float variance = 0;
        for (uint i = 0; i < DIMENSION; ++i)
        {
            float difference = Query[i] - mean;
            variance += difference * difference;
        }
        QueryMean = mean;
        QueryInverseDeviation = rsqrt(variance / DIMENSION + LUDUS_LIGHT_TRANSFORMER_QUERY_NORM_EPSILON);
    }
    GroupMemoryBarrierWithGroupSync();
}

[shader("compute")][numthreads(DIMENSION, 1, 1)] void ComputeMain(uint3 groupId : SV_GroupID,
                                                                  uint lane : SV_GroupIndex) {
    uint4 parameters = Nodes[0];
    uint queryIndex = groupId.x + groupId.y * parameters.w;
    if (queryIndex >= parameters.x)
        return;
    float3 positionModel = Features[parameters.y * ANCHOR_WORDS + queryIndex * QUERY_WORDS].xyz;
    if (lane == 0)
    {
        PointNeighbor neighbors[LUDUS_MAX_POINT_NEIGHBORS];
        bool hasNeighbors = FindPointNeighbors(parameters.y, parameters.z, positionModel, NEIGHBORS, neighbors);
        IsValidQuery = hasNeighbors == true ? 1 : 0;
        for (uint i = 0; i < NEIGHBORS; ++i)
        {
            NeighborIndices[i] = hasNeighbors == true ? neighbors[i].PointIndex : LUDUS_ABSENT_POINT_NODE;
            if (NeighborIndices[i] >= parameters.y)
                IsValidQuery = 0;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (IsValidQuery == 0)
    {
        if (lane == 0)
            Results[queryIndex] = float4(asfloat(0x7fc00000),
                                         0,
                                         0,
                                         0); // ludus-style: magic-number IEEE quiet NaN signals invalid neighborhood
        return;
    }
    uint offset = 0;
    float value = Weights[LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS * DIMENSION + lane];
    for (uint i = 0; i < LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS; ++i)
        value += Weights[lane * LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS + i] * ReadQueryFeature(queryIndex, i);
    Query[lane] = max(value, 0);
    offset += LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS, DIMENSION);
    GroupMemoryBarrierWithGroupSync();
    value = Weights[offset + DIMENSION * DIMENSION + lane];
    for (uint i = 0; i < DIMENSION; ++i)
        value += Weights[offset + lane * DIMENSION + i] * Query[i];
    Temporary[lane] = max(value, 0);
    GroupMemoryBarrierWithGroupSync();
    Query[lane] = Temporary[lane];
    GroupMemoryBarrierWithGroupSync();

    for (uint block = 0; block < LUDUS_LIGHT_TRANSFORMER_DEPTH; ++block)
    {
        uint blockOffset = LUDUS_LIGHT_TRANSFORMER_PRE_QUERY_SIZE + block * LUDUS_LIGHT_TRANSFORMER_BLOCK_SIZE;
        uint attentionNorm = blockOffset;
        uint feedForwardNorm = attentionNorm + 2 * DIMENSION;
        uint queryWeights = feedForwardNorm + 2 * DIMENSION;
        uint keyWeights = queryWeights + DIMENSION * DIMENSION;
        uint valueWeights = keyWeights + DIMENSION * DIMENSION;
        uint positionFirst = valueWeights + DIMENSION * DIMENSION;
        uint positionSecond =
            positionFirst + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(POSITION_CHANNELS, ATTENTION_CHANNELS);
        uint attentionFirst = positionSecond + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(ATTENTION_CHANNELS, DIMENSION);
        uint attentionSecond = attentionFirst + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(DIMENSION, ATTENTION_CHANNELS);
        uint attentionOutput = attentionSecond + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(ATTENTION_CHANNELS, DIMENSION);
        uint feedForwardFirst = attentionOutput + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(DIMENSION, DIMENSION);
        uint feedForwardSecond =
            feedForwardFirst + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(DIMENSION, FEED_FORWARD_CHANNELS);

        ComputeQueryStatistics(lane);
        Temporary[lane] = (Query[lane] - QueryMean) * QueryInverseDeviation * Weights[attentionNorm + lane] +
                          Weights[attentionNorm + DIMENSION + lane];
        GroupMemoryBarrierWithGroupSync();
        value = 0;
        for (uint i = 0; i < DIMENSION; ++i)
            value += Weights[queryWeights + lane * DIMENSION + i] * Temporary[i];
        QueryProjection[lane] = value;
        for (uint index = lane; index < NEIGHBORS * ATTENTION_CHANNELS; index += DIMENSION)
        {
            uint neighbor = index / ATTENTION_CHANNELS;
            uint channel = index % ATTENTION_CHANNELS;
            float3 relativePositionModel = Features[NeighborIndices[neighbor] * ANCHOR_WORDS].xyz - positionModel;
            value = Weights[positionFirst + POSITION_CHANNELS * ATTENTION_CHANNELS + channel];
            for (uint i = 0; i < POSITION_CHANNELS; ++i)
                value += Weights[positionFirst + channel * POSITION_CHANNELS + i] * relativePositionModel[i];
            PositionHidden[index] = max(value, 0);
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint neighborBegin = 0; neighborBegin < NEIGHBORS; neighborBegin += NEIGHBOR_TILE_SIZE)
        {
            for (uint localNeighbor = 0; localNeighbor < NEIGHBOR_TILE_SIZE; ++localNeighbor)
            {
                value = 0;
                for (uint i = 0; i < DIMENSION; ++i)
                    value += Weights[keyWeights + lane * DIMENSION + i] *
                             ReadAnchorFeature(NeighborIndices[neighborBegin + localNeighbor], i);
                Keys[localNeighbor * DIMENSION + lane] = value;
            }
            GroupMemoryBarrierWithGroupSync();
            for (uint index = lane; index < NEIGHBOR_TILE_SIZE * ATTENTION_CHANNELS; index += DIMENSION)
            {
                uint localNeighbor = index / ATTENTION_CHANNELS;
                uint neighbor = neighborBegin + localNeighbor;
                uint channel = index % ATTENTION_CHANNELS;
                value = Weights[attentionFirst + DIMENSION * ATTENTION_CHANNELS + channel];
                for (uint i = 0; i < DIMENSION; ++i)
                {
                    float difference = Keys[localNeighbor * DIMENSION + i] - QueryProjection[i] +
                                       ReadPositionEmbedding(positionSecond, neighbor, i);
                    value += Weights[attentionFirst + channel * DIMENSION + i] * difference;
                }
                AttentionHidden[neighbor * ATTENTION_CHANNELS + channel] = max(value, 0);
            }
            GroupMemoryBarrierWithGroupSync();
        }
        float logits[NEIGHBORS];
        float maximum = -3.402823466e38; // ludus-style: magic-number lowest finite binary32 for stable softmax
        for (uint neighbor = 0; neighbor < NEIGHBORS; ++neighbor)
        {
            value = Weights[attentionSecond + ATTENTION_CHANNELS * DIMENSION + lane];
            for (uint i = 0; i < ATTENTION_CHANNELS; ++i)
                value += Weights[attentionSecond + lane * ATTENTION_CHANNELS + i] *
                         AttentionHidden[neighbor * ATTENTION_CHANNELS + i];
            logits[neighbor] = value;
            maximum = max(maximum, value);
        }
        float denominator = 0;
        for (uint neighbor = 0; neighbor < NEIGHBORS; ++neighbor)
        {
            logits[neighbor] = exp(logits[neighbor] - maximum);
            denominator += logits[neighbor];
        }
        float aggregate = 0;
        for (uint neighborBegin = 0; neighborBegin < NEIGHBORS; neighborBegin += NEIGHBOR_TILE_SIZE)
        {
            for (uint localNeighbor = 0; localNeighbor < NEIGHBOR_TILE_SIZE; ++localNeighbor)
            {
                value = 0;
                for (uint i = 0; i < DIMENSION; ++i)
                    value += Weights[keyWeights + lane * DIMENSION + i] *
                             ReadAnchorFeature(NeighborIndices[neighborBegin + localNeighbor], i);
                Keys[localNeighbor * DIMENSION + lane] = value;
            }
            GroupMemoryBarrierWithGroupSync();
            for (uint localNeighbor = 0; localNeighbor < NEIGHBOR_TILE_SIZE; ++localNeighbor)
            {
                uint neighbor = neighborBegin + localNeighbor;
                value = ReadPositionEmbedding(positionSecond, neighbor, lane);
                // The released decoder applies V to projected K, not raw latents.
                for (uint i = 0; i < DIMENSION; ++i)
                    value += Weights[valueWeights + lane * DIMENSION + i] * Keys[localNeighbor * DIMENSION + i];
                aggregate += value * (logits[neighbor] / denominator);
            }
            GroupMemoryBarrierWithGroupSync();
        }
        Work[lane] = aggregate;
        GroupMemoryBarrierWithGroupSync();
        value = Weights[attentionOutput + DIMENSION * DIMENSION + lane];
        for (uint i = 0; i < DIMENSION; ++i)
            value += Weights[attentionOutput + lane * DIMENSION + i] * Work[i];
        Query[lane] += value;
        GroupMemoryBarrierWithGroupSync();
        ComputeQueryStatistics(lane);
        Temporary[lane] = (Query[lane] - QueryMean) * QueryInverseDeviation * Weights[feedForwardNorm + lane] +
                          Weights[feedForwardNorm + DIMENSION + lane];
        GroupMemoryBarrierWithGroupSync();
        for (uint channel = lane; channel < FEED_FORWARD_CHANNELS; channel += DIMENSION)
        {
            value = Weights[feedForwardFirst + DIMENSION * FEED_FORWARD_CHANNELS + channel];
            for (uint i = 0; i < DIMENSION; ++i)
                value += Weights[feedForwardFirst + channel * DIMENSION + i] * Temporary[i];
            Work[channel] = max(value, 0);
        }
        GroupMemoryBarrierWithGroupSync();
        value = Weights[feedForwardSecond + FEED_FORWARD_CHANNELS * DIMENSION + lane];
        for (uint i = 0; i < FEED_FORWARD_CHANNELS; ++i)
            value += Weights[feedForwardSecond + lane * FEED_FORWARD_CHANNELS + i] * Work[i];
        Query[lane] += value;
        GroupMemoryBarrierWithGroupSync();
    }
    offset =
        LUDUS_LIGHT_TRANSFORMER_PRE_QUERY_SIZE + LUDUS_LIGHT_TRANSFORMER_DEPTH * LUDUS_LIGHT_TRANSFORMER_BLOCK_SIZE;
    if (lane == 0)
    {
        float3 prediction = 0;
        for (uint channel = 0; channel < OUTPUT_CHANNELS; ++channel)
        {
            value = Weights[offset + DIMENSION * OUTPUT_CHANNELS + channel];
            for (uint i = 0; i < DIMENSION; ++i)
                value += Weights[offset + channel * DIMENSION + i] * Query[i];
            prediction[channel] = value > LUDUS_LIGHT_TRANSFORMER_SOFTPLUS_THRESHOLD ? value : log(1 + exp(value));
        }
        Results[queryIndex] = float4(prediction, 0);
    }
}
