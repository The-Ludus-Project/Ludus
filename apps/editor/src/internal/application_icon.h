#pragma once

#include <QIcon>

namespace ludus::editor
{
/// The Ludus application icon: the optically tuned 16, 24 and 32 pixel artwork for small sizes and
/// the approved reference tile for everything larger, so the OS picks the nearest drawing instead
/// of scaling one image (docs/architecture/editor-art-direction.md, Mark and icon use).
[[nodiscard]] QIcon MakeApplicationIcon();
} // namespace ludus::editor
