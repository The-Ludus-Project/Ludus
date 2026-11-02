#pragma once

// Scalar layout shared by the offline checkpoint adapter, C++ admission and Slang.
#define LUDUS_LIGHT_TRANSFORMER_DIMENSION 128
#define LUDUS_LIGHT_TRANSFORMER_WORD_CHANNELS 4
#define LUDUS_LIGHT_TRANSFORMER_DEPTH 6
#define LUDUS_LIGHT_TRANSFORMER_NEIGHBORS 32
#define LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS 9
#define LUDUS_LIGHT_TRANSFORMER_QUERY_WORDS 3
#define LUDUS_LIGHT_TRANSFORMER_OUTPUT_CHANNELS 3
#define LUDUS_LIGHT_TRANSFORMER_POSITION_CHANNELS 3
#define LUDUS_LIGHT_TRANSFORMER_ATTENTION_CHANNELS 16
#define LUDUS_LIGHT_TRANSFORMER_FEED_FORWARD_CHANNELS 512
#define LUDUS_LIGHT_TRANSFORMER_FILE_HEADER_WORDS 8
#define LUDUS_LIGHT_TRANSFORMER_FILE_MAGIC 0x44544c4c
#define LUDUS_LIGHT_TRANSFORMER_FILE_VERSION 1

// Affine records contain row-major [output,input] weights followed by output biases.
#define LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(inputCount, outputCount) (((inputCount) + 1) * (outputCount))
#define LUDUS_LIGHT_TRANSFORMER_PRE_QUERY_SIZE                                                                         \
    (LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_QUERY_CHANNELS, LUDUS_LIGHT_TRANSFORMER_DIMENSION) +  \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_DIMENSION, LUDUS_LIGHT_TRANSFORMER_DIMENSION))
// Per block: two layer norms; Q,K,V; position MLP; attention MLP; attention output; FF MLP.
#define LUDUS_LIGHT_TRANSFORMER_BLOCK_SIZE                                                                             \
    (4 * LUDUS_LIGHT_TRANSFORMER_DIMENSION +                                                                           \
     3 * LUDUS_LIGHT_TRANSFORMER_DIMENSION * LUDUS_LIGHT_TRANSFORMER_DIMENSION +                                       \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_POSITION_CHANNELS,                                    \
                                         LUDUS_LIGHT_TRANSFORMER_ATTENTION_CHANNELS) +                                 \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_ATTENTION_CHANNELS,                                   \
                                         LUDUS_LIGHT_TRANSFORMER_DIMENSION) +                                          \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_DIMENSION,                                            \
                                         LUDUS_LIGHT_TRANSFORMER_ATTENTION_CHANNELS) +                                 \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_ATTENTION_CHANNELS,                                   \
                                         LUDUS_LIGHT_TRANSFORMER_DIMENSION) +                                          \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_DIMENSION, LUDUS_LIGHT_TRANSFORMER_DIMENSION) +       \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_DIMENSION,                                            \
                                         LUDUS_LIGHT_TRANSFORMER_FEED_FORWARD_CHANNELS) +                              \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_FEED_FORWARD_CHANNELS,                                \
                                         LUDUS_LIGHT_TRANSFORMER_DIMENSION))
#define LUDUS_LIGHT_TRANSFORMER_WEIGHT_COUNT                                                                           \
    (LUDUS_LIGHT_TRANSFORMER_PRE_QUERY_SIZE + LUDUS_LIGHT_TRANSFORMER_DEPTH * LUDUS_LIGHT_TRANSFORMER_BLOCK_SIZE +     \
     LUDUS_LIGHT_TRANSFORMER_AFFINE_SIZE(LUDUS_LIGHT_TRANSFORMER_DIMENSION,                                            \
                                         LUDUS_LIGHT_TRANSFORMER_POSITION_CHANNELS))
#define LUDUS_LIGHT_TRANSFORMER_QUERY_NORM_EPSILON 0.000001
#define LUDUS_LIGHT_TRANSFORMER_BATCH_NORM_EPSILON 0.00001
#define LUDUS_LIGHT_TRANSFORMER_SOFTPLUS_THRESHOLD 20
