#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_document.h"

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace ludus::editor
{
/// Authored-position operation; alignment uses the selection's outer bounds, distribution its centres.
enum class RenderGraphLayoutOperation : foundation::uint8
{
    Arrange,               ///< Place dependency components in columns, with nonoverlapping cards.
    AlignLeft,             ///< Align left edges to the leftmost selected card.
    AlignHorizontalCentre, ///< Align horizontal centres to the selection bounds' centre.
    AlignRight,            ///< Align right edges to the rightmost selected card.
    AlignTop,              ///< Align top edges to the topmost selected card.
    AlignVerticalCentre,   ///< Align vertical centres to the selection bounds' centre.
    AlignBottom,           ///< Align bottom edges to the bottommost selected card.
    DistributeHorizontal,  ///< Space horizontal centres evenly, preserving the outermost centres.
    DistributeVertical,    ///< Space vertical centres evenly, preserving the outermost centres.
};

/// Copied authored card geometry in scene units; interface boxes are excluded by the caller.
struct RenderGraphLayoutNode final
{
    QString Identity; ///< Unique authored node ID.
    QRectF Bounds;    ///< Displayed card rectangle, including its current port rows and footer.
};

/// Directed authored-node dependency; endpoints outside the supplied nodes are ignored.
struct RenderGraphLayoutEdge final
{
    QString Source;      ///< Producer ID.
    QString Destination; ///< Consumer ID.
};

/// Compute integer saved positions without touching the document or scene. GUI thread only.
/// At most MAX_NODES unique, finite positive cards are accepted. Arrangement anchors at the
/// input bounds' top left, groups cycles in one column and uses identity order within columns.
/// Arrangement uses a common translation to fit the document's coordinate bounds; oversized
/// arrangements or out-of-range alignment/distribution fail. Alignment needs two cards; distribution needs three.
/// Invalid input or insufficient selection returns false and preserves outPositions. Success includes every supplied
/// node, even unchanged positions; the caller filters no-ops against saved positions.
[[nodiscard]] bool ComputeRenderGraphLayout(const QVector<RenderGraphLayoutNode>& nodes,
                                            const QVector<RenderGraphLayoutEdge>& edges,
                                            RenderGraphLayoutOperation operation,
                                            QVector<RenderGraphNodePosition>& outPositions);
} // namespace ludus::editor
