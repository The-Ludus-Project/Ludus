#pragma once

// Private GPU-compute RT1 ABI, shared verbatim by host dispatch/readback and
// renderer_reference.slang. The packed scene and Parameters block reuse the GPU
// preview layout (renderer_gpu_preview_layout.h); strides count 16-byte words.
// Update the kernel version when changing anything below.
#define LUDUS_REFERENCE_KERNEL_VERSION 3

#define LUDUS_REFERENCE_PARAMETERS_BINDING 0
#define LUDUS_REFERENCE_SCENE_BINDING 1
#define LUDUS_REFERENCE_ACCUMULATION_BINDING 2
#define LUDUS_REFERENCE_STATUS_BINDING 3
#define LUDUS_REFERENCE_TEXELS_BINDING 4
// The renderer's LTC table (ltc_table_layout.h), read by ReferenceLightSampling::ResampledProjectedSolidAngle and
// declared last: Slang numbers Metal buffers in declaration order.
#define LUDUS_REFERENCE_LTC_TABLE_BINDING 5
#define LUDUS_REFERENCE_BINDING_COUNT 6

// Parameters Sampling.z flag selecting ReferenceLightSampling::ResampledProjectedSolidAngle; the low bits keep the
// camera and opacity flags of renderer_gpu_preview_layout.h.
#define LUDUS_REFERENCE_RESAMPLED_LIGHT_FLAG 64

// Sampler version 2 bounce slots (docs/architecture/many-area-light-sampling-reference-review.md, "Random layout,
// sampler version 2"). Slots 0-7 keep their version-1 meanings, with slots 1-2 also holding the diffuse
// projected-solid-angle sample; 10-11 and 84-127 are reserved. Candidate i draws its power-CDF ticket at
// RIS_CANDIDATE_FIRST_SLOT + 2i and its reservoir ticket at the next slot; normalization candidate k draws at
// RIS_NORMALIZATION_FIRST_SLOT + k. The counts and slots are frozen constants of version 2.
#define LUDUS_REFERENCE_LTC_FIRST_SLOT 8
#define LUDUS_REFERENCE_RIS_CANDIDATE_FIRST_SLOT 12
#define LUDUS_REFERENCE_RIS_SLOTS_PER_CANDIDATE 2
#define LUDUS_REFERENCE_RIS_CANDIDATE_COUNT 32
#define LUDUS_REFERENCE_RIS_NORMALIZATION_FIRST_SLOT 76
#define LUDUS_REFERENCE_RIS_NORMALIZATION_CANDIDATE_COUNT 8

// Accumulation holds every pixel's committed sums first, then their compensation
// terms. Sum word 0 is emission plus the committed sample count, word 1 direct plus
// the pixel's dispatch status, word 2 indirect transport plus its validation failure code.
#define LUDUS_REFERENCE_SUM_WORDS 3
#define LUDUS_REFERENCE_PIXEL_WORDS 6

// Status words written by the reduction pass: (minimum samples, maximum samples,
// committed pixels, invalid pixels), (closest, visibility, null BSDF, roulette),
// (lowest failing pixel or INVALID_PIXEL, its depth, capacity failures, validation failure code).
#define LUDUS_REFERENCE_STATUS_WORDS 3
#define LUDUS_REFERENCE_INVALID_PIXEL 0xffffffff

#define LUDUS_REFERENCE_GROUP_WIDTH 64
#define LUDUS_REFERENCE_STACK_SIZE 64

// Per-pixel dispatch status word: query counts in the low two bytes, event flags,
// and the scattering depth of an invalid contribution in the top byte.
#define LUDUS_REFERENCE_QUERY_MASK 0xff
#define LUDUS_REFERENCE_VISIBILITY_SHIFT 8
#define LUDUS_REFERENCE_NULL_FLAG 0x10000
#define LUDUS_REFERENCE_ROULETTE_FLAG 0x20000
#define LUDUS_REFERENCE_INVALID_FLAG 0x40000
#define LUDUS_REFERENCE_COMMITTED_FLAG 0x80000
#define LUDUS_REFERENCE_CAPACITY_FLAG 0x100000
#define LUDUS_REFERENCE_DEPTH_SHIFT 24

// Validation stages mirror ReferenceFailure; host assertions keep the ABI synchronized.
#define LUDUS_REFERENCE_FAILURE_NONE 0
#define LUDUS_REFERENCE_FAILURE_INVALID_HIT 1
#define LUDUS_REFERENCE_FAILURE_INVALID_EMITTER_PDF 2
#define LUDUS_REFERENCE_FAILURE_INVALID_SURFACE_PARAMETERS 3
#define LUDUS_REFERENCE_FAILURE_INVALID_LIGHT_SAMPLE 4
#define LUDUS_REFERENCE_FAILURE_INVALID_BSDF_EVALUATION 5
#define LUDUS_REFERENCE_FAILURE_INVALID_DIRECT_CONTRIBUTION 6
#define LUDUS_REFERENCE_FAILURE_INVALID_SHADOW_SEGMENT 7
#define LUDUS_REFERENCE_FAILURE_INVALID_BSDF_SAMPLE 8
#define LUDUS_REFERENCE_FAILURE_INVALID_THROUGHPUT 9
#define LUDUS_REFERENCE_FAILURE_INVALID_RAY_ORIGIN 10
#define LUDUS_REFERENCE_FAILURE_INVALID_RADIANCE 11
#define LUDUS_REFERENCE_FAILURE_INVALID_ACCUMULATION 12
#define LUDUS_REFERENCE_FAILURE_INVALID_CAMERA_RAY 13
#define LUDUS_REFERENCE_FAILURE_INVALID_SCENE_LAYOUT 14
#define LUDUS_REFERENCE_FAILURE_TRAVERSAL_CAPACITY_EXCEEDED 15
