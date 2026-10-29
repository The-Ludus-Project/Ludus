#pragma once

// Literal preprocessor slots also generate explicit native registers. Reflection
// independently checks that the native and portable slot identities agree.
// First deferred resolve texture slot after the eleven authored material resources.
#define LUDUS_DEFERRED_NORMAL_BINDING 11
// Deferred base-color/metallic texture slot.
#define LUDUS_DEFERRED_BASE_BINDING 12
// Deferred emission/coverage texture slot.
#define LUDUS_DEFERRED_EMISSION_BINDING 13
// Deferred sampled private-depth slot.
#define LUDUS_DEFERRED_DEPTH_BINDING 14
// Deferred camera uniform slot.
#define LUDUS_DEFERRED_VIEW_BINDING 15

// Resolve-only storage replaces resources unused after material evaluation.
#define LUDUS_DEFERRED_MESH_LIGHT_BINDING 3
#define LUDUS_DEFERRED_LTC_BINDING 4
// Reconstructed positions within this plane tolerance are treated as coplanar,
// preventing a triangle from illuminating its own rasterized surface.
#define LUDUS_MESH_LIGHT_PLANE_EPSILON_METERS 0.00001
#define LUDUS_MESH_LIGHT_PLANE_RELATIVE_EPSILON 0.000002
