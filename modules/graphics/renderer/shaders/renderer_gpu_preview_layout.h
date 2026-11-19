#pragma once

#ifdef __cplusplus
#    include <ludus/content/scene_format/material.h>
#    include <ludus/graphics/renderer/lighting_layout.h>
#    include <ludus/graphics/renderer/scene_shader_layout.h>
#else
#    include "lighting_layout.h"
#    include "material.h"
#    include "scene_shader_layout.h"
#endif

// Private packed-scene ABI, shared verbatim by host packing and Slang admission.
// Strides count float4 words, not bytes. Update the version when changing layout.
#define LUDUS_GPU_PREVIEW_SURFACE_WORDS 12
#define LUDUS_GPU_PREVIEW_LIGHT_WORDS 4
// Panel luminaires follow the light records: LUMINAIRE_HEADER_WORDS words whose first X holds the count bits, then
// LUMINAIRE_WORDS per luminaire: center/half extent, tangent/radiance scale, bitangent/beam width, then beam
// steering/spread/twist and 0. The frame is orthonormal; the panel emits along cross(tangent, bitangent).
#define LUDUS_GPU_PREVIEW_LUMINAIRE_HEADER_WORDS 1
#define LUDUS_GPU_PREVIEW_LUMINAIRE_WORDS 4
// Word of a luminaire record holding beam steering, spread and twist.
#define LUDUS_GPU_PREVIEW_LUMINAIRE_BEAM_WORD 3
// 18: the camera matrix maps clip to eye-relative world (ClipToEye); 17: optional RT1 camera sky; 16: an RT1
// reference medium (renderer_reference_layout.h); 15: closure programs after the environment; 12: panel luminaires
// after the lights; 9: emitter bounds after the emitter records; 8: BVH leaf order in order word Z, beside native
// opacity ranges in X.
#define LUDUS_GPU_PREVIEW_KERNEL_VERSION 18
// Closure programs (material_program.h) follow the environment. Each has a header word (instruction, lobe and
// constant counts in X, Y and Z bits), its constants, one word per instruction (opcode, destination and operand A
// bits, then operands B | C << PROGRAM_OPERAND_SHIFT), then PROGRAM_LOBE_WORDS per lobe: (operation, weight and
// roughness register bits, power), eta and k. A Program surface's SCATTERING_ALPHA word W holds the bits of its
// program's first word; offset zero never holds a program.
#define LUDUS_GPU_PREVIEW_PROGRAM_HEADER_WORDS 1
#define LUDUS_GPU_PREVIEW_PROGRAM_LOBE_WORDS 3
#define LUDUS_GPU_PREVIEW_PROGRAM_OPERAND_SHIFT 8
// Words per mesh emitter in the bounds records that follow the emitter records:
// sphere centre/radius, normal-cone axis/cosine, then flags (X) and the mesh power CDF (Y).
#define LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_WORDS 3
#define LUDUS_GPU_PREVIEW_EMITTER_METADATA_WORD 2
#define LUDUS_GPU_PREVIEW_EMITTER_BOUNDS_TWO_SIDED 1u

#define LUDUS_GPU_PREVIEW_TRIANGLE_WORDS 16

#define LUDUS_GPU_PREVIEW_TEXTURES_PER_SURFACE 3

// Addressed-random dimension schema shared by the CPU reference path and the GPU
// preview: dimensions 0-1 jitter the primary ray, and scattering event d owns the
// PER_BOUNCE dimensions starting at FIRST_BOUNCE + d * PER_BOUNCE.
#define LUDUS_PATH_RANDOM_FIRST_BOUNCE_DIMENSION 2
#define LUDUS_PATH_RANDOM_DIMENSIONS_PER_BOUNCE 8
// Existing slots shared by the CPU and GPU reference light proposals.
#define LUDUS_PATH_RANDOM_LIGHT_SELECTION_SLOT 0
#define LUDUS_PATH_RANDOM_INCIDENT_FIRST_SLOT 1
#define LUDUS_PATH_RANDOM_INCIDENT_SECOND_SLOT 2
// Reference sampler version 2 (ReferenceLightSampling::ResampledProjectedSolidAngle) keeps slots 0-7 and widens each
// bounce to 128 dimensions; renderer_reference_layout.h assigns the new slots.
#define LUDUS_PATH_RANDOM_VERSION2_DIMENSIONS_PER_BOUNCE 128
// Bounce slot of the bounded draw that picks an emitter triangle from the
// power-weighted emitter CDF once the uniform light proposal chose an emitter.
#define LUDUS_PATH_RANDOM_EMITTER_SLOT 7
// Admitted MaxDepth limits. Both keep every bounce dimension of both sampler versions within the 16-bit
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
#define LUDUS_GPU_PREVIEW_PARAMETERS_BYTES 240

