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
/// Resolve-only triangle storage replaces the unused baked-light texture at slot three.
static const LightingLayoutValue DEFERRED_MESH_LIGHT_BINDING = LUDUS_DEFERRED_MESH_LIGHT_BINDING;
/// Resolve-only LTC table storage replaces the unused shadow texture at slot four.
static const LightingLayoutValue DEFERRED_LTC_BINDING = LUDUS_DEFERRED_LTC_BINDING;
/// Most corners of one convex emitter polygon record; covers a 64-segment pbrt disk outline. Larger convex
/// outlines are fanned into records of at most this many corners.
static const LightingLayoutValue MAX_MESH_LIGHT_POLYGON_CORNERS = 64;
/// Float4 words before an emitter polygon's corners: unit normal/sidedness, emission/corner count, then the bounding
/// sphere (MESH_LIGHT_POLYGON_BOUND_WORD).
static const LightingLayoutValue MESH_LIGHT_POLYGON_HEADER_WORDS = 3;
/// Header word holding the centroid of the record's corners in world metres (xyz) and the largest corner distance
/// from it (w), a sphere enclosing the polygon. It tightens the culling bound and selects the far-field point
/// evaluation (render-graph-authoring.md, "Mesh-light culling").
static const LightingLayoutValue MESH_LIGHT_POLYGON_BOUND_WORD = 2;
/// Float4 words of the largest record. A record is its header followed by exactly its corner count of world
/// positions, so records have variable length. The first corner's w holds the polygon's area in square metres, which
/// bounds its contribution for culling; the other corners' w are zero.
static const LightingLayoutValue MAX_MESH_LIGHT_POLYGON_WORDS =
    MESH_LIGHT_POLYGON_HEADER_WORDS + MAX_MESH_LIGHT_POLYGON_CORNERS;
/// Maximum retained emitter polygon records: merged convex polygons, unmerged triangles and four subtriangles per
/// textured source triangle. Exceeding this admission limit rejects the candidate without truncating lights. The
/// culling grid bounds per-pixel cost by the records near each pixel, so the limit bounds the upload: at most 67 words
/// per record keeps records and grid under 40 MB and every word offset exact in float32. The Sample project's Bistro
/// needs 17,571 records.
static const LightingLayoutValue MAX_RASTER_MESH_LIGHT_POLYGONS = 32768;
/// Mesh-light culling grid, appended after the polygon records of the same storage buffer when the scene has
/// emitters (render-graph-authoring.md, "Mesh-light culling"). Its first word is
/// passed to the resolve; zero means no grid, and every record is evaluated. The grid holds three header words,
/// (origin xyz in metres, cubic cell size), (cells along x, y, z, word of the first list entry) and (cull radiance,
/// zero, zero, zero), then the cell table, then the cell lists. Counts, offsets and indices are exact nonnegative
/// integers stored as float32.
static const LightingLayoutValue MESH_LIGHT_GRID_HEADER_WORDS = 3;
/// Header word whose x is the radiance below which a record's bounded Lambert term is omitted.
static const LightingLayoutValue MESH_LIGHT_GRID_CULL_WORD = 2;
/// Cells along the receivers' longest axis; the other axes use as many cubic cells as cover them.
static const LightingLayoutValue MESH_LIGHT_GRID_CELLS_PER_AXIS = 32;
/// Cell table entries per Float4 word, each a (first list entry, entry count) pair, in x-fastest cell order.
static const LightingLayoutValue MESH_LIGHT_GRID_CELLS_PER_WORD = 2;
/// List entries per Float4 word. An entry is the word offset of a listed record's header.
static const LightingLayoutValue MESH_LIGHT_GRID_ENTRIES_PER_WORD = 4;
/// Most list entries over all cells. A scene needing more uploads no grid and evaluates every record.
static const LightingLayoutValue MAX_MESH_LIGHT_GRID_ASSIGNMENTS = 1048576;
/// Largest finite half-float value; emission is nonnegative and saturates before G-buffer storage.
static const LightingLayoutValue DEFERRED_MAX_HALF = 65504;
#ifdef __cplusplus
static_assert(DEFERRED_NORMAL_BINDING == CLUSTERED_AUTHORED_BINDING_COUNT);
} // namespace ludus::graphics::renderer
#endif
