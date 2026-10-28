#include "internal/viewport_object_overlay_item.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_object_overlay.h"

#include <QQuickItem>

namespace ludus::editor
{
namespace
{

class ObjectOverlayItem final : public QQuickItem
{
public:
    ObjectOverlayItem(ViewportObjectInteraction& inoutInteraction, QQuickItem& inoutParent)
        : QQuickItem(&inoutParent), mInteraction(inoutInteraction)
    {
        setObjectName(QStringLiteral("viewportObjectSelectionOverlay"));
        setFlag(ItemHasContents);
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
    bool mHasGeometryChanges = true;
};
} // namespace

QQuickItem* CreateViewportObjectOverlayItem(ViewportObjectInteraction& inoutInteraction, QQuickItem& inoutParent)
{
    return new ObjectOverlayItem(inoutInteraction, inoutParent);
}
} // namespace ludus::editor
