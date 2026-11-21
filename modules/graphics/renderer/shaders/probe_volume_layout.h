#pragma once

// Shared with renderer_lit.slang. A probe owns six ambient-cube texels followed by
// six face-major visibility maps. Faces are ordered +X, -X, +Y, -Y, +Z, -Z.
// Each visibility texel stores four depths in RGBA order: (0, 0), (1, 0),
// (0, 1), (1, 1). Offsets use the same face and clamp at its edge.
#define LUDUS_GI_PROBE_FACES 6
#define LUDUS_GI_PROBE_FACE_WIDTH 16
#define LUDUS_GI_PROBE_FACE_TEXELS (LUDUS_GI_PROBE_FACE_WIDTH * LUDUS_GI_PROBE_FACE_WIDTH)
#define LUDUS_GI_PROBE_VISIBILITY_TEXELS (LUDUS_GI_PROBE_FACES * LUDUS_GI_PROBE_FACE_TEXELS)
#define LUDUS_GI_PROBE_TEXELS (LUDUS_GI_PROBE_FACES + LUDUS_GI_PROBE_VISIBILITY_TEXELS)
// The changed width rejects every older nearest-depth atlas on load. It also
// keeps the largest supported grid within the previous maximum atlas height.
#define LUDUS_GI_PROBE_ATLAS_WIDTH 1024
#define LUDUS_GI_PROBE_DEPTH_CELLS 2
// Bias in cell units covers rounding of the two-cell UNORM8 depth range.
#define LUDUS_GI_PROBE_DEPTH_BIAS_CELLS (1.0F / 128.0F)
// Suppress sub-footprint visibility that can dominate renormalized probe weights.
#define LUDUS_GI_PROBE_MIN_VISIBLE_COVERAGE 0.75F
