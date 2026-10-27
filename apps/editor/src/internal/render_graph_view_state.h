#pragma once

#include <ludus/foundation/base/types.h>

#include <QPointF>
#include <QStringList>

namespace ludus::editor
{
/// Direction through displayed connections in one configuration; never enters Subgraph assets.
enum class RenderGraphTraceDirection : foundation::uint8
{
    None,       ///< Normal selection/port-hover highlighting.
    Upstream,   ///< Producers that reach the selected nodes.
    Downstream, ///< Consumers reachable from the selected nodes.
};

/// View-only canvas snapshot. CentreScene is the scene point at the viewport centre;
/// Zoom is a dimensionless magnification (1 means 100%). Selection contains authored node IDs.
/// Missing selection IDs are ignored on restoration; no document state or scene-item pointers are held.
struct RenderGraphViewState final
{
    QStringList Selection;        ///< Authored node IDs, independent of scene-item lifetimes.
    QPointF CentreScene;          ///< Scene-space point at the viewport centre.
    foundation::float64 Zoom = 1; ///< Dimensionless magnification, bounded by the canvas on capture/restore.
    RenderGraphTraceDirection Trace = RenderGraphTraceDirection::None; ///< Saved dependency-trace mode.
};
} // namespace ludus::editor
