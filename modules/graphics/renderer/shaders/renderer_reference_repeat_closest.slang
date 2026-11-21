// Diagnostic kernel of the wavefront hit-shading qualification: every closest-hit query is traced twice.
// Transport, pixel and sample addresses are unchanged; see renderer_reference.slang.
#define LUDUS_REFERENCE_REPEATED_CLOSEST_TRAVERSALS 1
#include "renderer_reference.slang"
