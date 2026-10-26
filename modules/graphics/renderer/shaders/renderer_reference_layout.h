#pragma once

// Private GPU-compute RT1 ABI, shared verbatim by host dispatch/readback and
// renderer_reference.slang. The packed scene and Parameters block reuse the GPU
// preview layout (renderer_gpu_preview_layout.h); strides count 16-byte words.
// Update the kernel version when changing anything below.
#define LUDUS_REFERENCE_KERNEL_VERSION 1

#define LUDUS_REFERENCE_PARAMETERS_BINDING 0
#define LUDUS_REFERENCE_SCENE_BINDING 1
#define LUDUS_REFERENCE_ACCUMULATION_BINDING 2
#define LUDUS_REFERENCE_STATUS_BINDING 3
#define LUDUS_REFERENCE_TEXELS_BINDING 4
#define LUDUS_REFERENCE_BINDING_COUNT 5

// Accumulation holds every pixel's committed sums first, then their compensation
// terms. Sum word 0 is emission plus the committed sample count, word 1 direct plus
// the pixel's dispatch status, word 2 indirect transport.
#define LUDUS_REFERENCE_SUM_WORDS 3
#define LUDUS_REFERENCE_PIXEL_WORDS 6

// Status words written by the reduction pass: (minimum samples, maximum samples,
// committed pixels, invalid pixels), (closest, visibility, null BSDF, roulette),
// (lowest failing pixel or INVALID_PIXEL, its depth, capacity failures, reserved).
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
