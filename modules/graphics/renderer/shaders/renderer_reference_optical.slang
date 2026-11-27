// RT1 kernel of optical reference captures (positive ReferenceCaptureDescription::OpticalWavelengthWorld): the
// renderer_reference transport plus its optical connections. Geometric captures dispatch renderer_reference, which
// compiles none of the optical connection state; see renderer_reference.slang.
#define LUDUS_REFERENCE_OPTICAL_CONNECTIONS 1
#include "renderer_reference.slang"