// Options.w packs scene-data groups in bits 0–2 and scene-wide material restrictions above them.
#define LUDUS_GRAPH_REQUIRES_EXTENDED_BSDF 8u

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
// The graph ToneMap applies Khronos PBR Neutral instead of Reinhard (display_tone_mapping.slang).
#define LUDUS_GPU_PREVIEW_PBR_NEUTRAL_FLAG 64
// Scene work uses the compute graph wrapper without a camera or an image. Dimensions.w carries its item count.
#define LUDUS_GPU_PREVIEW_SCENE_WORK_FLAG 128u
#define LUDUS_GPU_PREVIEW_SCENE_WORK_ROW_WIDTH 1024u
#define LUDUS_GPU_PREVIEW_ROULETTE_OR_WORK_COUNT_COMPONENT 3
#define LUDUS_GPU_PREVIEW_HISTORY_SHIFT 12
#define LUDUS_GPU_PREVIEW_FLAGS_MASK ((1u << LUDUS_GPU_PREVIEW_HISTORY_SHIFT) - 1u)
#define LUDUS_GPU_PREVIEW_MAX_HISTORY_EPOCH (0xffffffffu >> LUDUS_GPU_PREVIEW_HISTORY_SHIFT)
#define LUDUS_GPU_PREVIEW_RAY_MASK 255
#define LUDUS_GPU_PREVIEW_VERTEX_COLOR_WORD 13

#define LUDUS_GPU_PREVIEW_BINDING_COUNT 6
#define LUDUS_GPU_PREVIEW_SCENE_BINDING 1
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
#define LUDUS_GPU_PREVIEW_SCATTERING_ETA_WORD 9
#define LUDUS_GPU_PREVIEW_SCATTERING_K_WORD 10
#define LUDUS_GPU_PREVIEW_SCATTERING_ALPHA_WORD 11

#define LUDUS_GPU_PREVIEW_TEXTURE_LINEAR_FLAG 1
#define LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_U_FLAG 2
#define LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_V_FLAG 4
#define LUDUS_GPU_PREVIEW_TEXTURE_SRGB_FLAG 8
#define LUDUS_GPU_PREVIEW_TEXTURE_SAMPLER_FLAGS                                                                        \
    (LUDUS_GPU_PREVIEW_TEXTURE_LINEAR_FLAG | LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_U_FLAG |                                 \
     LUDUS_GPU_PREVIEW_TEXTURE_REPEAT_V_FLAG)

// Extended graph ABI; low Sampling.z bits retain the camera/kernel flags.
#define LUDUS_GRAPH_PASS_INDEX_SHIFT 8
#define LUDUS_GRAPH_PROGRAM_COUNT 16
#define LUDUS_GRAPH_PASS_INDEX_MASK (LUDUS_GRAPH_PROGRAM_COUNT - 1)
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
#define LUDUS_RAY_PATH_CAPTURE_PASS_COUNT LUDUS_GRAPH_PROGRAM_COUNT
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

