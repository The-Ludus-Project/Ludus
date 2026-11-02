#pragma once

// Private host/shader contracts for the editable hierarchical geometry-sampling template.
// Public callers inspect RenderGraphFailureReason, whose documented enum values match these codes.
#define LUDUS_SCENE_FAILURE_GEOMETRY_SAMPLING_LIGHTS 20
#define LUDUS_SCENE_FAILURE_GEOMETRY_STATISTICS_CAPACITY 21
#define LUDUS_GEOMETRY_SAMPLING_STATISTIC_WORDS 8
#define LUDUS_GEOMETRY_SAMPLING_HEADER_WORDS 1
// Comparison record: geometry RGB/reserved, BSDF RGB/reserved, generation/sample/version,
// then failure flag/reason/pixel extent. All four words are explicit uint4 records, never host padding.
#define LUDUS_GEOMETRY_COMPARISON_WORDS 4
