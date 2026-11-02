// Explicit GPU triangle traversal; transport, shading and accumulation stay in the shared kernel.
#define LUDUS_SOFTWARE_RAY_TRAVERSAL 1
#include "renderer_gpu_preview.slang"
