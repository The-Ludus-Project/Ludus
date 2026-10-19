#include "internal/viewport_frame_source.h"

namespace ludus::editor
{
ViewportFrameSource::ViewportFrameSource(QObject* parentOrNull) : QObject(parentOrNull) {}
ViewportFrameSource::~ViewportFrameSource() = default;
} // namespace ludus::editor
