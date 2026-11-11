#pragma once

#include <QImage>
#include <QWidget>

/// Render a widget into a raster whose pixel coordinates match its layout coordinates.
[[nodiscard]] QImage RenderWidgetAtLogicalSize(QWidget& inoutWidget);

/// Deliver a local mouse move without moving the host desktop cursor.
void SendTestMouseMove(QWidget& inoutWidget, const QPoint& positionWidget);
