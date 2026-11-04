#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/transform.hpp>

#include "mesh_silhouette.h"
#include "selection_outline_request.h"
#include "viewport_object_overlay.h"

#include <QByteArray>
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
    explicit ViewportObjectInteraction(RenderPreview& inoutPreview, QObject* parentOrNull);
    /// Refresh authoring permission/placement from the authoritative World. Cancels a stale gesture.
    void SetWorld(const WorldDocument& world, const QString& selected, bool canEdit);
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
    /// Axis, from 0 for X, of the handle under the pointer, or -1 when none.
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetHoveredAxis() const noexcept
    {
        return mHoverAxis;
    }
    /// Whether a handle drag is in progress.
    [[nodiscard]] LUDUS_INLINE bool IsDragging() const noexcept
    {
        return mDragAxis >= 0;
    }
    [[nodiscard]] bool Press(QPointF positionPixel, const QRectF& framePixel);
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
    /// Original stable ID and absolute placement arrays (metres, Euler degrees, positive scale).
    void PlacementCommitted(const QString& id, const QJsonObject& placement);

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
    /// Make mSilhouette the adjacency of scene mesh meshIndex, rebuilding once per scene identity or mesh change.
    /// False when that mesh has no usable adjacency.
    [[nodiscard]] bool refreshSilhouette(foundation::int32 meshIndex);
    void buildHandles();
    /// Axis of the built handle nearest positionPixel within reach, or -1; records that handle's screen direction.
    [[nodiscard]] foundation::int32 pickHandle(QPointF positionPixel);
    /// Append one handle's strokes and fills with hover and drag emphasis applied.
    void appendHandleGeometry(const HandleLine& handle, ViewportObjectOverlay& inoutOverlay) const;
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
    foundation::int32 mDragAxis = -1;
    foundation::int32 mHoverAxis = -1;
    bool mCanEdit = false;
    bool mIsActive = true;
    bool mHasPresentedView = false;
    bool mIsGpuOutlineEnabled = false;
    bool mHasReportedGpuOutline = false;
};
} // namespace ludus::editor
