#pragma once

// Case ABI shared by power_cdf_parity_tests.cpp and power_cdf_parity.slang. The kernel binds the
// GPU preview's parameter block and scene words as graph shaders do, so it reads the packed power
// CDF through the public scene_shader.slang accessors. Test-only parameter use: Dimensions.xy is
// the launch extent in pixels and Dimensions.z the sample index.
// Invocation `i` selects at pixel `i` and evaluates the selection PMF of triangle `i`, writing
// one uint4: (selected triangle, selection PMF bits, has sample, PMF bits of triangle `i`).
#define LUDUS_POWER_CDF_PARITY_GROUP_SIZE 64
// The first graph storage buffer slot, LUDUS_GRAPH_BUFFER0_BINDING.
#define LUDUS_POWER_CDF_PARITY_RESULT_BINDING 6
// Candidate tickets use the review's template layout, dimension 16 + 2k for k < 32. Every
// invocation whose index is a multiple of this period instead uses one dimension past
// RANDOM_MAX_DIMENSION, which the bounded mapper rejects.
#define LUDUS_POWER_CDF_PARITY_FIRST_TICKET_DIMENSION 16
#define LUDUS_POWER_CDF_PARITY_TICKET_COUNT 32
#define LUDUS_POWER_CDF_PARITY_INVALID_PERIOD 37
