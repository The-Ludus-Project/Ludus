#pragma once

#include <ludus/foundation/base/compiler.h>

#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QWidget>

namespace ludus::editor
{
/// Routes a mouse's side Back/Forward buttons (Qt::BackButton and Qt::ForwardButton, also
/// called X1/X2) to an owner's history actions, as browsers do. Applies while the pointer is
/// over area or one of its descendants in the same window; popups and other windows are not
/// covered. A press or double-click triggers the matching action when it is enabled, never
/// while another mouse button is held. Presses, double-clicks and releases of these buttons
/// are consumed, so area never treats them as ordinary clicks. The application-wide filter
/// lives until owner is destroyed; area and both actions must outlive owner. GUI thread only.
class MouseHistoryButtons final : public QObject
{
public:
    LUDUS_INLINE MouseHistoryButtons(QObject& inoutOwner, QWidget& inoutArea, QAction& inoutBack, QAction& inoutForward)
        : QObject(&inoutOwner), mArea(inoutArea), mBack(inoutBack), mForward(inoutForward)
    {
        QCoreApplication* applicationOrNull = QCoreApplication::instance();
        if (applicationOrNull != nullptr)
        {
            applicationOrNull->installEventFilter(this);
        }
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        const QEvent::Type type = event->type();
        if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick &&
            type != QEvent::MouseButtonRelease)
        {
            return false;
        }
        const QMouseEvent* mouse = static_cast<const QMouseEvent*>(event);
        QAction* actionOrNull = mouse->button() == Qt::BackButton      ? &mBack
                                : mouse->button() == Qt::ForwardButton ? &mForward
                                                                       : nullptr;
        const QWidget* widgetOrNull = qobject_cast<const QWidget*>(watched);
        if (actionOrNull == nullptr || widgetOrNull == nullptr ||
            (widgetOrNull != &mArea && mArea.isAncestorOf(widgetOrNull) == false))
        {
            return false;
        }
        const Qt::MouseButtons otherButtons = mouse->buttons() & ~Qt::MouseButtons(Qt::BackButton | Qt::ForwardButton);
        if (type != QEvent::MouseButtonRelease && otherButtons == Qt::NoButton && actionOrNull->isEnabled() == true)
        {
            actionOrNull->trigger();
        }
        event->accept();
        return true;
    }

private:
    QWidget& mArea;
    QAction& mBack;
    QAction& mForward;
};
} // namespace ludus::editor
