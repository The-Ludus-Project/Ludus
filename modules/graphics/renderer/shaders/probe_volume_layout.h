#pragma once

// Shared with renderer_lit.slang. A probe owns six ambient-cube texels followed by
// six face-major depth maps. Faces are ordered +X, -X, +Y, -Y, +Z, -Z.
#define LUDUS_GI_PROBE_FACES 6
#define LUDUS_GI_PROBE_FACE_WIDTH 4
#define LUDUS_GI_PROBE_FACE_TEXELS (LUDUS_GI_PROBE_FACE_WIDTH * LUDUS_GI_PROBE_FACE_WIDTH)
#define LUDUS_GI_PROBE_VISIBILITY_TEXELS (LUDUS_GI_PROBE_FACES * LUDUS_GI_PROBE_FACE_TEXELS)
#define LUDUS_GI_PROBE_TEXELS (LUDUS_GI_PROBE_FACES + LUDUS_GI_PROBE_VISIBILITY_TEXELS)
#define LUDUS_GI_PROBE_ATLAS_WIDTH 256
#define LUDUS_GI_PROBE_DEPTH_CELLS 2
