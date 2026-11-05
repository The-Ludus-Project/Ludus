#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
/// Strokes of a selection outline. Widths are physical pixels of the target, so a caller on a scaled display
/// multiplies the logical width by the device pixel ratio.
struct SelectionOutlineStyle final
{
    /// Straight RGBA of the main stroke, each component in [0,1], written as display-encoded values.
    foundation::math::Vector4 OutlineColor{1, 1, 1, 1};
    /// Straight RGBA of the stroke drawn under it, each component in [0,1], so the outline reads on bright and
    /// dark pixels alike. Alpha zero omits the casing.
    foundation::math::Vector4 CasingColor{0, 0, 0, 0};
    /// Width of the main stroke, centred on the silhouette: positive, at most MAX_STROKE_WIDTH_PIXELS.
    foundation::float32 OutlineWidthPixels = 2;
    /// Width of the casing stroke, centred on the silhouette: at least OutlineWidthPixels and at most
    /// MAX_STROKE_WIDTH_PIXELS.
    foundation::float32 CasingWidthPixels = OutlineWidthPixels * 2;
};
/// Everything one frame of a selection outline depends on besides the mesh.
struct SelectionOutlineView final
{
    /// Finite mesh-local to homogeneous clip matrix, the same matrix that places the mesh in the image the
    /// outline is drawn over. The outline is the mesh's silhouette in that image.
    foundation::math::Matrix4 MeshToClip = foundation::math::Matrix4::Identity();
    /// Positive physical width of the target the outline is drawn into.
    foundation::uint32 Width = 0;
    /// Positive physical height of the target the outline is drawn into.
    foundation::uint32 Height = 0;
    /// Stroke colours and widths.
    SelectionOutlineStyle Style{};
};
namespace internal
{
/// Private identity access for independently retained selection outline versions.
struct SelectionOutlineAccess;
} // namespace internal
/// Owned immutable prepared outline for one view; generation-checked and scoped to one renderer.
/// Release once between frames; accepted GPU work retains exact resources.
struct PreparedSelectionOutline final
{
private:
    foundation::uint64 mOwner = 0;
    foundation::uint64 mGeneration = 0;
    foundation::uint32 mSlot = 0;
    /// Grants private access to incarnation identities.
    friend struct internal::SelectionOutlineAccess;
};
/// Draws the silhouette outline of one selected triangle mesh over an image, entirely on the GPU.
///
/// The outline is found in screen space instead of from the mesh's edges. The selected mesh is rasterized once
/// into a one-channel coverage mask, with no depth test so that another object in front does not hide the
/// outline. A second pass over the mesh's projected bounds then strokes the boundary of that mask: every pixel
/// within half a stroke width of a boundary between covered and uncovered pixels takes the stroke colour. Only
/// the outer contour of the projected mesh, and the contour of any hole in it, is stroked; edges inside the
/// projected area are not part of the mask boundary.
///
/// The cost per frame is one rasterization of the mesh's triangles plus a bounded neighbourhood search per pixel
/// near the mesh, and does not grow with an adjacency build or per-frame CPU work on the mesh. Selecting a mesh
/// uploads its positions and indices once, in SetMesh.
///
/// It uses the portable raster profile only, a vertex/index buffer pair, instance rows, one sampled
/// texture and premultiplied blending, so it runs on every RHI backend including the browser backends.
///
/// All members run on the RHI owner thread, between frames, except Draw, which records into an open frame. The
/// device must outlive this object.
class SelectionOutlineRenderer final
{
public:
    /// Largest stroke width, in physical pixels, bounding the per-pixel search.
    static constexpr foundation::float32 MAX_STROKE_WIDTH_PIXELS = 16;
    /// Construct without allocation or device access.
    SelectionOutlineRenderer() noexcept;
    /// Release logical GPU ownership; RHI retires accepted uses.
    ~SelectionOutlineRenderer();
    /// Create the fixed shaders, layouts, sampler and pipelines.
    /// @param device Ready device with the portable raster profile.
    /// @param maskVertex Vertex stage of renderer_selection_mask.
    /// @param maskFragment Fragment stage of renderer_selection_mask.
    /// @param edgeVertex Vertex stage of renderer_selection_outline.
    /// @param edgeFragment Fragment stage of renderer_selection_outline.
    /// @return Ready or Pending once owned (poll Poll until Ready); InvalidDescription when a stage does not
    /// have the expected interface; InvalidState when already initialized. Failure requires Reset before retry.
    [[nodiscard]] rhi::RasterStatus Initialize(rhi::DeviceHandle device,
                                               const rhi::RasterShaderDescription& maskVertex,
                                               const rhi::RasterShaderDescription& maskFragment,
                                               const rhi::RasterShaderDescription& edgeVertex,
                                               const rhi::RasterShaderDescription& edgeFragment) noexcept;
    /// Advance asynchronous setup between frames, without waiting.
    [[nodiscard]] rhi::RasterStatus Poll() noexcept;
    /// Drop the mesh, prepared versions and fixed resources, between frames.
    void Reset() noexcept;
    /// Whether Initialize and its asynchronous setup have completed.
    [[nodiscard]] bool IsReady() const noexcept;
    /// Upload the mesh to outline, replacing any earlier one. The arrays are copied before return; the caller may
    /// release them at once. Existing prepared versions stay valid and draw the new mesh.
    /// @param positionsMesh Finite mesh-local positions.
    /// @param indices Triangle-list indices into positionsMesh: a positive multiple of three, each below the
    /// position count.
    /// @return Ready or Pending (poll GetMeshStatus); NotReady before Initialize completes; InvalidDescription for
    /// an empty, non-finite or out-of-range mesh; CapacityExceeded or OutOfMemory when the RHI refuses the size.
    /// Failure preserves the previous mesh.
    [[nodiscard]] rhi::RasterStatus SetMesh(foundation::BorrowedView<const foundation::math::Vector3> positionsMesh,
                                            foundation::BorrowedView<const foundation::uint32> indices) noexcept;
    /// Forget the mesh. Prepared versions then draw nothing.
    void ClearMesh() noexcept;
    /// Readiness of the mesh buffers: Ready, Pending, or NotReady when no mesh is set.
    [[nodiscard]] rhi::RasterStatus GetMeshStatus() noexcept;
    /// Create the per-frame data of an outline of the current mesh. Creating a version is cheap (one small
    /// buffer), so a caller prepares one per distinct view. At most four versions exist at once.
    /// @param view Matrix, target extent and strokes.
    /// @param outPrepared Null on entry; receives the version on Ready or Pending.
    /// @return Ready or Pending (poll GetStatus); NotReady before Initialize completes or without a mesh;
    /// InvalidDescription for a non-finite matrix, an empty extent or invalid strokes; CapacityExceeded when four
    /// versions are in use; InvalidState when outPrepared is not null. Failure preserves outPrepared.
    [[nodiscard]] rhi::RasterStatus Prepare(const SelectionOutlineView& view,
                                            PreparedSelectionOutline& outPrepared) noexcept;
    /// Poll the version's buffers and the mesh until all are ready to draw. Draw requires Ready.
    [[nodiscard]] rhi::RasterStatus GetStatus(PreparedSelectionOutline prepared) noexcept;
    /// Draw the outline over an already initialized target. Records two passes into the open frame: the mask, then the
    /// strokes over description's target. The target keeps its contents; only pixels near the silhouette change.
    /// A mesh entirely outside the target draws nothing and records no pass.
    /// @param prepared A version whose GetStatus returned Ready, prepared for exactly this target extent.
    /// @param description Target and optional viewport/scissor. Color is the acquired surface when null, otherwise
    /// an RGBA8 linear attachment. Load, store and depth members are replaced.
    /// @return Ready; InvalidHandle for a stale version; InvalidState when the extent changed or the mesh was
    /// replaced by an empty one; Unsupported for another target format; or the RHI failure that stopped recording.
    [[nodiscard]] rhi::RasterStatus Draw(PreparedSelectionOutline prepared,
                                         const rhi::RasterPassDescription& description) noexcept;
    /// Clear owned identity; submitted resources retire only after RHI completion.
    [[nodiscard]] rhi::RasterStatus Release(PreparedSelectionOutline& inoutPrepared) noexcept;

private:
    struct State;
    foundation::UniquePtr<State> mState;
};
} // namespace ludus::graphics::renderer
