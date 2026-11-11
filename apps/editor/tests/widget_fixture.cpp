#include "widget_fixture.h"

#include <QCoreApplication>
#include <QMouseEvent>

QImage RenderWidgetAtLogicalSize(QWidget& inoutWidget)
{
    // The sample locations below are widget coordinates. A screen grab would
    // index device pixels, whose dimensions vary with the display scale.
    QImage image(inoutWidget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    inoutWidget.render(&image);
    return image;
}

void SendTestMouseMove(QWidget& inoutWidget, const QPoint& positionWidget)
{
    QMouseEvent event(QEvent::MouseMove,
                      positionWidget,
                      inoutWidget.mapToGlobal(positionWidget),
                      Qt::NoButton,
                      Qt::NoButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&inoutWidget, &event);
}
