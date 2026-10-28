#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/transform.hpp>

#include <QJsonObject>
#include <QLineF>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QVector>

class QPainter;
namespace ludus::editor
{
class RenderPreview;
class WorldDocument;
/// GUI-thread object picking and a transactional transform gesture. Preview destruction disables queries and drawing.
/// A drag previews a wire cage; release requests one validated World edit, Escape/focus loss discards it.
class ViewportObjectInteraction final : public QObject
{
    Q_OBJECT
public:
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
    /// Empty follows the viewport; otherwise match the presented graph image letterboxing.
    void SetImageSize(QSize size);
    [[nodiscard]] bool Press(QPointF positionPixel, const QRectF& framePixel);
    [[nodiscard]] bool Move(QPointF positionPixel, bool shouldSnap);
    [[nodiscard]] bool Release();
    void Cancel();
    /// Paint in the same logical coordinates used by Press. No device or image readback required.
    void Paint(QPainter& inoutPainter, const QRectF& framePixel);
    /// Closest triangle under a point; empty means background. Handles perspective and orthographic views.
    [[nodiscard]] QString Pick(QPointF positionPixel, const QRectF& framePixel) const;
Q_SIGNALS:
    void Changed();
    void SelectionRequested(const QString& id);
    /// Original stable ID and absolute placement arrays (metres, Euler degrees, positive scale).
    void PlacementCommitted(const QString& id, const QJsonObject& placement);

private:
    struct HandleLine
    {
        QLineF Line;
        foundation::int32 Axis = 0;
    };
    [[nodiscard]] bool describe(const QRectF& framePixel, foundation::math::Matrix4& outWorldToClip) const;
    [[nodiscard]] bool project(foundation::math::Vector3 positionWorld, QPointF& outPositionPixel) const;
    [[nodiscard]] QRectF resolveFrame(const QRectF& framePixel) const;
    void buildHandles();
    [[nodiscard]] foundation::math::Affine3 makeTransform(const QJsonObject& placement) const;
    QPointer<RenderPreview> mPreviewOrNull;
    QString mSelected;
    QJsonObject mPlacement;
    QJsonObject mCandidate;
    foundation::math::Affine3 mParentToWorld;
    foundation::math::Matrix4 mWorldToClip;
    QRectF mFramePixel;
    QSize mImageSize;
    QPointF mPressPixel;
    QPointF mPivotPixel;
    QPointF mAxisPixel;
    foundation::float64 mPixelsPerUnit = 1;
    foundation::float64 mRotationDirection = 1;
    QVector<HandleLine> mHandles;
    Tool mTool = Tool::Move;
    foundation::int32 mDragAxis = -1;
    bool mCanEdit = false;
    bool mIsActive = true;
};
} // namespace ludus::editor
