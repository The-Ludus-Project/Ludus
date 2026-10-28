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
