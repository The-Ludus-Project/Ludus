// Diagnostic kernel of the wavefront hit-shading qualification: threads take pixels in a fixed pseudorandom
// order instead of row order. Every pixel traces and commits the same addressed samples; see renderer_reference.slang.
#define LUDUS_REFERENCE_PERMUTED_PIXELS 1
#include "renderer_reference.slang"
