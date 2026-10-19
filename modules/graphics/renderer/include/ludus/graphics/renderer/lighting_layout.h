#pragma once

#ifdef __cplusplus
#    include <ludus/foundation/base/types.h>
namespace ludus::graphics::renderer
{
/// Exact unsigned 32-bit scalar for host/shader layout constants.
using LightingLayoutValue = foundation::uint32;
#else
/// Unsigned 32-bit scalar for the shader declarations of layout constants.
typedef uint LightingLayoutValue;
#endif

/// Maximum shadow atlas regions, shared verbatim by host and shader layouts.
static const LightingLayoutValue SHADOW_REGION_CAPACITY = 80;
/// Float4 words per shadow projection: four matrix rows, rectangle and settings.
static const LightingLayoutValue SHADOW_REGION_WORDS = 6;
/// Total Float4 words in the shadow uniform array.
static const LightingLayoutValue SHADOW_DATA_WORDS = SHADOW_REGION_CAPACITY * SHADOW_REGION_WORDS;
/// Frozen CPU/compute transport profile; version 4 adds geometric emission sidedness.
static const LightingLayoutValue LIGHTING_PROFILE_VERSION = 4;

/// MaterialDebugView value for ordinary shading; debug-view programs then match their base program.
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_NONE = 0;
/// Shaded world-space normal (after surface-map perturbation), mapped from [-1,1] to [0,1] RGB.
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_NORMAL_WORLD = 1;
/// Camera distance mapped linearly between ViewDescription::DebugDepthRange near (black) and far (white).
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_LINEAR_DEPTH = 2;
/// Perceptual roughness after surface-map scaling, as grey.
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_ROUGHNESS = 3;
/// Metallic fraction after surface-map scaling, as grey.
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_METALLIC = 4;
/// Fractional mesh UV0 as red/green.
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_TEXCOORD_UV = 5;
/// Count of direct lights with a nonzero (shadowed) contribution, as a heat ramp.
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_LIGHT_COUNT = 6;
/// Light count at which the debug heat ramp saturates to its hottest colour.
static const LightingLayoutValue MATERIAL_DEBUG_LIGHT_COUNT_SATURATION = 16;

#ifdef __cplusplus
} // namespace ludus::graphics::renderer
#endif
