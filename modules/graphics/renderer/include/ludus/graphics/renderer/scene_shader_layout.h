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
