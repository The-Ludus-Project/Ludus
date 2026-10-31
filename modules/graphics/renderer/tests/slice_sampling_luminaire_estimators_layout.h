#pragma once

// Result ABI shared by slice_sampling_luminaire_estimator_tests.cpp and slice_sampling_luminaire_estimators.slang.
// Each case is one receiver of the kernel's receiver table lit by one emission profile; case c runs
// LUDUS_SLICE_SAMPLING_ESTIMATORS_INVOCATIONS_PER_CASE invocations (invocation / that count = c), each drawing
// LUDUS_SLICE_SAMPLING_ESTIMATORS_SAMPLES_PER_INVOCATION independent samples of every proposal of the
// slice-sampling-luminaire template (render_graphs/slice_sampling_luminaire/main_shaders/slice_sampling.slang). An
// invocation writes LUDUS_SLICE_SAMPLING_ESTIMATORS_RECORDS_PER_INVOCATION uint4 records of float bits:
//   0: (sum, sum of squares) of the defensive mixture's irradiance estimate, then the same for slice sampling
//   1: (sum, sum of squares) of uniform panel area's irradiance estimate, then of the seed weight W(u) (Eq. 14)
//   2: (sum, sum of squares) of f / b under uniform panel area, which also estimates Z; the number of null slice
//      outcomes; and the number of slice samples whose W differs from the W recomputed from their slope (section 5's
//      replay) by more than the kernel's relative tolerance
// An irradiance estimate is the mean RGB channel of RadianceOverDensity times the receiver cosine, unoccluded.
// The results occupy LUDUS_SLICE_SAMPLING_ESTIMATORS_RESULTS_BINDING.
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_RESULTS_BINDING 0
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_RECORDS_PER_INVOCATION 3
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_GROUP_SIZE 64
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_INVOCATIONS_PER_CASE 1024
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_SAMPLES_PER_INVOCATION 256
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_RECEIVER_COUNT 8
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_PROFILE_COUNT 2
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_CASE_COUNT                                                                     \
    (LUDUS_SLICE_SAMPLING_ESTIMATORS_RECEIVER_COUNT * LUDUS_SLICE_SAMPLING_ESTIMATORS_PROFILE_COUNT)
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_IRRADIANCE_RECORD 0
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_AREA_RECORD 1
#define LUDUS_SLICE_SAMPLING_ESTIMATORS_NORMALIZER_RECORD 2
