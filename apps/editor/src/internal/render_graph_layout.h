#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_document.h"

#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVector>

namespace ludus::editor
{
/// Minimum authored-card selection for distribution; shared by menu admission and calculation.
inline constexpr foundation::int32 MINIMUM_RENDER_GRAPH_DISTRIBUTION_COUNT = 3;
/// Most cards one layout call accepts: well above a configuration's node limit plus its two interface boxes.
/// Layout stays near linear in cards and wires, so this only bounds pathological input.
inline constexpr foundation::int32 MAXIMUM_RENDER_GRAPH_LAYOUT_CARDS = 1024;

/// Authored-position operation; alignment uses the selection's outer bounds, distribution its centres.
enum class RenderGraphLayoutOperation : foundation::uint8
{
    Arrange,               ///< Place dependency components in columns, with nonoverlapping cards and few crossings.
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
/// At most MAXIMUM_RENDER_GRAPH_LAYOUT_CARDS unique, finite positive cards are accepted. Arrangement anchors at the
/// input bounds' top left, groups cycles in one column and lays out layers (Sugiyama style): columns by
/// dependency order, a card nothing feeds just before its first consumer, rows reordered by neighbour
/// barycentres to reduce wire crossings, then pulled toward their neighbours. Identity order breaks every tie.
/// Arrangement uses a common translation to fit the document's coordinate bounds; oversized
/// arrangements or out-of-range alignment/distribution fail. Alignment needs two cards; distribution needs three.
/// Invalid input or insufficient selection returns false and preserves outPositions. Success includes every supplied
/// node, even unchanged positions; the caller filters no-ops against saved positions.
[[nodiscard]] bool ComputeRenderGraphLayout(const QVector<RenderGraphLayoutNode>& nodes,
                                            const QVector<RenderGraphLayoutEdge>& edges,
                                            RenderGraphLayoutOperation operation,
                                            QVector<RenderGraphNodePosition>& outPositions);

/// How tangled a drawing is: wires are measured as straight port-to-port chords, which is what the eye follows.
struct RenderGraphWireMeasure final
{
    foundation::int32 Crossings = 0;     ///< Pairs of chords that cross; wires sharing a port never count.
    foundation::float64 TotalLength = 0; ///< Sum of chord lengths, in scene units.
};

/// Count crossings and total length of straight wires. Sweeps by left edge, so wires far apart are never compared.
[[nodiscard]] RenderGraphWireMeasure MeasureRenderGraphWires(const QVector<QLineF>& wires);

/// A wire between two cards with its ports' heights below each card's top, for placement that lines ports up.
struct RenderGraphLayoutWire final
{
    QString Source;                       ///< Producer ID.
    QString Destination;                  ///< Consumer ID.
    foundation::float64 SourceY = 0;      ///< Output port height below the producer's top.
    foundation::float64 DestinationY = 0; ///< Input port height below the consumer's top.
};

/// Display-only placement for cards without a saved position; saved cards never move. With nothing saved, the
/// whole graph gets the layered Auto Layout. Otherwise each unsaved card sits beside the saved cards it is wired
/// to: a producer one column left of its leftmost consumer, a consumer one column right of its rightmost producer,
/// at the height that puts its port level with the median of the ports it is wired to. Cards with no placed
/// neighbour keep their given position. Callers resolve overlaps.
void PlaceUnsavedRenderGraphCards(const QVector<RenderGraphLayoutWire>& wires,
                                  const QSet<QString>& savedCards,
                                  foundation::float64 columnSpacing,
                                  QVector<RenderGraphLayoutNode>& inoutCards);

/// Display-only placement: move each movable card down, in order, just far enough that no card overlaps a fixed
/// card or an earlier movable card, keeping gap between them. Use whole scene-unit offsets so the caller's integer
/// node origins remain representable even when borders have fractional bounds. Fixed cards never move.
/// Cards that already clear every other card stay where they are. GUI thread only.
void ResolveRenderGraphOverlaps(const QVector<QRectF>& fixedCards,
                                foundation::float64 gap,
                                QVector<QRectF>& inoutMovableCards);
} // namespace ludus::editor
