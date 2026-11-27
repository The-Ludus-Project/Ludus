#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

// Cached cube shadow maps for raster mesh lights (render-graph-authoring.md, "Mesh-light shadows"). Emitter polygon
// records are grouped into casters; each caster renders up to six cube faces of linear depth once per prepared scene
// with the renderer's shadow pipelines, and the deferred resolve compares a receiver's depth in the same face
// projection with them.
namespace ludus::graphics::renderer::internal
{
/// Fullscreen and auxiliary draw allowance shared by shadow admission and submission.
inline constexpr foundation::usize SHADOW_FRAME_RESERVED_DRAWS = 16;
/// Most triangles the shadow faces drawn in one frame submit; a face over the budget is drawn alone. A face submits
/// every triangle of each mesh its frustum and caster range reach, so a scene of large meshes (Bistro: about half a
/// million triangles per face) completes its faces over many frames instead of stalling a few.
inline constexpr foundation::uint64 MAX_MESH_LIGHT_SHADOW_TRIANGLES_PER_FRAME = 4000000;
/// Atlas texel origin of one face slot.
struct MeshLightShadowFaceOrigin final
{
    foundation::uint32 X = 0;
    foundation::uint32 Y = 0;
};
/// One cluster of emitter records shadowed from a single point.
struct MeshLightShadowCaster final
{
    /// Point from which the faces are rendered: the members' enclosing sphere centre, moved a little along the mean
    /// normal of one-sided emitters so it lies in front of them.
    foundation::math::Vector3 PositionWorld;
    /// Radius of the sphere enclosing every member polygon, in metres.
    foundation::float32 LightRadiusMeters = 0;
    /// Unit flux-weighted mean normal of a one-sided caster's members, which face it; zero for an enclosing caster.
    /// The resolve places the light disk whose penumbra it estimates in the plane through the caster across it, and
    /// ignores occluders behind that plane, such as the emitters themselves.
    foundation::math::Vector3 LightNormalWorld;
    /// Radius of that light disk, in metres: the uniform disk with the members' flux-weighted second moment of area
    /// about the caster's sphere centre, at most LightRadiusMeters. A single polygon gets about its equal-area disk.
    foundation::float32 DiskRadiusMeters = 0;
    /// Occluders nearer along a face's axis are ignored: a two-sided caster's own emitting surfaces, and the half of
    /// a one-sided caster's offset that lies in front of its emitters. Faces store linear depth, whose precision
    /// does not depend on it.
    foundation::float32 NearMeters = 0;
    /// Every receiver a member can light, and every receiver in the scene bounds, lies within this distance; it is
    /// the faces' far plane, and the 16-bit depth codes span (RangeMeters - NearMeters) / 65535 metres each.
    foundation::float32 RangeMeters = 0;
    /// Sum of the members' largest emission channel times area, which ranks casters for atlas space.
    foundation::float32 Flux = 0;
    /// Bit f is set when cube face f (+X, -X, +Y, -Y, +Z, -Z) can receive a member's light.
    foundation::uint32 FaceMask = 0;
    /// Side of every face slot in texels, including MESH_LIGHT_SHADOW_GUTTER_TEXELS on each edge.
    foundation::uint32 FaceTexels = 0;
    /// Atlas origin of each face slot; meaningful for faces in FaceMask after allocation.
    foundation::StaticArray<MeshLightShadowFaceOrigin, MESH_LIGHT_SHADOW_FACE_COUNT> FaceOrigins{};
};
/// Instances [First, First + Count) of one mesh drawn into a shadow face, with the material whose masked coverage they
/// need, if any.
struct MeshLightShadowPacket final
{
    foundation::usize MeshSlot = 0;
    /// Renderer material slot of an alpha-masked material, drawn with its shadow pipeline; unused when IsMasked is
    /// false and the flat shadow pipeline draws the packet.
    foundation::usize MaterialSlot = 0;
    bool IsMasked = false;
    foundation::uint32 First = 0;
    foundation::uint32 Count = 0;
};
/// One face slot's raster pass: its atlas rectangle and its packets.
struct MeshLightShadowFaceDraw final
{
    rhi::RasterRectangle Slot{};
    foundation::uint32 FirstPacket = 0;
    foundation::uint32 PacketCount = 0;
    /// Triangles its packets submit, which bounds the faces drawn per frame.
    foundation::uint64 Triangles = 0;
};
/// Side of each directional slot. Directional squares lead the atlas and cube faces are packed after them.
inline constexpr foundation::uint32 DIRECTIONAL_SHADOW_TEXELS = 1024;
/// Directional lights given a map in one prepared scene, brightest first: an Atmosphere's Sun and Moon. Further
/// eligible directional lights illuminate unshadowed and are counted in the report. The squares share the
/// MESH_LIGHT_SHADOW_ATLAS_TEXELS atlas with cube faces, so each one halves the space those faces can use.
inline constexpr foundation::uint32 MAX_DIRECTIONAL_SHADOW_REGIONS = MAX_DIRECTIONAL_SHADOW_LIGHTS;

/// DIRECTIONAL_SHADOW_TEXELS squares reserved ahead of cube faces: one per directional map, then one square per sky
/// level that tiles its visibility maps (deferred_layout.h, SKY_VISIBILITY_DIRECTION_COUNT).
inline constexpr foundation::uint32 MAX_RESERVED_SHADOW_SQUARES = MAX_DIRECTIONAL_SHADOW_REGIONS + 2;
/// Directional and sky visibility regions drawn before any cube face.
inline constexpr foundation::uint32 MAX_LEADING_SHADOW_REGIONS =
    2 * MAX_DIRECTIONAL_SHADOW_REGIONS + 2 * SKY_VISIBILITY_DIRECTION_COUNT;
static_assert(SKY_VISIBILITY_SLOTS_PER_SIDE * SKY_VISIBILITY_SLOTS_PER_SIDE == SKY_VISIBILITY_DIRECTION_COUNT);
static_assert(SKY_VISIBILITY_SLOTS_PER_SIDE * SKY_VISIBILITY_SLOT_TEXELS == DIRECTIONAL_SHADOW_TEXELS);
static_assert(MAX_DIRECTIONAL_SHADOW_REGIONS + SKY_VISIBILITY_DIRECTION_COUNT <= SHADOW_REGION_CAPACITY);

/// Reserved square index's interior, inside a one-texel gutter. Squares follow the Z-order of
/// DIRECTIONAL_SHADOW_TEXELS squares from the atlas origin, the order AllocateRasterMeshLightShadowFaces reserves.
[[nodiscard]] rhi::RasterRectangle GetDirectionalShadowInterior(foundation::uint32 index) noexcept;
/// Interior of sky visibility map direction (row-major SKY_VISIBILITY_SLOTS_PER_SIDE grid) inside a one-texel gutter,
/// within the reserved square whose own interior GetDirectionalShadowInterior returns.
[[nodiscard]] rhi::RasterRectangle GetSkyVisibilityInterior(const rhi::RasterRectangle& squareInterior,
                                                            foundation::uint32 direction) noexcept;
/// Unit world direction toward the sky of sky visibility map direction, for direction in
/// [0, SKY_VISIBILITY_DIRECTION_COUNT). The directions follow a spherical Fibonacci spiral over the +Y hemisphere:
/// equal solid angles, elevation sines (direction + 0.5) / count below one and successive azimuths a golden angle
/// apart, so a cosine-weighted sum over them integrates the upper hemisphere without favouring an azimuth.
[[nodiscard]] foundation::math::Vector3 GetSkyVisibilityDirection(foundation::uint32 direction) noexcept;
/// Side of a square atlas holding reservedSquareCount reserved squares and no cube faces.
[[nodiscard]] foundation::uint32 GetReservedShadowAtlasTexels(foundation::uint32 reservedSquareCount) noexcept;

/// Atlas progress before or during one recorded frame.
struct ShadowAtlasProgress final
{
    /// Index of the first mesh-light face still to render.
    foundation::uint32 NextFace = 0;
    bool IsCleared = false;
    /// Leading directional and sky visibility regions already drawn.
    foundation::uint32 DrawnDirectional = 0;
};

/// GPU state of one prepared scene's shadow atlas. The scene generation creates the texture and its sampled view;
/// Renderer::prepareDeferredShadows builds the face packets from the scene snapshot, and
/// Renderer::drawDeferredShadows clears the atlas to the far depth (unoccluded) and then draws the directional and sky
/// visibility regions and whole cube faces in caster order (brightest first), over as many frames as the per-frame draw
/// and triangle budgets require.
struct DeferredShadowAtlas final
{
    /// Directional regions, brightest light first, followed by coarse and optional local sky visibility regions in
    /// matching direction order; all are drawn before any mesh-light faces.
    foundation::StaticArray<ShadowRegion, MAX_LEADING_SHADOW_REGIONS> Directional{};
    foundation::StaticArray<MeshLightShadowFaceDraw, MAX_LEADING_SHADOW_REGIONS> DirectionalDraws{};
    /// Leading Directional entries that map lights.
    foundation::uint32 DirectionalCount = 0;
    /// Whole-scene regions come first, then their admitted near regions, then the sky regions.
    foundation::uint32 NearCount = 0;
    /// True when near regions can be fitted without overlapping any mesh-light face.
    bool HasNearSpace = false;
    /// Sky visibility regions after them: zero or SKY_VISIBILITY_DIRECTION_COUNT.
    foundation::uint32 SkyCount = 0;
    /// A second square covers a bounded neighbourhood, keeping each coarse map's full depth range.
    foundation::uint32 LocalSkyCount = 0;
    foundation::uint64 SkyRevision = 0;
    bool HasLocalSkyBudgetFallback = false;
    foundation::math::Vector3 SkyAnchorWorld{};
    foundation::uint32 WidthTexels = 0;
    foundation::uint32 HeightTexels = 0;
    rhi::TextureHandle Texture{};
    rhi::TextureViewHandle Sampled{};
    rhi::BufferHandle Instances{};
    bool HasSampled = false;
    foundation::Array<MeshLightShadowPacket> Packets;
    foundation::Array<MeshLightShadowFaceDraw> Faces;
    ShadowAtlasProgress Submitted;
    ShadowAtlasProgress Recording;
    bool HasPackets = false;
};
/// Counts describing one clustering.
struct MeshLightShadowReport final
{
    /// Casters built, at most MAX_MESH_LIGHT_SHADOW_CASTERS.
    foundation::uint32 Casters = 0;
    /// Records assigned to a caster.
    foundation::uint32 ShadowedRecords = 0;
    /// Records left unshadowed because the caster limit was reached.
    foundation::uint32 UnshadowedRecords = 0;
    /// Faces that receive light over all casters, each rendered once.
    foundation::uint32 Faces = 0;
};
/// Group the recordCount emitter polygon records that start inoutWords into shadow casters, brightest first, and write
/// each record's caster plus one into its MESH_LIGHT_SHADOW_CASTER_CORNER w (zero past the caster limit). A record
/// joins an existing caster when the merged enclosing sphere stays within a radius limit and, at the finer levels,
/// the sidedness matches and a one-sided normal lies within a cone about the caster's first member. The finest of a
/// fixed sequence of ever coarser levels (0.5 m within 30 and then 90 degrees, then 0.5, 1 and 2 m across normals)
/// that needs at most MAX_MESH_LIGHT_SHADOW_CASTERS casters is used, or the coarsest up to that limit. A caster with
/// two-sided members, or one-sided members beyond 30 degrees of their flux-weighted mean normal, encloses them:
/// it renders all six faces from their sphere centre with a near distance of at least its radius. Otherwise it
/// renders from just in front of them, with a near distance of half that offset. A caster's range is the farthest
/// reach of its members' light, clamped to the farthest corner of receiversWorld (when it is not empty) widened by
/// how far the resolve's normal offset can move a receiver. Call before AppendRasterMeshLightGrid, which groups cell
/// lists by caster. Face slots are left unallocated.
/// @return Ready; InvalidDescription for malformed records or OutOfMemory, preserving every output.
[[nodiscard]] rhi::RasterStatus
BuildRasterMeshLightShadowCasters(foundation::Array<foundation::math::Vector4>& inoutWords,
                                  foundation::uint32 recordCount,
                                  const foundation::math::Aabb3& receiversWorld,
                                  foundation::Array<MeshLightShadowCaster>& outCasters,
                                  MeshLightShadowReport& outReport) noexcept;
/// Give every caster power-of-two face slots of MIN_MESH_LIGHT_SHADOW_FACE_TEXELS to
/// MAX_MESH_LIGHT_SHADOW_FACE_TEXELS texels, by the square root of its share of the brightest caster's flux, halving
/// every slot until all faces fit an atlas of atlasTexels squared, and pack them without overlap.
/// The leading reservedSquareCount reserved squares (at most MAX_RESERVED_SHADOW_SQUARES) are excluded from
/// cube-face allocation.
/// @return False, leaving the casters unchanged, when even the smallest slots do not fit.
[[nodiscard]] bool AllocateRasterMeshLightShadowFaces(foundation::Array<MeshLightShadowCaster>& inoutCasters,
                                                      foundation::uint32 atlasTexels,
                                                      foundation::uint32 reservedSquareCount = 0) noexcept;
/// Append the caster table (deferred_layout.h, MESH_LIGHT_SHADOW_HEADER_WORDS), followed by each caster's light-disk
/// word (MESH_LIGHT_SHADOW_DISK_WORDS), and report its first word.
/// @return Ready, or OutOfMemory preserving both outputs.
[[nodiscard]] rhi::RasterStatus
AppendRasterMeshLightShadowTable(foundation::Array<foundation::math::Vector4>& inoutWords,
                                 foundation::BorrowedView<const MeshLightShadowCaster> casters,
                                 foundation::uint32 atlasTexels,
                                 foundation::uint32& outTableWord) noexcept;
/// Tangent of half the field of view a face slot is rendered with: its interior spans 90 degrees and its gutter
/// extends beyond, so the full slot of faceTexels spans faceTexels / (faceTexels - 2 * gutter).
[[nodiscard]] foundation::float32 GetRasterMeshLightShadowTangent(foundation::uint32 faceTexels) noexcept;
/// The world-to-clip transform rendering one face slot of a caster, from its near distance to its range. Its
/// rasterized depth z / w is conventional, and every row is divided by the range so that clip z itself is the
/// linear depth (forward - near) / (range - near) the face stores (renderer_shadow_depth.slang,
/// SelectShadowDepth).
/// @return Ready, or InvalidDescription preserving outWorldToClip for a face outside [0, 6) or a degenerate caster.
[[nodiscard]] rhi::RasterStatus MakeRasterMeshLightShadowFaceToClip(const MeshLightShadowCaster& caster,
                                                                    foundation::uint32 face,
                                                                    foundation::math::Matrix4& outWorldToClip) noexcept;
/// CPU mirror of the resolve's lookup: the face a receiver falls in, its atlas texel coordinates (texel centres at
/// half-integers) and its linear depth in that face's projection: the clip z of MakeRasterMeshLightShadowFaceToClip.
/// @return False, leaving the outputs unchanged, for a receiver at the caster position.
[[nodiscard]] bool LocateRasterMeshLightShadowTexel(const MeshLightShadowCaster& caster,
                                                    foundation::math::Vector3 positionWorld,
                                                    foundation::uint32& outFace,
                                                    foundation::math::Vector2& outTexel,
                                                    foundation::float32& outDepth) noexcept;
/// The 16-bit fixed-point code stored for a depth in [0, 1] (clamped), in the red and green channels of the RGBA8
/// atlas as renderer_shadow.lusl writes it.
[[nodiscard]] foundation::uint32 EncodeRasterMeshLightShadowDepth(foundation::float32 depth) noexcept;
/// The depth an encoded code reads back as.
[[nodiscard]] foundation::float32 DecodeRasterMeshLightShadowDepth(foundation::uint32 code) noexcept;
} // namespace ludus::graphics::renderer::internal
