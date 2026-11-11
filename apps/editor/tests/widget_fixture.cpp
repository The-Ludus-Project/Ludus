#include "widget_fixture.h"

QImage RenderWidgetAtLogicalSize(QWidget& inoutWidget)
{
    // The sample locations below are widget coordinates. A screen grab would
    // index device pixels, whose dimensions vary with the display scale.
    QImage image(inoutWidget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    inoutWidget.render(&image);
    return image;
}
