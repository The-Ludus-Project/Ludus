#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/graphics/renderer/lighting.hpp>

#include "mesh_silhouette.h"
#include "selection_outline_request.h"
#include "viewport_object_overlay.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QString>

class QPainter;
namespace ludus::editor
{
class RenderPreview;
struct PreviewObject;
class WorldDocument;
/// GUI-thread object picking and a transactional transform gesture. Preview destruction disables queries and drawing.
/// The selection is marked by its silhouette, and a drag previews the silhouette of the candidate; release requests one
/// validated World edit, Escape/focus loss discards it.
class ViewportObjectInteraction final : public QObject
{
    Q_OBJECT
public:
    /// Number of spatial axes exposed by the transform tools.
    static constexpr foundation::int32 AXIS_COUNT = ViewportObjectOverlay::AXIS_COUNT;
    /// Line segments in each rotation ring.
    static constexpr foundation::int32 RING_SEGMENTS = ViewportObjectOverlay::RING_SEGMENTS;
    /// Position, rotation and scale are distinct authoring properties.
    static constexpr foundation::usize TRANSFORM_PROPERTY_COUNT = 3;
    /// Bounded property names or schema prefixes, kept in their shared property order.
    using TransformPropertyNames = foundation::StaticArray<QString, TRANSFORM_PROPERTY_COUNT>;
    enum class Tool : foundation::uint8
    {
        Select,
        Move,
        Rotate,
        Scale
    };
    /// What a handle drags: one axis, a plane (move only) every axis at once (scale only), or a volume face.
    enum class HandleKind : foundation::uint8
    {
        Axis,
        Plane,
        Uniform,
        TerrainExtent,
        ProbeFace,
        ProbeSpacing
    };
    /// A handle under the pointer or being dragged. Index is the axis, from 0 for X, of an axis handle and the axis
    /// normal to a plane handle (0 for the YZ plane, 1 for XZ, 2 for XY). Probe face indices are the three
    /// maximum faces in axis order followed by the three minimum faces. A negative index means no handle.
    struct HandleTarget final
    {
        HandleKind Kind = HandleKind::Axis;
        foundation::int32 Index = -1;
        [[nodiscard]] LUDUS_INLINE bool IsValid() const noexcept
        {
            return Index >= 0;
        }
        [[nodiscard]] LUDUS_INLINE bool operator==(const HandleTarget& other) const noexcept
        {
            return Index == other.Index && (Index < 0 || Kind == other.Kind);
        }
    };
    explicit ViewportObjectInteraction(RenderPreview& inoutPreview, QObject* parentOrNull);
    /// Refresh authoring permission/placement from the authoritative World. Cancels a stale gesture.
    void SetWorld(const WorldDocument& world, const QString& selected, bool canEdit);
    /// Draw the Add Terrain footprint and its X/Z resize handles for the current draft selection.
    /// An empty recipe removes the draft overlay.
    void SetTerrainFootprint(const QJsonObject& recipe);
    /// An empty key restores normal picking. The brush radius is measured in terrain-local metres.
    /// While a key is set, left presses belong to the brush: they never select or show transform handles.
    /// When canSculpt is false (for example while the previous stroke's preview is pending), presses are
    /// consumed without starting a stroke or picking a height.
    void SetTerrainBrush(const QString& key,
                         foundation::float32 radius,
                         bool shouldPickHeight,
                         bool shouldShowSlope,
                         bool canSculpt);
    void SetTool(Tool tool);
    [[nodiscard]] LUDUS_INLINE Tool GetTool() const noexcept
    {
        return mTool;
    }
    void SetActive(bool isActive);
    /// Whether the UI owner currently admits object input and drawing.
    [[nodiscard]] LUDUS_INLINE bool IsActive() const noexcept
    {
        return mIsActive;
    }
    /// Empty follows the viewport; otherwise match the presented graph image letterboxing.
    void SetImageSize(QSize size);
    /// Project picking, the wire cage and handles through the camera of the shown image instead of
    /// the live navigation camera, so they stay on its pixels while a newer camera still renders.
    /// Only clip x/y and the [0, w] depth range are read, so a conventional-depth view is accepted.
    void SetPresentedView(const foundation::math::Matrix4& worldToClip);
    /// Follow the live navigation camera again, for images rendered synchronously with it.
    void ClearPresentedView();
    /// Track the pointer without a button: the handle within reach of positionPixel is highlighted.
    /// Returns whether a handle is under the pointer. Ignored while dragging; emits Changed on a new target.
    [[nodiscard]] bool Hover(QPointF positionPixel, const QRectF& framePixel);
    /// Drop the hover highlight, as when the pointer leaves the viewport.
    void ClearHover();
    /// Axis, from 0 for X, of the axis handle under the pointer, or -1 when none, or when it is a plane or centre
    /// handle.
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetHoveredAxis() const noexcept
    {
        return mHoverTarget.Kind == HandleKind::Axis ? mHoverTarget.Index : -1;
    }
    /// The handle of any kind under the pointer; its Index is negative when there is none.
    [[nodiscard]] LUDUS_INLINE HandleTarget GetHoveredHandle() const noexcept
    {
        return mHoverTarget;
    }
    /// Whether the pointer is over a handle of any kind.
    [[nodiscard]] LUDUS_INLINE bool HasHoveredHandle() const noexcept
    {
        return mHoverTarget.IsValid();
    }
    /// Whether a handle drag is in progress.
    [[nodiscard]] LUDUS_INLINE bool IsDragging() const noexcept
    {
        return mDragTarget.IsValid() || mTerrainStroke.isEmpty() == false;
    }
    /// The handle being dragged; its Index is negative when no drag is in progress.
    [[nodiscard]] LUDUS_INLINE HandleTarget GetDraggedHandle() const noexcept
    {
        return mDragTarget;
    }
    [[nodiscard]] bool Press(QPointF positionPixel, const QRectF& framePixel);
    /// Request framing for the visible entity under a double-click in the edit view.
    void RequestFrameAt(QPointF positionPixel, const QRectF& framePixel);
    /// Frame the selected probe grid, including one whose bake is disabled.
    [[nodiscard]] bool FrameProbeVolume() noexcept;
    [[nodiscard]] bool Move(QPointF positionPixel, bool shouldSnap);
    [[nodiscard]] bool Release();
    void Cancel();
    /// Paint in the same logical coordinates used by Press. No device or image readback required.
    void Paint(QPainter& inoutPainter, const QRectF& framePixel);
    /// Project the bounded silhouette outline and handle strokes on the GUI thread. Failure clears output, retaining
    /// buffer capacity. The same geometry feeds the native scene graph and the readback painter.
    /// While the GPU outline is enabled the silhouette is left out; the luminaire panel and handles remain.
    void BuildOverlay(const QRectF& framePixel, ViewportObjectOverlay& outOverlay);
    /// Leave the selected mesh's silhouette to the GPU, which draws it into the shown image, instead of building it on
    /// the CPU. Emits GpuOutlineChanged when the request that follows from the selection changes.
    void SetGpuOutlineEnabled(bool isEnabled);
    [[nodiscard]] LUDUS_INLINE bool IsGpuOutlineEnabled() const noexcept
    {
        return mIsGpuOutlineEnabled;
    }
    /// The outline the GPU should draw for the last overlay build: the selected object's mesh, its candidate placement
    /// during a drag, and strokes scaled to the shown image.
    /// @return False when no object with a mesh is selected and shown; outRequest is then unchanged.
    [[nodiscard]] bool TryGetGpuOutlineRequest(SelectionOutlineRequest& outRequest) const;
    /// Closest triangle under a point; empty means background. Handles perspective and orthographic views.
    /// Objects whose cached mesh bounds the ray misses, or reaches only beyond the nearest hit so far,
    /// skip their triangles. The cache is rebuilt once after each scene identity change.
    [[nodiscard]] QString Pick(QPointF positionPixel, const QRectF& framePixel);
Q_SIGNALS:
    void Changed();
    /// The selection outline the GPU should draw changed, possibly to none; read it with TryGetGpuOutlineRequest.
    void GpuOutlineChanged();
    void SelectionRequested(const QString& id);
    void FrameRequested(const QString& id);
    /// Original stable ID and absolute placement arrays (metres, Euler degrees, positive scale).
    void PlacementCommitted(const QString& id, const QJsonObject& placement);
    void ProbeSpacingCommitted(const QString& id, foundation::float64 spacingMetres);
    void ProbeFaceCommitted(const QString& id, foundation::int32 axis, foundation::uint32 count, bool isMinimumFace);
    void TerrainExtentCommitted(const QString& key, foundation::float64 extent);
    void TerrainHeightPicked(foundation::float32 height);
    /// The stroke being dragged gained a point: the local XZ pairs since the press, in order.
    /// The first emission of a gesture carries exactly the press point. Finished, Rejected or Cancelled ends it.
    void TerrainStrokeChanged(const QJsonArray& points);
    void TerrainStrokeFinished(const QJsonArray& points);
    void TerrainStrokeRejected(const QString& message);
    /// Cancel, or pausing sculpting, discarded the stroke being dragged; no Finished or Rejected follows for it.
    void TerrainStrokeCancelled();
    /// The terrain was pressed with a brush while sculpting is paused; no stroke or height pick started.
    void TerrainSculptRefused();

private:
    struct HandleLine
    {
        QLineF Line;
        foundation::int32 Axis = 0;
        /// Rotation ring segments farther from the camera than the pivot draw dimmer.
        bool IsBack = false;
    };
    /// BuildOverlay without the GPU outline hand-off.
    void buildOverlayGeometry(const QRectF& framePixel, ViewportObjectOverlay& outOverlay);

