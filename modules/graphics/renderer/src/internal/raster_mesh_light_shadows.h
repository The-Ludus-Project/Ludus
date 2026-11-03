#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

// Cached cube shadow maps for raster mesh lights (render-graph-authoring.md, "Mesh-light shadows"). Emitter polygon
// records are grouped into casters; each caster renders up to six cube faces of conventional depth once per
// prepared scene with the renderer's flat shadow pipeline, and the deferred resolve compares a receiver's depth in
// the same face projection with them.
namespace ludus::graphics::renderer::internal
{
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
    /// Radius of the sphere enclosing every member polygon, in metres; scales the penumbra.
    foundation::float32 LightRadiusMeters = 0;
    /// Occluders nearer along a face's axis are ignored: a two-sided caster's own emitting surfaces, and a sliver
    /// in front of a one-sided caster. At least a hundredth of the range, which bounds 16-bit depth precision.
    foundation::float32 NearMeters = 0;
    /// Every receiver a member can light lies within this distance; it is the faces' far plane.
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
/// GPU state of one prepared scene's shadow atlas. The scene generation creates the texture and its sampled view;
/// Renderer::prepareMeshLightShadows builds the face packets from the scene snapshot, and
/// Renderer::drawMeshLightShadows clears the atlas to the far depth (unoccluded) and then draws whole faces, in caster
/// order (brightest first), over as many frames as the per-frame draw and triangle budgets require.
struct MeshLightShadowAtlas final
{
    rhi::TextureHandle Texture{};
    rhi::TextureViewHandle Sampled{};
    rhi::BufferHandle Instances{};
    bool HasSampled = false;
    foundation::Array<MeshLightShadowPacket> Packets;
    foundation::Array<MeshLightShadowFaceDraw> Faces;
    /// First face not yet drawn.
    foundation::uint32 NextFace = 0;
    bool HasPackets = false;
    bool IsCleared = false;
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
/// renders from just in front of them. Call before AppendRasterMeshLightGrid, which groups cell lists by caster.
/// Face slots are left unallocated.
/// @return Ready; InvalidDescription for malformed records or OutOfMemory, preserving every output.
[[nodiscard]] rhi::RasterStatus
BuildRasterMeshLightShadowCasters(foundation::Array<foundation::math::Vector4>& inoutWords,
                                  foundation::uint32 recordCount,
                                  foundation::Array<MeshLightShadowCaster>& outCasters,
                                  MeshLightShadowReport& outReport) noexcept;
/// Give every caster power-of-two face slots of MIN_MESH_LIGHT_SHADOW_FACE_TEXELS to
/// MAX_MESH_LIGHT_SHADOW_FACE_TEXELS texels, by the square root of its share of the brightest caster's flux, halving
/// every slot until all faces fit an atlas of atlasTexels squared, and pack them without overlap.
/// @return False, leaving the casters unchanged, when even the smallest slots do not fit.
[[nodiscard]] bool AllocateRasterMeshLightShadowFaces(foundation::Array<MeshLightShadowCaster>& inoutCasters,
                                                      foundation::uint32 atlasTexels) noexcept;
/// Append the caster table (deferred_layout.h, MESH_LIGHT_SHADOW_HEADER_WORDS) and report its first word.
/// @return Ready, or OutOfMemory preserving both outputs.
[[nodiscard]] rhi::RasterStatus
AppendRasterMeshLightShadowTable(foundation::Array<foundation::math::Vector4>& inoutWords,
                                 foundation::BorrowedView<const MeshLightShadowCaster> casters,
                                 foundation::uint32 atlasTexels,
                                 foundation::uint32& outTableWord) noexcept;
/// Tangent of half the field of view a face slot is rendered with: its interior spans 90 degrees and its gutter
/// extends beyond, so the full slot of faceTexels spans faceTexels / (faceTexels - 2 * gutter).
[[nodiscard]] foundation::float32 GetRasterMeshLightShadowTangent(foundation::uint32 faceTexels) noexcept;
/// The conventional-depth world-to-clip transform rendering one face slot of a caster, from its near distance to
/// its range.
/// @return Ready, or InvalidDescription preserving outWorldToClip for a face outside [0, 6) or a degenerate caster.
[[nodiscard]] rhi::RasterStatus MakeRasterMeshLightShadowFaceToClip(const MeshLightShadowCaster& caster,
                                                                    foundation::uint32 face,
                                                                    foundation::math::Matrix4& outWorldToClip) noexcept;
/// CPU mirror of the resolve's lookup: the face a receiver falls in, its atlas texel coordinates (texel centres at
/// half-integers) and its conventional depth in that face's projection (MakeRasterMeshLightShadowFaceToClip).
/// @return False, leaving the outputs unchanged, for a receiver at the caster position.
[[nodiscard]] bool LocateRasterMeshLightShadowTexel(const MeshLightShadowCaster& caster,
                                                    foundation::math::Vector3 positionWorld,
                                                    foundation::uint32& outFace,
                                                    foundation::math::Vector2& outTexel,
                                                    foundation::float32& outDepth) noexcept;
/// The 16-bit fixed-point code stored for a depth in [0, 1] (clamped), in the red and green channels of the RGBA8
/// atlas as renderer_shadow.slang writes it.
[[nodiscard]] foundation::uint32 EncodeRasterMeshLightShadowDepth(foundation::float32 depth) noexcept;
/// The depth an encoded code reads back as.
[[nodiscard]] foundation::float32 DecodeRasterMeshLightShadowDepth(foundation::uint32 code) noexcept;
} // namespace ludus::graphics::renderer::internal
