#include "internal/viewport_object_overlay_item.h"
#include "internal/viewport_gizmo_readout.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_object_overlay.h"

#include <QPainter>
#include <QQuickItem>
#include <QQuickPaintedItem>

namespace ludus::editor
{
namespace
{

/// The number beside the pointer while a handle is dragged. Painted text in a label-sized texture of its own, so the
/// strokes stay one triangle batch and no viewport-sized texture exists.
class ReadoutItem final : public QQuickPaintedItem
{
public:
    explicit ReadoutItem(QQuickItem& inoutParent) : QQuickPaintedItem(&inoutParent)
    {
        setObjectName(QStringLiteral("viewportGizmoReadout"));
        setAntialiasing(true);
        setVisible(false);
        // Never takes the pointer from the handle being dragged.
        setEnabled(false);
    }
    /// Show text beside anchorPixel inside framePixel, or hide the label when text is empty.
    void SetReadout(const QString& text, QPointF anchorPixel, const QRectF& framePixel)
    {
        if (text.isEmpty() == true)
        {
            setVisible(false);
            return;
        }
        if (text != mText)
        {
            mText = text;
            setSize(MeasureGizmoReadout(mText));
            update();
        }
        setPosition(PlaceGizmoReadout(anchorPixel, size(), framePixel));
        setVisible(true);
    }
    void paint(QPainter* painter) override
    {
        PaintGizmoReadout(*painter, mText, {0, 0});
    }

private:
    QString mText;
};

class ObjectOverlayItem final : public QQuickItem
{
public:
    ObjectOverlayItem(ViewportObjectInteraction& inoutInteraction, QQuickItem& inoutParent)
        : QQuickItem(&inoutParent), mInteraction(inoutInteraction)
    {
        setObjectName(QStringLiteral("viewportObjectSelectionOverlay"));
        setFlag(ItemHasContents);
        mReadoutOrNull = new ReadoutItem(*this);
        connect(&mInteraction, &ViewportObjectInteraction::Changed, this, [this]() { polish(); });
        connect(this, &QQuickItem::widthChanged, this, [this]() { polish(); });
        connect(this, &QQuickItem::heightChanged, this, [this]() { polish(); });
        polish();
    }

protected:
    void updatePolish() override
    {
        // GUI state is read once per Qt frame. The scene-graph synchronization below
        // reads only these owned strokes while Qt blocks GUI mutations.
        mInteraction.BuildOverlay(boundingRect(), mOverlay);
        if (mReadoutOrNull != nullptr)
        {
            mReadoutOrNull->SetReadout(mOverlay.ReadoutText, mOverlay.ReadoutAnchorPixel, mOverlay.FramePixel);
        }
        mHasGeometryChanges = true;
        update();
    }
    QSGNode* updatePaintNode(QSGNode* oldNodeOrNull, UpdatePaintNodeData*) override
    {
        if (mHasGeometryChanges == false && oldNodeOrNull != nullptr)
        {
            return oldNodeOrNull;
        }
        mHasGeometryChanges = false;
        return UpdateViewportObjectOverlayNode(mOverlay, oldNodeOrNull);
    }

private:
    ViewportObjectInteraction& mInteraction;
    ViewportObjectOverlay mOverlay;
    ReadoutItem* mReadoutOrNull = nullptr;
    bool mHasGeometryChanges = true;
};
} // namespace

QQuickItem* CreateViewportObjectOverlayItem(ViewportObjectInteraction& inoutInteraction, QQuickItem& inoutParent)
{
    return new ObjectOverlayItem(inoutInteraction, inoutParent);
}
} // namespace ludus::editor