// Renderer-owned read-only LTC table (Peters 2021 F0 axis; docs/development/ltc-table.md), bound
// to a scene compute kernel whose technique sets scene_interface.polygonal_light_sampling and whose reflection
// keeps this slot. Slots 11 and 12 hold the native Metal ray function tables. The table is the
// last binding of a job, after the ray path capture buffer.
#define LUDUS_GRAPH_LTC_TABLE_BINDING 13
#define LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT 1
// Per-job failure summary of a graph ray kernel: one uint per LUDUS_SCENE_FAILURE_* reason (scene_shader_layout.h),
// counting the pixels that latched that reason since the job's camera or scene last changed. Every graph ray kernel
// binds it after the LTC table and before the Atmosphere sky; the native preview and reference kernels do not.
#define LUDUS_GRAPH_FAILURE_BINDING 14
#define LUDUS_GRAPH_FAILURE_BINDING_COUNT 1
// The diagnostic wrapper owns this scene failure reason; authored callbacks use their own range.
#define LUDUS_SCENE_FAILURE_LUSL_BOUNDS 28
// One optional LUSL line follows the failure count words. Old kernels write only the counts.
#define LUDUS_GRAPH_FAILURE_BOUNDS_LINE_WORD LUDUS_SCENE_FAILURE_REASON_COUNT
#define LUDUS_GRAPH_FAILURE_WORDS (LUDUS_GRAPH_FAILURE_BOUNDS_LINE_WORD + 1)
#define LUDUS_GRAPH_FAILURE_WORD_BYTES 4
#define LUDUS_GRAPH_FAILURE_BYTES (LUDUS_GRAPH_FAILURE_WORDS * LUDUS_GRAPH_FAILURE_WORD_BYTES)

// World-feature tables (graphics::renderer::WorldFeature; docs/development/render-graph-authoring.md#world-features).
// The renderer binds each read-only float4 table only to a scene compute kernel whose reflection keeps its slot, which
// happens exactly when the kernel calls the scene function that reads it. Native ray stages reserve slot 11 for their
// function table and bind neither. Both tables live in the job's scene buffer at a
// LUDUS_GRAPH_WORLD_TABLE_ALIGNMENT_BYTES-aligned offset, after the scene words.
//
// Aerial perspective, read by ApplyAerialPerspective: a header of LUDUS_AERIAL_PERSPECTIVE_HEADER_WORDS words (word 0
// x = 1 when present, 0 omits the haze; word 1 xyz = outward up; word 2 xyz = unit direction toward the Sun), then two
// words per entry (in-scattered RGB radiance, RGB transmittance) ordered by distance slice, zenith row, then Sun
// azimuth column. Slice k ends LUDUS_AERIAL_PERSPECTIVE_MAX_METRES * (k / (slices - 1))^2 metres from the observer.
#define LUDUS_GRAPH_AERIAL_PERSPECTIVE_BINDING 11
#define LUDUS_AERIAL_PERSPECTIVE_HEADER_WORDS 3
#define LUDUS_AERIAL_PERSPECTIVE_ENTRY_WORDS 2
#define LUDUS_AERIAL_PERSPECTIVE_AZIMUTH_COUNT 32
#define LUDUS_AERIAL_PERSPECTIVE_ZENITH_COUNT 32
#define LUDUS_AERIAL_PERSPECTIVE_SLICE_COUNT 32
#define LUDUS_AERIAL_PERSPECTIVE_MAX_METRES 32000.0
// Atmosphere sky, read by EvaluateEnvironment: one header word (x = width, y = height, as exact floats; zero width
// keeps the World environment), then width * height RGB radiance texels in EnvironmentDescription's latitude/longitude
// layout. The binding follows the failure summary and is a kernel's last binding.
#define LUDUS_GRAPH_ATMOSPHERE_SKY_BINDING 15
#define LUDUS_ATMOSPHERE_SKY_HEADER_WORDS 1
#define LUDUS_ATMOSPHERE_SKY_TABLE_WIDTH 512
#define LUDUS_ATMOSPHERE_SKY_TABLE_HEIGHT 256
#define LUDUS_GRAPH_WORLD_TABLE_ALIGNMENT_BYTES 256
// Renderer-owned World-feature tables one kernel may bind: the aerial perspective and Atmosphere sky tables.
#define LUDUS_GRAPH_WORLD_TABLE_BINDING_COUNT 2
