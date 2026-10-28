#pragma once

class QQuickItem;
class QSGNode;

namespace ludus::editor
{
class ViewportObjectInteraction;
struct ViewportObjectOverlay;
/// Create a Qt-owned native selection item. The GUI-thread interaction must outlive the item.
/// Projects strokes during polish and uploads triangles during scene-graph synchronization;
/// no viewport-sized painter texture or renderer resources are allocated.
[[nodiscard]] QQuickItem* CreateViewportObjectOverlayItem(ViewportObjectInteraction& inoutInteraction,
                                                          QQuickItem& inoutParent);
/// Update a Qt scene-graph-owned node from already projected strokes on the synchronization thread.
/// Takes ownership of oldNodeOrNull, returning its replacement; empty overlays delete the old node.
[[nodiscard]] QSGNode* UpdateViewportObjectOverlayNode(const ViewportObjectOverlay& overlay, QSGNode* oldNodeOrNull);
} // namespace ludus::editor