    /// A plane handle: a parallelogram beside the pivot, spanned by the screen directions of two axes.
    struct PlaneHandle
    {
        /// The axis normal to the plane.
        foundation::int32 NormalAxis = 0;
        ViewportHandleFill::Corners CornersPixel{};
    };
    [[nodiscard]] bool describe(const QRectF& framePixel, foundation::math::Matrix4& outWorldToClip) const;
    [[nodiscard]] bool project(foundation::math::Vector3 positionWorld, QPointF& outPositionPixel) const;
    [[nodiscard]] QRectF resolveFrame(const QRectF& framePixel) const;
    /// Pixel position of a clip-space point whose w is positive, in the frame of the last describe.
    [[nodiscard]] QPointF convertClipToPixel(const foundation::math::Vector4& clip) const;
    /// Clip a segment to the camera's near side and the frame's sides and convert it to pixels.
    /// False for a segment that cannot be seen; outLinePixel is then unchanged.
    [[nodiscard]] bool projectSegment(const SilhouetteSegment& segment, QLineF& outLinePixel) const;
    /// Append the silhouette of object, or its bounding box when its mesh is too large for adjacency, to the overlay.
    void appendObjectOutline(const PreviewObject& object,
                             const foundation::math::Affine3& meshToWorld,
                             ViewportObjectOverlay& inoutOverlay);
    void appendBoxOutline(const foundation::math::Aabb3& bounds,
                          const foundation::math::Matrix4& boundsToClip,
                          ViewportObjectOverlay& inoutOverlay) const;
    void appendProbeVolume(ViewportObjectOverlay& inoutOverlay) const;
    [[nodiscard]] bool projectProbeFace(const graphics::renderer::GiProbeGrid& grid,
                                        foundation::int32 face,
                                        QPointF& outPositionPixel) const;
    [[nodiscard]] bool canEditProbeVolume() const noexcept;
    [[nodiscard]] bool projectProbeScaleHandle(const graphics::renderer::GiProbeGrid& grid,
                                               QPointF& outPositionPixel) const;
    [[nodiscard]] HandleTarget pickProbeHandle(QPointF positionPixel) const;
    [[nodiscard]] bool beginProbeFaceDrag(QPointF positionPixel);
    void dragProbeFace(QPointF positionPixel);
    void dragProbeSpacing(QPointF positionPixel, QPointF deltaPixel, bool shouldSnap);
    graphics::renderer::GiProbeGrid mProbeGrid{};
    graphics::renderer::GiProbeGrid mProbeDragGrid{};
    QRectF mProbeDragFramePixel;
    bool mIsStandaloneProbeVolume = false;
    /// Make mSilhouette the adjacency of scene mesh meshIndex, rebuilding once per scene identity or mesh change.
    /// False when that mesh has no usable adjacency.
    [[nodiscard]] bool refreshSilhouette(foundation::int32 meshIndex);
    void buildHandles();
    void appendTerrainFootprint(ViewportObjectOverlay& inoutOverlay) const;
    void appendTerrainBrush(ViewportObjectOverlay& inoutOverlay) const;
    [[nodiscard]] QString pick(QPointF positionPixel,
                               const QRectF& framePixel,
                               foundation::math::Vector3& outHitMesh,
                               foundation::float32* outSlopeDegreesOrNull);
    [[nodiscard]] bool updateTerrainBrush(QPointF positionPixel, const QRectF& outerFramePixel);
    /// Returns true when the hit became the stroke's newest point.
    [[nodiscard]] bool appendTerrainStrokePoint();
    [[nodiscard]] HandleTarget pickTerrainHandle(QPointF positionPixel) const;
    [[nodiscard]] bool beginTerrainDrag(QPointF positionPixel);
    void dragTerrainExtent(QPointF positionPixel);
    [[nodiscard]] foundation::math::Vector3 getTerrainHandleMesh(foundation::int32 axis) const noexcept;
    QString mTerrainBrushKey;
    QJsonArray mTerrainStroke;
    foundation::math::Vector3 mTerrainBrushHitMesh;
    foundation::float32 mTerrainBrushRadius = 0;
    foundation::float32 mTerrainSlopeDegrees = 0;
    bool mHasTerrainBrushHit = false;
    bool mShouldPickTerrainHeight = false;
    bool mShouldShowTerrainSlope = false;
    bool mCanSculptTerrain = false;
    bool mHasTerrainStrokeGap = false;
    bool mHasTerrainStrokeOverflowed = false;
    QRectF mTerrainStrokeFramePixel;
    foundation::math::Vector2 mTerrainExtent;
    foundation::float32 mTerrainBaseElevation = 0;
    foundation::float32 mTerrainDragExtent = 0;
    /// Axis of the built handle nearest positionPixel within reach, or -1; records that handle's screen direction.
    /// The handle under positionPixel: the centre handle first, then an axis within a few pixels of its shaft, then a
    /// plane handle, then the nearest axis within reach. Records an axis handle's screen direction.
    [[nodiscard]] HandleTarget pickHandle(QPointF positionPixel);
    /// Fill mPlaneHandles for the move tool from each axis's screen direction, skipping a plane seen edge-on.
    void buildPlaneHandles(const foundation::StaticArray<QPointF, AXIS_COUNT>& axisUnitsPixel,
                           const foundation::StaticArray<bool, AXIS_COUNT>& hasAxisUnits,
                           foundation::math::Vector3 forwardWorld);
    /// Colour of a handle with hover and drag emphasis applied. colourAxis selects the axis token.
    [[nodiscard]] QRgb getHandleColour(HandleTarget target, foundation::int32 colourAxis, bool isBack) const;
    /// Append one handle's strokes and fills with hover and drag emphasis applied.
    void appendHandleGeometry(const HandleLine& handle, ViewportObjectOverlay& inoutOverlay) const;
    /// Append a plane handle's casing and solid parallelogram.
    void appendPlaneGeometry(const PlaneHandle& plane, ViewportObjectOverlay& inoutOverlay) const;
    /// Append a uniform scale handle's casing and solid cube.
    void appendUniformGeometry(HandleTarget target, QPointF centerPixel, ViewportObjectOverlay& inoutOverlay) const;
    /// Begin dragging a plane handle: fix the plane through the pivot and where the pointer meets it.
    [[nodiscard]] bool beginPlaneDrag(QPointF positionPixel);
    /// Drag results for the three kinds of handle. Each updates the placement values and the readout text, and returns
    /// false when the pointer gives no usable value.
    [[nodiscard]] bool dragAxis(QPointF deltaPixel, bool shouldSnap, QJsonArray& inoutValues);
    [[nodiscard]] bool dragPlane(QPointF positionPixel, bool shouldSnap, QJsonArray& inoutValues);
    [[nodiscard]] bool dragUniform(QPointF deltaPixel, bool shouldSnap, QJsonArray& inoutValues);
    [[nodiscard]] foundation::math::Affine3 makeTransform(const QJsonObject& placement) const;
    /// Recompute mMeshBoundsMesh for a changed scene identity; false leaves it empty (no rejection).
    [[nodiscard]] bool refreshMeshBounds();
    QPointer<RenderPreview> mPreviewOrNull;
    QString mSelected;
    QJsonObject mPlacement;
    QJsonObject mCandidate;
    foundation::math::Affine3 mParentToWorld;
    foundation::math::Matrix4 mWorldToClip;
    foundation::math::Matrix4 mPresentedWorldToClip;
    QRectF mFramePixel;
    QSize mImageSize;
    QPointF mPressPixel;
    QPointF mPivotPixel;
    QPointF mAxisPixel;
    foundation::float64 mPixelsPerUnit = 1;
    foundation::float64 mRotationDirection = 1;
    /// Half extent of the selected entity's luminaire panel, outlined in the overlay; zero when it has none.
    foundation::float32 mLuminaireHalfExtentWorld = 0;
    static constexpr foundation::usize MAX_HANDLE_LINES =
        static_cast<foundation::usize>(AXIS_COUNT) * static_cast<foundation::usize>(RING_SEGMENTS);
    foundation::FixedArray<HandleLine, MAX_HANDLE_LINES> mHandles;
    static constexpr foundation::usize MAX_PLANE_HANDLES = static_cast<foundation::usize>(AXIS_COUNT);
    foundation::FixedArray<PlaneHandle, MAX_PLANE_HANDLES> mPlaneHandles;
    /// Whether the scale tool's centre handle, at the pivot, is built.
    bool mHasUniformHandle = false;
    /// The plane a plane drag moves in: through the pivot as pressed, with the world directions of its two parent
    /// axes, and where the pointer met it when pressed.
    foundation::math::Matrix4 mDragClipToWorld;
    foundation::math::Vector3 mDragPlaneOriginWorld;
    foundation::math::Vector3 mDragPlaneNormalWorld;
    foundation::math::Vector3 mDragPlaneStartWorld;
    foundation::int32 mDragPlaneFirstAxis = 0;
    foundation::int32 mDragPlaneSecondAxis = 1;
    /// Numeric readout of the drag in progress, and the pointer position it follows.
    QString mReadoutText;
    QPointF mReadoutAnchorPixel;
    ViewportObjectOverlay mPaintOverlay;
    /// Welded edge adjacency of the selected object's mesh, cached with the scene identity and mesh it was built for.
    MeshSilhouette mSilhouette;
    QByteArray mSilhouetteIdentity;
    foundation::int32 mSilhouetteMesh = -1;
    bool mHasSilhouette = false;
    /// Scratch for one object's outline, reused between builds.
    foundation::Array<SilhouetteSegment> mOutlineSegments;
    /// Padded mesh-space bounds, one per scene mesh, of the scene whose Identity is mMeshBoundsIdentity.
    foundation::Array<foundation::math::Aabb3> mMeshBoundsMesh;
    QByteArray mMeshBoundsIdentity;
    /// What the GPU outline draws, as of the last overlay build; Mesh is -1 when nothing is outlined.
    struct GpuOutline final
    {
        QByteArray SceneIdentity;
        foundation::int32 Mesh = -1;
        foundation::math::Affine3 MeshToWorld{};
        foundation::float32 StrokeScale = 1;
        [[nodiscard]] friend bool operator==(const GpuOutline&, const GpuOutline&) = default;
    };
    GpuOutline mGpuOutline;
    GpuOutline mGpuOutlineReported;
    Tool mTool = Tool::Move;
    HandleTarget mDragTarget;
    HandleTarget mHoverTarget;
    bool mCanEdit = false;
    bool mIsActive = true;
    bool mHasPresentedView = false;
    bool mIsGpuOutlineEnabled = false;
    bool mHasReportedGpuOutline = false;
};
} // namespace ludus::editor
