/// \file scene_shader_layout.h
/// Shared scene shader revision used by native and Slang consumers.
#pragma once

// Logical API revision, independent of native resource byte layout.
#define LUDUS_SCENE_SHADER_INTERFACE_VERSION 1
// Visible intervals one line visibility query returns (a count, not bytes). The running set of
// occluded intervals holds one fewer; a query that needs more fails. Chosen by the point-line
// PL0 evidence: dense blinds of 24 slats fit with a margin of 8 (design section 4.2).
#define LUDUS_SCENE_LINE_INTERVAL_CAPACITY 32
// Line visibility merge tolerance, as a fraction of the segment (unitless, in [0, 1]). Occluded
// intervals whose gap is at most this merge, and visible intervals no longer than this are
// dropped, so the rule only removes visible length. PL0 dropped at most 2.1e-7 m (section 4.3).
#define LUDUS_SCENE_LINE_INTERVAL_MERGE_TOLERANCE 0.000001

// Why an invocation failed, recorded by FailSceneQuery with queries.HasFailed. The renderer counts the pixels
// that latch each reason, so the editor can name the failing check; see
// docs/development/render-graph-authoring.md, "Explain failed pixels". Reasons below
// LUDUS_SCENE_FAILURE_AUTHORED_FIRST are the scene interface's; authored shaders may report their own in
// [LUDUS_SCENE_FAILURE_AUTHORED_FIRST, LUDUS_SCENE_FAILURE_REASON_COUNT).
#define LUDUS_SCENE_FAILURE_UNSPECIFIED 0       // A shader set queries.HasFailed without a reason.
#define LUDUS_SCENE_FAILURE_INVALID_RADIANCE 1  // A sample returned non-finite or negative radiance.
#define LUDUS_SCENE_FAILURE_INTERFACE_VERSION 2 // The scene interface version does not match the renderer.
#define LUDUS_SCENE_FAILURE_SURFACE_BUDGET 3    // More surface queries than scene_interface.surface_queries.
#define LUDUS_SCENE_FAILURE_VISIBILITY_BUDGET 4 // More visibility queries than scene_interface.visibility_queries.
#define LUDUS_SCENE_FAILURE_LINE_BUDGET 5       // More line queries than scene_interface.line_queries.
#define LUDUS_SCENE_FAILURE_INVALID_RAY 6       // A ray with a non-unit direction or an invalid distance range.
#define LUDUS_SCENE_FAILURE_TRAVERSAL 7         // The traversal could not finish, such as a full BVH stack.
#define LUDUS_SCENE_FAILURE_RANDOM_EXHAUSTED 8  // The invocation's random stream ran out of dimensions.
#define LUDUS_SCENE_FAILURE_INVALID_ARGUMENT 9  // A scene function received an out-of-range or degenerate input.
#define LUDUS_SCENE_FAILURE_LIGHT_SAMPLE 10     // No light could be selected or sampled.
#define LUDUS_SCENE_FAILURE_STORAGE_CAPACITY 11 // A graph storage buffer has fewer entries than the pixels using it.
#define LUDUS_SCENE_FAILURE_STORED_RECORD 12    // Per-pixel data an earlier pass did not write for this pixel or frame.
#define LUDUS_SCENE_FAILURE_SCENE_DATA 13       // An invalid triangle, surface or material from a scene lookup.
#define LUDUS_SCENE_FAILURE_INVALID_WEIGHT 14   // A non-finite or negative sampling weight, probability or PDF.
#define LUDUS_SCENE_FAILURE_AUTHORED_FIRST 32
#define LUDUS_SCENE_FAILURE_REASON_COUNT 64
