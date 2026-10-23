#pragma once

#ifdef __cplusplus
#    include <ludus/graphics/renderer/lighting_layout.h>
#else
#    include "lighting_layout.h"
#endif

// Private packed-scene ABI, shared verbatim by host packing and Slang admission.
// Strides count float4 words, not bytes. Update the version when changing layout.
#define LUDUS_GPU_PREVIEW_SURFACE_WORDS 9
#define LUDUS_GPU_PREVIEW_LIGHT_WORDS 4
#define LUDUS_GPU_PREVIEW_KERNEL_VERSION LIGHTING_PROFILE_VERSION

#define LUDUS_GPU_PREVIEW_TRIANGLE_WORDS 16

#define LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE 3

// Addressed-random dimension schema shared by the CPU reference path and the GPU
// preview: dimensions 0-1 jitter the primary ray, and scattering event d owns the
// PER_BOUNCE dimensions starting at FIRST_BOUNCE + d * PER_BOUNCE.
#define LUDUS_PATH_RANDOM_FIRST_BOUNCE_DIMENSION 2
#define LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE 8
// Admitted MaxDepth limits. Both keep every bounce dimension within the 16-bit
// Dimension of addressed-random layout v1; host code static_asserts this.
#define LUDUS_REFERENCE_MAX_DEPTH 64
#define LUDUS_GPU_PREVIEW_MAX_DEPTH 8

// Shared triangle traversal and linked graph-kernel representation constants.
#define LUDUS_GPU_PREVIEW_GEOMETRIC_NORMAL_WORD 8
#define LUDUS_GPU_PREVIEW_INVALID_TRIANGLE 0xffffffff
#define LUDUS_GPU_PREVIEW_FAILED_TRIANGLE 0xfffffffe
#define LUDUS_GPU_PREVIEW_GROUP_WIDTH 64
#define LUDUS_GPU_PREVIEW_PARAMETERS_BYTES 224

// Scene interface metadata in otherwise unused packed triangle components.
#define LUDUS_GRAPH_INSTANCE_WORD 3
#define LUDUS_GRAPH_MATERIAL_WORD 4
#define LUDUS_GRAPH_MATERIAL_KIND_WORD 5
#define LUDUS_GRAPH_SOURCE_WORD 7
#define LUDUS_GRAPH_EMITTER_WORD 9
#define LUDUS_GRAPH_LIGHT_SOURCE_WORD 3
#define LUDUS_GRAPH_SOURCE_HIGH_SHIFT 32
#define LUDUS_GRAPH_TEXTURE_CHANNELS 4

// Preserve camera/kernel flags and ordered invocation bits below camera history.
#define LUDUS_GPU_PREVIEW_OPAQUE_FLAG 8
#define LUDUS_GPU_PREVIEW_DENOISING_FLAG 16
#define LUDUS_GPU_PREVIEW_FLAGS_MASK 0x3ff
#define LUDUS_GPU_PREVIEW_HISTORY_SHIFT 10
#define LUDUS_GPU_PREVIEW_MAX_HISTORY_EPOCH 0x1fffff
#define LUDUS_GPU_PREVIEW_RAY_MASK 255
#define LUDUS_GPU_PREVIEW_VERTEX_COLOR_WORD 13

#define LUDUS_GPU_PREVIEW_BINDING_COUNT 6
#define LUDUS_GRAPH_VISIBILITY_BINDING 8
#define LUDUS_GRAPH_VISIBILITY_DEPTH_BINDING 9
#define LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT 2
#define LUDUS_GRAPH_VISIBILITY_BINDING_COUNT (LUDUS_GPU_PREVIEW_BINDING_COUNT + LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT)
#define LUDUS_GPU_PREVIEW_STORAGE_BINDING_COUNT 4
#define LUDUS_GPU_PREVIEW_VECTOR_BYTES 16
#define LUDUS_GPU_PREVIEW_RAY_SCENE_BINDING 5
#define LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES 32

#define LUDUS_GPU_PREVIEW_COLORS_BINDING 3
#define LUDUS_GPU_PREVIEW_TEXELS_BINDING 4

#define LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD 5

// Extended graph ABI; low Sampling.z bits retain the camera/kernel flags.
#define LUDUS_GRAPH_PASS_INDEX_SHIFT 8
#define LUDUS_GRAPH_PASS_INDEX_MASK 3
#define LUDUS_GRAPH_BUFFER0_BINDING 6
#define LUDUS_GRAPH_BUFFER1_BINDING 7
#define LUDUS_GRAPH_COMPUTE_BINDING_COUNT 8
