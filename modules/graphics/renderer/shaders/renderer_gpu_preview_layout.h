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
// 9: emitter bounds after the emitter records; 8: BVH leaf order in order word Z, beside native opacity ranges in X.
#define LUDUS_GPU_PREVIEW_KERNEL_VERSION 9
// Words per mesh emitter in the bounds records that follow the emitter records:
// sphere centre/radius, normal-cone axis/cosine, then flag bits in word 2 X.
#define LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_WORDS 3
#define LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_TWO_SIDED 1u

#define LUDUS_GPU_PREVIEW_TRIANGLE_WORDS 16

#define LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE 3

// Addressed-random dimension schema shared by the CPU reference path and the GPU
// preview: dimensions 0-1 jitter the primary ray, and scattering event d owns the
// PER_BOUNCE dimensions starting at FIRST_BOUNCE + d * PER_BOUNCE.
#define LUDUS_PATH_RANDOM_FIRST_BOUNCE_DIMENSION 2
#define LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE 8
// Bounce slot of the bounded draw that picks an emitter triangle from the
// power-weighted emitter CDF once the uniform light proposal chose an emitter.
#define LUDUS_PATH_RANDOM_EMITTER_SLOT 7
// Admitted MaxDepth limits. Both keep every bounce dimension within the 16-bit
// Dimension of addressed-random layout v1; host code static_asserts this.
#define LUDUS_REFERENCE_MAX_DEPTH 64
#define LUDUS_GPU_PREVIEW_MAX_DEPTH 8

// Shared triangle traversal and linked graph-kernel representation constants.
#define LUDUS_GPU_PREVIEW_GEOMETRIC_NORMAL_WORD 8
#define LUDUS_GPU_PREVIEW_INVALID_TRIANGLE 0xffffffff
#define LUDUS_GPU_PREVIEW_FAILED_TRIANGLE 0xfffffffe
// Graph callback entries retain their existing linear grouping.
#define LUDUS_GPU_PREVIEW_GROUP_WIDTH 64
// Thanks to Apple, "Scale compute workloads across Apple GPUs", WWDC22,
// https://developer.apple.com/videos/play/wwdc2022/10159/: compare group shapes
// against matched pixel/sample addresses. The native preview's measured 8x8
// choice changes dispatch grouping, not transport arithmetic or the estimator.
#ifndef LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_WIDTH
#    define LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_WIDTH 8
#endif
#ifndef LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_HEIGHT
#    define LUDUS_GPU_PREVIEW_TRANSPORT_GROUP_HEIGHT 8
#endif
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
// Mixed ranges permit native opaque visibility and direct opaque candidate acceptance.
#define LUDUS_GPU_PREVIEW_MIXED_OPACITY_FLAG 32
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

// Material record word addresses; the host packer and shader readers share these.
#define LUDUS_GPU_PREVIEW_BASE_COLOR_WORD 0
#define LUDUS_GPU_PREVIEW_ROUGHNESS_WORD 1
#define LUDUS_GPU_PREVIEW_TINT_WORD 2
#define LUDUS_GPU_PREVIEW_UV_TRANSFORM_WORD 3
#define LUDUS_GPU_PREVIEW_EMISSION_WORD 4
#define LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD 5
#define LUDUS_GPU_PREVIEW_SURFACE_MAP_WORD 6
#define LUDUS_GPU_PREVIEW_SURFACE_OPTIONS_WORD 7
#define LUDUS_GPU_PREVIEW_EMISSION_MAP_WORD 8

#define LUDUS_GPU_PREVIEW_TEXTURE_LINEAR_FLAG 1
#define LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_U_FLAG 2
#define LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_V_FLAG 4
#define LUDUS_GPU_PREVIEW_TEXTURE_SRGB_FLAG 8
#define LUDUS_GPU_PREVIEW_TEXTURE_SAMPLER_FLAGS                                                                        \
    (LUDUS_GPU_PREVIEW_TEXTURE_LINEAR_FLAG | LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_U_FLAG |                                 \
     LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_V_FLAG)

// Extended graph ABI; low Sampling.z bits retain the camera/kernel flags.
#define LUDUS_GRAPH_PASS_INDEX_SHIFT 8
#define LUDUS_GRAPH_PASS_INDEX_MASK 3
#define LUDUS_GRAPH_BUFFER0_BINDING 6
#define LUDUS_GRAPH_BUFFER1_BINDING 7
#define LUDUS_GRAPH_COMPUTE_BINDING_COUNT 8
// Per-pass ray traversal codes, matching render_graph_format::RayTraversal. The build writes one
// code per ordered pass into the linked kernel's strategy table.
#define LUDUS_GRAPH_TRAVERSAL_NATIVE_QUERY 0
#define LUDUS_GRAPH_TRAVERSAL_TRIANGLE_LOOP 1
#define LUDUS_GRAPH_TRAVERSAL_CUSTOM 2
#define LUDUS_GRAPH_TRAVERSAL_HARDWARE_PIPELINE 3
#define LUDUS_GRAPH_TRAVERSAL_COMPUTE_BVH 4

