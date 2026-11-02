#ifndef LUDUS_LIGHT_TRANSFORMER_QUERY_INCLUDED
#    define LUDUS_LIGHT_TRANSFORMER_QUERY_INCLUDED 1
#    include "light_transformer_encoder_layout.h"
#    include "light_transformer_layout.h"
#    include "point_search_layout.h"

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, section 3.4, https://arxiv.org/abs/2510.18189.
// Independently evaluate the released decoder against graph-resident trained
// anchors. This per-query path fits the ordinary scene-compute callback ABI;
// the SDK's cooperative decoder recipe is available for batched query workloads.
static const uint LIGHT_QUERY_DIMENSION = LUDUS_LIGHT_TRANSFORMER_DIMENSION;
static const uint LIGHT_QUERY_NEIGHBORS = LUDUS_LIGHT_TRANSFORMER_NEIGHBORS;
static const uint LIGHT_QUERY_HIDDEN = LUDUS_LIGHT_TRANSFORMER_ATTENTION_CHANNELS;
static const uint LIGHT_QUERY_WIDE = LUDUS_LIGHT_TRANSFORMER_FEED_FORWARD_CHANNELS;
static const uint LIGHT_QUERY_WORD_CHANNELS = LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS;
static const uint LIGHT_QUERY_POSITION_CHANNELS = LUDUS_LIGHT_TRANSFORMER_POSITION_CHANNELS;
static const float LIGHT_QUERY_LOWEST = -3.402823466e38;
uint4 ReadPointSearchNodeWord(uint index)
{
    return GraphBuffer0[index + LUDUS_LIGHT_ENCODER_PACKET_HEADER_WORDS];
}
#    include "point_search.slang"

