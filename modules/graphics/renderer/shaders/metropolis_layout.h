#pragma once

// One owner for the CPU oracle and shader primary cube and persistent-record schema.
// The editable template's Buffer stride is checked against LUDUS_MH_PIXEL_BYTES.
#define LUDUS_MH_MAXIMUM_SCATTERING_DEPTH 3
#define LUDUS_MH_CAMERA_DIMENSIONS 2
#define LUDUS_MH_BSDF_DIMENSIONS 3
#define LUDUS_MH_PRIMARY_DIMENSIONS                                                                                    \
    (LUDUS_MH_CAMERA_DIMENSIONS + LUDUS_MH_MAXIMUM_SCATTERING_DEPTH * LUDUS_MH_BSDF_DIMENSIONS)
#define LUDUS_MH_COMPONENTS_PER_WORD 4
#define LUDUS_MH_WORD_BYTES 16
#define LUDUS_MH_PRIMARY_WORDS                                                                                         \
    ((LUDUS_MH_PRIMARY_DIMENSIONS + LUDUS_MH_COMPONENTS_PER_WORD - 1) / LUDUS_MH_COMPONENTS_PER_WORD)
#define LUDUS_MH_CURRENT_WORD LUDUS_MH_PRIMARY_WORDS
#define LUDUS_MH_TOUR_WORD (LUDUS_MH_CURRENT_WORD + 1)
#define LUDUS_MH_GENERATION_WORD (LUDUS_MH_TOUR_WORD + 1)
#define LUDUS_MH_IDENTITY_WORD (LUDUS_MH_GENERATION_WORD + 1)
#define LUDUS_MH_RECORD_WORDS (LUDUS_MH_IDENTITY_WORD + 1)
#define LUDUS_MH_ESTIMATOR_COUNT 3
#define LUDUS_MH_ACCUMULATOR_WORDS 2
#define LUDUS_MH_FILM_WORDS (LUDUS_MH_ESTIMATOR_COUNT * LUDUS_MH_ACCUMULATOR_WORDS)
#define LUDUS_MH_PIXEL_BYTES ((LUDUS_MH_RECORD_WORDS + LUDUS_MH_FILM_WORDS) * LUDUS_MH_WORD_BYTES)
#define LUDUS_MH_RECORD_VERSION 2
// Profile-specific failure codes, mirrored by the public RenderGraphFailureReason enum.
#define LUDUS_SCENE_FAILURE_BSDF_ONLY_LIGHTS 15
#define LUDUS_SCENE_FAILURE_PRIMARY_MATERIAL 16
