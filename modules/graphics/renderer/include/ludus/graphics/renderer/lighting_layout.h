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
/// Float4 words preceding the direct-light array in the portable material ABI.
static const LightingLayoutValue FORWARD_HEADER_WORDS = 9;
/// Maximum selected direct lights in the portable forward profile.
static const LightingLayoutValue FORWARD_LIGHT_COUNT = 8;
/// Float4 words encoding one direct light.
static const LightingLayoutValue FORWARD_LIGHT_WORDS = 4;
/// Byte offset of the shadow projections after selected direct lights.
static const LightingLayoutValue FORWARD_SHADOW_OFFSET =
    (FORWARD_HEADER_WORDS + FORWARD_LIGHT_COUNT * FORWARD_LIGHT_WORDS) * 16;
/// Byte offset of surface-map controls after the shadow projections.
static const LightingLayoutValue FORWARD_SURFACE_OFFSET = FORWARD_SHADOW_OFFSET + SHADOW_DATA_WORDS * 16;
/// Maximum direct lights retained by the clustered material ABI.
static const LightingLayoutValue CLUSTERED_LIGHT_COUNT = 128;
/// Float4 rows of the rigid world-to-grid transform.
static const LightingLayoutValue CLUSTERED_GRID_ROWS = 3;
/// Grid transform rows, minimum and reciprocal cell extents.
static const LightingLayoutValue CLUSTERED_GRID_WORDS = CLUSTERED_GRID_ROWS + 2;
/// Byte offset of clustered shadow projections after the complete light list.
static const LightingLayoutValue CLUSTERED_SHADOW_OFFSET =
    (FORWARD_HEADER_WORDS + CLUSTERED_LIGHT_COUNT * FORWARD_LIGHT_WORDS) * 16;
/// Byte offset of the clustered grid transform.
static const LightingLayoutValue CLUSTERED_GRID_OFFSET = CLUSTERED_SHADOW_OFFSET + SHADOW_DATA_WORDS * 16;
/// Byte offset of clustered surface-map controls after the grid transform.
static const LightingLayoutValue CLUSTERED_SURFACE_OFFSET = CLUSTERED_GRID_OFFSET + CLUSTERED_GRID_WORDS * 16;
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
/// Count of direct lights with a nonzero (shadowed) contribution, as a viridis ramp.
static const LightingLayoutValue MATERIAL_DEBUG_VIEW_LIGHT_COUNT = 6;
/// Light count at which the debug viridis ramp saturates to its brightest colour.
static const LightingLayoutValue MATERIAL_DEBUG_LIGHT_COUNT_SATURATION = 16;

#ifdef __cplusplus
} // namespace ludus::graphics::renderer
#endif
