#pragma once

#include <QImage>
#include <QWidget>

/// Render a widget into a raster whose pixel coordinates match its layout coordinates.
[[nodiscard]] QImage RenderWidgetAtLogicalSize(QWidget& inoutWidget);