// Ray path capture: diagnostic LUDUS_RAY_PATH_CAPTURE kernels only. One uint32-word
// storage buffer holds a CPU-written header, a summary per selected pixel, sample and
// pass, then appended event records. Offsets and strides count words, not bytes; the
// host decodes field by field. See render-graph-authoring.md#ray-path-inspection-proposed.
#define LUDUS_RAY_PATH_CAPTURE_BINDING 10
#define LUDUS_RAY_PATH_CAPTURE_VERSION 1
#define LUDUS_RAY_PATH_CAPTURE_WORD_BYTES 4
#define LUDUS_RAY_PATH_CAPTURE_PASS_COUNT 4
#define LUDUS_RAY_PATH_HEADER_VERSION_WORD 0
#define LUDUS_RAY_PATH_HEADER_PIXEL_X_WORD 1
#define LUDUS_RAY_PATH_HEADER_PIXEL_Y_WORD 2
#define LUDUS_RAY_PATH_HEADER_WIDTH_WORD 3
#define LUDUS_RAY_PATH_HEADER_HEIGHT_WORD 4
#define LUDUS_RAY_PATH_HEADER_FIRST_SAMPLE_WORD 5
#define LUDUS_RAY_PATH_HEADER_SAMPLE_COUNT_WORD 6
#define LUDUS_RAY_PATH_HEADER_RECORD_CAPACITY_WORD 7
#define LUDUS_RAY_PATH_HEADER_APPENDED_WORD 8
#define LUDUS_RAY_PATH_HEADER_DROPPED_WORD 9
#define LUDUS_RAY_PATH_HEADER_WORDS 12
#define LUDUS_RAY_PATH_SUMMARY_SAMPLE_WORD 0
#define LUDUS_RAY_PATH_SUMMARY_PASS_WORD 1
#define LUDUS_RAY_PATH_SUMMARY_EVENT_COUNT_WORD 2
#define LUDUS_RAY_PATH_SUMMARY_FLAGS_WORD 3
#define LUDUS_RAY_PATH_SUMMARY_RADIANCE_WORD 4
#define LUDUS_RAY_PATH_SUMMARY_WORDS 8
#define LUDUS_RAY_PATH_SUMMARY_WRITTEN_FLAG 1
#define LUDUS_RAY_PATH_SUMMARY_FAILED_FLAG 2
#define LUDUS_RAY_PATH_RECORD_PIXEL_X_WORD 0
#define LUDUS_RAY_PATH_RECORD_PIXEL_Y_WORD 1
#define LUDUS_RAY_PATH_RECORD_SAMPLE_WORD 2
#define LUDUS_RAY_PATH_RECORD_PASS_WORD 3
#define LUDUS_RAY_PATH_RECORD_SEQUENCE_WORD 4
#define LUDUS_RAY_PATH_RECORD_KIND_WORD 5
#define LUDUS_RAY_PATH_RECORD_OUTCOME_WORD 6
#define LUDUS_RAY_PATH_RECORD_REJECTED_WORD 7
#define LUDUS_RAY_PATH_RECORD_ORIGIN_WORD 8
#define LUDUS_RAY_PATH_RECORD_MIN_DISTANCE_WORD 11
#define LUDUS_RAY_PATH_RECORD_DIRECTION_WORD 12
#define LUDUS_RAY_PATH_RECORD_MAX_DISTANCE_WORD 15
#define LUDUS_RAY_PATH_RECORD_HIT_DISTANCE_WORD 16
#define LUDUS_RAY_PATH_RECORD_TRIANGLE_WORD 17
#define LUDUS_RAY_PATH_RECORD_INSTANCE_WORD 18
#define LUDUS_RAY_PATH_RECORD_PRIMITIVE_WORD 19
#define LUDUS_RAY_PATH_RECORD_SOURCE_LOW_WORD 20
#define LUDUS_RAY_PATH_RECORD_SOURCE_HIGH_WORD 21
#define LUDUS_RAY_PATH_RECORD_WORDS 22
#define LUDUS_RAY_PATH_KIND_CLOSEST 0
#define LUDUS_RAY_PATH_KIND_VISIBILITY 1
#define LUDUS_RAY_PATH_KIND_RASTER_FIRST_HIT 2
#define LUDUS_RAY_PATH_OUTCOME_MISS 0
#define LUDUS_RAY_PATH_OUTCOME_HIT 1
#define LUDUS_RAY_PATH_OUTCOME_FAILURE 2