float ReadLightQueryWeight(uint index)
{
    uint first = GraphBuffer0[0].w + GraphBuffer0[0].y * LUDUS_LIGHT_ENCODER_ANCHOR_WORDS;
    return asfloat(GraphBuffer0[first + index / LIGHT_QUERY_WORD_CHANNELS])[index % LIGHT_QUERY_WORD_CHANNELS];
}
float ReadLightQueryAnchor(uint anchor, uint channel)
{
    uint first = GraphBuffer0[0].w + anchor * LUDUS_LIGHT_ENCODER_ANCHOR_WORDS + 1;
    return asfloat(GraphBuffer0[first + channel / LIGHT_QUERY_WORD_CHANNELS])[channel % LIGHT_QUERY_WORD_CHANNELS];
}
float ComputeLightQueryRow(uint offset, uint channel, uint inputs, in float values[LIGHT_QUERY_DIMENSION], bool hasBias)
{
    float value = hasBias == true ? ReadLightQueryWeight(offset + inputs * LIGHT_QUERY_DIMENSION + channel) : 0;
    for (uint input = 0; input < inputs; ++input)
        value += ReadLightQueryWeight(offset + channel * inputs + input) * values[input];
    return value;
}
void NormalizeLightQuery(in float values[LIGHT_QUERY_DIMENSION],
                         uint offset,
                         out float outValues[LIGHT_QUERY_DIMENSION])
{
    float mean = 0;
    for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
        mean += values[channel];
    mean /= LIGHT_QUERY_DIMENSION;
    float variance = 0;
    for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
    {
        float difference = values[channel] - mean;
        variance += difference * difference;
    }
    float inverseDeviation = rsqrt(variance / LIGHT_QUERY_DIMENSION + LUDUS_LIGHT_TRANSFORMER_QUERY_NORM_EPSILON);
    for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
        outValues[channel] = (values[channel] - mean) * inverseDeviation * ReadLightQueryWeight(offset + channel) +
                             ReadLightQueryWeight(offset + LIGHT_QUERY_DIMENSION + channel);
}
bool TryEvaluateLightQuery(float3 positionModel, float3 normalModel, float3 albedo, out float3 outPrediction)
{
    outPrediction = 0;
    uint words;
    uint stride;
    GraphBuffer0.GetDimensions(words, stride);
    uint4 metadata = GraphBuffer0[0];
    uint weightsBegin = metadata.w + metadata.y * LUDUS_LIGHT_ENCODER_ANCHOR_WORDS;
    uint weightWords =
        (LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT + LIGHT_QUERY_WORD_CHANNELS - 1) / LIGHT_QUERY_WORD_CHANNELS;
    if (metadata.x != LUDUS_LIGHT_ENCODER_PACKET_MAGIC || metadata.y < LIGHT_QUERY_NEIGHBORS ||
        metadata.y > LUDUS_LIGHT_ENCODER_MAX_POINTS / LUDUS_LIGHT_ENCODER_ANCHOR_DIVISOR || weightsBegin > words ||
        weightWords > words - weightsBegin)
        return false;
    PointNeighbor neighbors[LUDUS_MAX_POINT_NEIGHBORS];
    if (FindPointNeighbors(metadata.y, metadata.z, positionModel, LIGHT_QUERY_NEIGHBORS, neighbors) == false)
        return false;
    float query[LIGHT_QUERY_DIMENSION];
    float temporary[LIGHT_QUERY_DIMENSION];
    float projectedQuery[LIGHT_QUERY_DIMENSION];
    float key[LIGHT_QUERY_DIMENSION];
    float position[LIGHT_QUERY_DIMENSION];
    float aggregate[LIGHT_QUERY_DIMENSION];
    float maximum[LIGHT_QUERY_DIMENSION];
    float denominator[LIGHT_QUERY_DIMENSION];
    float work[LIGHT_QUERY_WIDE];
    for (uint channel = 0; channel < LIGHT_QUERY_POSITION_CHANNELS; ++channel)
    {
        temporary[channel] = positionModel[channel];
        temporary[LIGHT_QUERY_POSITION_CHANNELS + channel] = normalModel[channel];
        temporary[2 * LIGHT_QUERY_POSITION_CHANNELS + channel] = albedo[channel];
    }
    uint offset = 0;
    for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
        query[channel] =
            max(ComputeLightQueryRow(offset, channel, LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS, temporary, true), 0);
    offset += LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS, LIGHT_QUERY_DIMENSION);
    for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
        temporary[channel] = max(ComputeLightQueryRow(offset, channel, LIGHT_QUERY_DIMENSION, query, true), 0);
    for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
        query[channel] = temporary[channel];
    for (uint block = 0; block < LUDUS_LIGHT_TRANSFORMER_DEPTH; ++block)
    {
        uint attentionNorm = LUDUS_LIGHT_TRANSFORMER_PRE_QUERY_SIZE + block * LUDUS_LIGHT_TRANSFORMER_BLOCK_SIZE;
        uint feedForwardNorm = attentionNorm + 2 * LIGHT_QUERY_DIMENSION;
        uint queryWeights = feedForwardNorm + 2 * LIGHT_QUERY_DIMENSION;
        uint keyWeights = queryWeights + LIGHT_QUERY_DIMENSION * LIGHT_QUERY_DIMENSION;
        uint valueWeights = keyWeights + LIGHT_QUERY_DIMENSION * LIGHT_QUERY_DIMENSION;
        uint positionFirst = valueWeights + LIGHT_QUERY_DIMENSION * LIGHT_QUERY_DIMENSION;
        uint positionSecond =
            positionFirst + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LIGHT_QUERY_POSITION_CHANNELS, LIGHT_QUERY_HIDDEN);
        uint attentionFirst =
            positionSecond + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LIGHT_QUERY_HIDDEN, LIGHT_QUERY_DIMENSION);
        uint attentionSecond =
            attentionFirst + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LIGHT_QUERY_DIMENSION, LIGHT_QUERY_HIDDEN);
        uint attentionOutput =
            attentionSecond + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LIGHT_QUERY_HIDDEN, LIGHT_QUERY_DIMENSION);
        uint feedForwardFirst =
            attentionOutput + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LIGHT_QUERY_DIMENSION, LIGHT_QUERY_DIMENSION);
        uint feedForwardSecond =
            feedForwardFirst + LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LIGHT_QUERY_DIMENSION, LIGHT_QUERY_WIDE);
        NormalizeLightQuery(query, attentionNorm, temporary);
        for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
        {
            projectedQuery[channel] =
                ComputeLightQueryRow(queryWeights, channel, LIGHT_QUERY_DIMENSION, temporary, false);
            aggregate[channel] = 0;
            maximum[channel] = LIGHT_QUERY_LOWEST;
            denominator[channel] = 0;
        }
        for (uint neighbor = 0; neighbor < LIGHT_QUERY_NEIGHBORS; ++neighbor)
        {
            uint anchor = neighbors[neighbor].PointIndex;
            if (anchor >= metadata.y)
                return false;
            for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
            {
                float value = 0;
                for (uint input = 0; input < LIGHT_QUERY_DIMENSION; ++input)
                    value += ReadLightQueryWeight(keyWeights + channel * LIGHT_QUERY_DIMENSION + input) *
                             ReadLightQueryAnchor(anchor, input);
                key[channel] = value;
            }
            float3 relativePositionModel =
                asfloat(GraphBuffer0[metadata.w + anchor * LUDUS_LIGHT_ENCODER_ANCHOR_WORDS]).xyz - positionModel;
            for (uint channel = 0; channel < LIGHT_QUERY_HIDDEN; ++channel)
            {
                float value =
                    ReadLightQueryWeight(positionFirst + LIGHT_QUERY_POSITION_CHANNELS * LIGHT_QUERY_HIDDEN + channel);
                for (uint input = 0; input < LIGHT_QUERY_POSITION_CHANNELS; ++input)
                    value += ReadLightQueryWeight(positionFirst + channel * LIGHT_QUERY_POSITION_CHANNELS + input) *
                             relativePositionModel[input];
                work[channel] = max(value, 0);
            }
            for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
            {
                float value =
                    ReadLightQueryWeight(positionSecond + LIGHT_QUERY_HIDDEN * LIGHT_QUERY_DIMENSION + channel);
                for (uint input = 0; input < LIGHT_QUERY_HIDDEN; ++input)
                    value += ReadLightQueryWeight(positionSecond + channel * LIGHT_QUERY_HIDDEN + input) * work[input];
                position[channel] = value;
                temporary[channel] = key[channel] - projectedQuery[channel] + value;
            }
            for (uint channel = 0; channel < LIGHT_QUERY_HIDDEN; ++channel)
            {
                float value =
                    ReadLightQueryWeight(attentionFirst + LIGHT_QUERY_DIMENSION * LIGHT_QUERY_HIDDEN + channel);
                for (uint input = 0; input < LIGHT_QUERY_DIMENSION; ++input)
                    value += ReadLightQueryWeight(attentionFirst + channel * LIGHT_QUERY_DIMENSION + input) *
                             temporary[input];
                work[channel] = max(value, 0);
            }
            for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
            {
                float score =
                    ReadLightQueryWeight(attentionSecond + LIGHT_QUERY_HIDDEN * LIGHT_QUERY_DIMENSION + channel);
                for (uint input = 0; input < LIGHT_QUERY_HIDDEN; ++input)
                    score += ReadLightQueryWeight(attentionSecond + channel * LIGHT_QUERY_HIDDEN + input) * work[input];
                float value =
                    ComputeLightQueryRow(valueWeights, channel, LIGHT_QUERY_DIMENSION, key, false) + position[channel];
                float nextMaximum = max(maximum[channel], score);
                float previousWeight = exp(maximum[channel] - nextMaximum);
                float weight = exp(score - nextMaximum);
                aggregate[channel] = aggregate[channel] * previousWeight + value * weight;
                denominator[channel] = denominator[channel] * previousWeight + weight;
                maximum[channel] = nextMaximum;
            }
        }
        for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
            temporary[channel] = aggregate[channel] / denominator[channel];
        for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
            query[channel] += ComputeLightQueryRow(attentionOutput, channel, LIGHT_QUERY_DIMENSION, temporary, true);
        NormalizeLightQuery(query, feedForwardNorm, temporary);
        for (uint channel = 0; channel < LIGHT_QUERY_WIDE; ++channel)
        {
            float value = ReadLightQueryWeight(feedForwardFirst + LIGHT_QUERY_DIMENSION * LIGHT_QUERY_WIDE + channel);
            for (uint input = 0; input < LIGHT_QUERY_DIMENSION; ++input)
                value +=
                    ReadLightQueryWeight(feedForwardFirst + channel * LIGHT_QUERY_DIMENSION + input) * temporary[input];
            work[channel] = max(value, 0);
        }
        for (uint channel = 0; channel < LIGHT_QUERY_DIMENSION; ++channel)
        {
            float value = ReadLightQueryWeight(feedForwardSecond + LIGHT_QUERY_WIDE * LIGHT_QUERY_DIMENSION + channel);
            for (uint input = 0; input < LIGHT_QUERY_WIDE; ++input)
                value += ReadLightQueryWeight(feedForwardSecond + channel * LIGHT_QUERY_WIDE + input) * work[input];
            query[channel] += value;
        }
    }
    offset =
        LUDUS_LIGHT_TRANSFORMER_PRE_QUERY_SIZE + LUDUS_LIGHT_TRANSFORMER_DEPTH * LUDUS_LIGHT_TRANSFORMER_BLOCK_SIZE;
    for (uint channel = 0; channel < LUDUS_LIGHT_TRANSFORMER_OUTPUT_CHANNELS; ++channel)
    {
        float value =
            ReadLightQueryWeight(offset + LIGHT_QUERY_DIMENSION * LUDUS_LIGHT_TRANSFORMER_OUTPUT_CHANNELS + channel);
        for (uint input = 0; input < LIGHT_QUERY_DIMENSION; ++input)
            value += ReadLightQueryWeight(offset + channel * LIGHT_QUERY_DIMENSION + input) * query[input];
        outPrediction[channel] = value > LUDUS_LIGHT_TRANSFORMER_SOFTPLUS_THRESHOLD ? value : log(1 + exp(value));
    }
    return all(isfinite(outPrediction));
}
#endif
