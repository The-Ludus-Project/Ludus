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
/// Skin profiles a renderer holds at once: the distinct scattering distances of its live skin materials
/// (content::scene_format::MaterialModel::Skin). A skin pixel's G-buffer metallic channel holds -(one-based profile
/// slot), since skin is a dielectric, and the diffusion passes read the slot's distance from a per-view table.
static const LightingLayoutValue DEFERRED_MAX_SKIN_PROFILES = 64;
/// Rows of the inverse world-to-clip transform in the per-view uniform block.
static const LightingLayoutValue DEFERRED_VIEW_ROWS = 4;
/// Inverse transform, packed eye, target dimensions, view options and lighting view, in Float4 words.
static const LightingLayoutValue DEFERRED_VIEW_WORDS = DEFERRED_VIEW_ROWS + 4;
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
/// Mesh-light shadow casters (render-graph-authoring.md, "Mesh-light shadows"). Records are grouped into casters,
/// each a point from which cube shadow faces are rendered once per prepared scene into one RGBA8 atlas. A record's
/// caster is stored, plus one, in the w of its second corner (this corner index); zero leaves it unshadowed.
static const LightingLayoutValue MESH_LIGHT_SHADOW_CASTER_CORNER = 1;
/// The caster table follows the culling grid, and the grid cull word's y holds its first word; zero means no
/// shadows. Its header word is (caster count, atlas side in texels, reciprocal atlas side, light-disk flag): a flag
/// of one means MESH_LIGHT_SHADOW_DISK_WORDS per caster follow the casters' words, in caster order; resolves cooked
/// before the disk words read the zero-flag layout unchanged.
static const LightingLayoutValue MESH_LIGHT_SHADOW_HEADER_WORDS = 1;
/// Float4 words per caster: (position xyz in metres, range in metres), (light radius in metres, near distance over
/// range, face slot side in texels, tangent of the slot's half field of view), then the atlas texel origins of the
/// six faces, two per word. A face that receives no light has origin (-1, -1) and reads as unoccluded.
static const LightingLayoutValue MESH_LIGHT_SHADOW_CASTER_WORDS = 5;
/// Float4 words per caster after every caster's MESH_LIGHT_SHADOW_CASTER_WORDS when the header's flag is one: (unit
/// normal xyz of a one-sided caster's emitters, which face it, or zero for one enclosing them, radius in metres of the
/// light disk whose penumbra the resolve estimates). A one-sided caster's disk lies in the plane through the caster
/// across that normal, and an enclosing caster's faces the receiver.
static const LightingLayoutValue MESH_LIGHT_SHADOW_DISK_WORDS = 1;
/// Cube faces of a caster, in +X, -X, +Y, -Y, +Z, -Z order.
static const LightingLayoutValue MESH_LIGHT_SHADOW_FACE_COUNT = 6;
/// Most casters per scene. Clustering coarsens until the records fit (render-graph-authoring.md, "Mesh-light
/// shadows"); records beyond the coarsest clustering's limit stay unshadowed and are reported.
static const LightingLayoutValue MAX_MESH_LIGHT_SHADOW_CASTERS = 256;
/// Side of the square shadow atlas in texels.
static const LightingLayoutValue MESH_LIGHT_SHADOW_ATLAS_TEXELS = 2048;
/// Smallest face slot side in texels; slots are powers of two.
static const LightingLayoutValue MIN_MESH_LIGHT_SHADOW_FACE_TEXELS = 32;
/// Largest face slot side in texels, given to the most powerful caster.
static const LightingLayoutValue MAX_MESH_LIGHT_SHADOW_FACE_TEXELS = 512;
/// Texels around each face's 90-degree interior, rendered with a wider field of view. The deferred resolve looks
/// every shadow tap up by direction on the face whose interior holds it, so its filter crosses face seams and is not
/// limited by the gutter.
static const LightingLayoutValue MESH_LIGHT_SHADOW_GUTTER_TEXELS = 4;
/// Fixed upper-hemisphere directions whose orthographic depth maps give the deferred resolve its sky visibility
/// (render-graph-authoring.md, "Sky visibility"). Their regions follow the directional regions in the shadow data.
static const LightingLayoutValue SKY_VISIBILITY_DIRECTION_COUNT = 64;
/// Side of one sky visibility map slot in atlas texels, including a one-texel gutter on each edge.
static const LightingLayoutValue SKY_VISIBILITY_SLOT_TEXELS = 128;
/// Half width in world metres of the local sky maps. The receiver uses coarse coverage outside this neighbourhood.
static const LightingLayoutValue SKY_VISIBILITY_LOCAL_RADIUS_METRES = 8;
/// Sky visibility slots along each side of their reserved atlas square.
static const LightingLayoutValue SKY_VISIBILITY_SLOTS_PER_SIDE = 8;
/// Light index a sky visibility region stores in place of a light's: above every admitted light index, so a light's
/// shadow lookup never reads one, and the first of SKY_VISIBILITY_DIRECTION_COUNT trailing regions carries it.
static const LightingLayoutValue SKY_VISIBILITY_REGION_TAG = 255;
/// Forward view distance in metres at which the near and whole-scene maps begin blending.
static const LightingLayoutValue DIRECTIONAL_NEAR_BLEND_METRES = 10;
/// Forward view distance in metres beyond which the resolve samples only the whole-scene map.
static const LightingLayoutValue DIRECTIONAL_NEAR_END_METRES = 12;
/// Lighting views the clustered deferred resolve can draw in place of its final colour (DeferredLightingView).
/// Ordinary radiance of every term.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_FINAL = 0;
/// Shadowed direct radiance of every analytic light.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_DIRECT_LIGHTS = 1;
/// Shadowed direct radiance of one analytic light.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_LIGHT = 2;
/// One analytic light's shadow-map visibility and coverage as display colours.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_SHADOW_COVERAGE = 3;
/// Diffuse environment or Atmosphere sky radiance.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_SKY_DIFFUSE = 4;
/// Specular environment or Atmosphere sky radiance.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_SKY_SPECULAR = 5;
/// Shadowed radiance of every emissive mesh (area) light.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_AREA_LIGHTS = 6;
/// Radiance the Atmosphere's aerial perspective scatters in between the camera and the surface.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_AERIAL_PERSPECTIVE = 7;
/// Hue of the largest contributor at each pixel, at the brightness of the final colour.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_DOMINANT_LIGHT = 8;
/// Cosine-weighted sky visibility about the surface normal as grey.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_SKY_VISIBILITY = 9;
/// Largest lighting view code.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_LAST = DEFERRED_LIGHTING_VIEW_SKY_VISIBILITY;
/// Contributor categories of the dominant-light view, each drawn in its own hue (GetDeferredLightingViewHue): the
/// environment or sky, then all area lights together, then analytic light index i at category
/// DEFERRED_LIGHTING_VIEW_FIRST_LIGHT_CATEGORY + i in ascending source ID order.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_SKY_CATEGORY = 0;
/// Area light category of the dominant-light view.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_AREA_CATEGORY = 1;
/// Category of the first analytic light in the dominant-light view.
static const LightingLayoutValue DEFERRED_LIGHTING_VIEW_FIRST_LIGHT_CATEGORY = 2;
/// Resolve slot of the shadow atlas: the material base-colour texture, which the resolve never samples.
static const LightingLayoutValue DEFERRED_MESH_LIGHT_SHADOW_BINDING = LUDUS_DEFERRED_MESH_LIGHT_SHADOW_BINDING;
/// Largest finite half-float value; emission is nonnegative and saturates before G-buffer storage.
static const LightingLayoutValue DEFERRED_MAX_HALF = 65504;
/// Emission/coverage alpha of a covered G-buffer pixel; empty pixels keep the cleared zero and the resolve skips them.
static const LightingLayoutValue DEFERRED_COVERED = 1;
/// Emission/coverage alpha of a covered pixel whose emission already holds its surface chart's baked diffuse. The
/// resolve omits the baked lights' live diffuse there, exactly once, and keeps their live specular.
static const LightingLayoutValue DEFERRED_SURFACE_CHART_COVERED = 2;
#ifdef __cplusplus
static_assert(DEFERRED_NORMAL_BINDING == CLUSTERED_AUTHORED_BINDING_COUNT);
} // namespace ludus::graphics::renderer
#endif
