#pragma once

#include "lighting_layout.h"

#ifdef __cplusplus
#    include <ludus/graphics/renderer/detail/deferred_binding_slots.inc>
#else
#    include "deferred_binding_slots.inc"
#endif

#ifdef __cplusplus
namespace ludus::graphics::renderer
{
#endif
/// RGBA16F G-buffer attachments: world normal/roughness, base color/metallic and emission/coverage.
static const LightingLayoutValue DEFERRED_TARGET_COUNT = 3;
/// Rows of the inverse world-to-clip transform in the per-view uniform block.
static const LightingLayoutValue DEFERRED_VIEW_ROWS = 4;
/// Inverse transform, packed eye, target dimensions and view options, in Float4 words.
static const LightingLayoutValue DEFERRED_VIEW_WORDS = DEFERRED_VIEW_ROWS + 3;
/// First resolve texture, following the unchanged authored material bindings.
static const LightingLayoutValue DEFERRED_NORMAL_BINDING = LUDUS_DEFERRED_NORMAL_BINDING;
/// Base-color/metallic resolve texture.
static const LightingLayoutValue DEFERRED_BASE_BINDING = LUDUS_DEFERRED_BASE_BINDING;
/// Emission/coverage resolve texture.
static const LightingLayoutValue DEFERRED_EMISSION_BINDING = LUDUS_DEFERRED_EMISSION_BINDING;
/// Unfiltered private depth read used to reconstruct world positions.
static const LightingLayoutValue DEFERRED_DEPTH_BINDING = LUDUS_DEFERRED_DEPTH_BINDING;
/// Per-view inverse transform, eye and dimensions.
static const LightingLayoutValue DEFERRED_VIEW_BINDING = LUDUS_DEFERRED_VIEW_BINDING;
/// Complete immutable resolve binding layout; at most the RHI's sixteen slots.
static const LightingLayoutValue DEFERRED_BINDING_COUNT = DEFERRED_VIEW_BINDING + 1;
/// Largest finite half-float value; emission is nonnegative and saturates before G-buffer storage.
static const LightingLayoutValue DEFERRED_MAX_HALF = 65504;
#ifdef __cplusplus
static_assert(DEFERRED_NORMAL_BINDING == CLUSTERED_AUTHORED_BINDING_COUNT);
} // namespace ludus::graphics::renderer
#endif
