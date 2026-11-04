#include "internal/render_graph_layout.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QHash>
#include <QPair>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <utility>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr float64 COLUMN_GAP = 60;
constexpr float64 ROW_GAP = 28;
constexpr float64 BEND_HEIGHT = 0;
/// Barycentre passes over every column pair; layered graphs of the maximum size settle well inside this.
constexpr qsizetype CROSSING_SWEEPS = 12;
/// Row-relaxation passes; each is linear in slots.
constexpr qsizetype PLACEMENT_SWEEPS = 8;

bool AreBoundsValid(const QRectF& bounds)
{
    return math::IsFinite(bounds.x()) == true && math::IsFinite(bounds.y()) == true &&
           math::IsFinite(bounds.width()) == true && math::IsFinite(bounds.height()) == true && bounds.width() > 0 &&
           bounds.height() > 0 && qAbs(bounds.x()) <= RenderGraphDocument::MAX_COORDINATE &&
           qAbs(bounds.y()) <= RenderGraphDocument::MAX_COORDINATE &&
           bounds.width() <= RenderGraphDocument::MAX_COORDINATE &&
           bounds.height() <= RenderGraphDocument::MAX_COORDINATE;
}

/// One slot in a layered column: an authored card, or a zero-width bend that carries a wire across a column.
struct LayeredSlot final
{
    qsizetype Node = -1; ///< Index into the supplied nodes; -1 for a bend.
    float64 Height = 0;
    float64 Top = 0;
    QVector<qsizetype> Before; ///< Slots in earlier columns that feed this one.
    QVector<qsizetype> After;  ///< Slots in later columns this one feeds.
};

/// A slot's mean neighbour position and its rank before sorting, which breaks ties.
struct RankedSlot final
{
    float64 Key = 0;
    qsizetype Rank = 0;
};

void SortUnique(QVector<qsizetype>& inoutValues)
{
    std::sort(inoutValues.begin(), inoutValues.end());
    inoutValues.erase(std::unique(inoutValues.begin(), inoutValues.end()), inoutValues.end());
}

/// Mean rank of the neighbours as a fraction of their column, so columns of different sizes compare.
float64 GetBarycentre(const QVector<qsizetype>& neighbours, const QVector<float64>& fractions, float64 fallback)
{
    if (neighbours.isEmpty() == true)
    {
        return fallback;
    }
    float64 sum = 0;
    for (const qsizetype neighbour : neighbours)
    {
        sum += fractions.at(neighbour);
    }
    return sum / static_cast<float64>(neighbours.size());
}

/// Layered placement (Sugiyama): columns follow dependency order, wires crossing a column get a bend slot,
/// rows are reordered by neighbour barycentres to reduce crossings, then cards are pulled toward the
/// middle of their neighbours without leaving their order. Deterministic: every tie falls back to index.
void PlaceLayeredNodes(const QVector<RenderGraphLayoutNode>& nodes,
                       const QVector<RenderGraphLayoutEdge>& edges,
                       const QHash<QString, qsizetype>& indices,
                       const QVector<qsizetype>& components,
                       QVector<qsizetype>& inoutColumns,
                       QVector<QPointF>& outPositions)
{
    const qsizetype count = nodes.size();
    // Component representative -> column; edges inside one component (a cycle) carry no order.
    QVector<QPair<qsizetype, qsizetype>> links;
    for (const RenderGraphLayoutEdge& edge : edges)
    {
        if (indices.contains(edge.Source) == false || indices.contains(edge.Destination) == false)
        {
            continue;
        }
        const qsizetype source = indices.value(edge.Source);
        const qsizetype destination = indices.value(edge.Destination);
        if (components.at(source) != components.at(destination))
        {
            links.append({source, destination});
        }
    }
    std::sort(links.begin(), links.end());
    links.erase(std::unique(links.begin(), links.end()), links.end());
    // A card nothing feeds sits just before its first consumer instead of at the far left, which
    // shortens every wire it owns (a camera or scene hub then no longer spans the whole graph).
    QVector<uint8> isFed(count, 0);
    for (const QPair<qsizetype, qsizetype>& link : std::as_const(links))
    {
        isFed[components.at(link.second)] = 1;
    }
    QVector<qsizetype> nearest(count, count);
    for (const QPair<qsizetype, qsizetype>& link : std::as_const(links))
    {
        nearest[components.at(link.first)] =
            qMin(nearest.at(components.at(link.first)), inoutColumns.at(components.at(link.second)));
    }
    for (qsizetype index = 0; index < count; ++index)
    {
        const qsizetype representative = components.at(index);
        if (representative == index && isFed.at(index) == 0 && nearest.at(index) < count)
        {
            inoutColumns[index] = nearest.at(index) - 1;
        }
    }
    qsizetype firstColumn = count;
    for (qsizetype index = 0; index < count; ++index)
    {
        firstColumn = qMin(firstColumn, inoutColumns.at(components.at(index)));
    }
    QVector<LayeredSlot> slots;
    QVector<QVector<qsizetype>> columnSlots(count);
    const auto columnOf = [&](qsizetype node) { return inoutColumns.at(components.at(node)) - firstColumn; };
    for (qsizetype index = 0; index < count; ++index)
    {
        slots.append({ .Node = index, .Height = nodes.at(index).Bounds.height() });
        columnSlots[columnOf(index)].append(index);
    }
    for (const QPair<qsizetype, qsizetype>& link : std::as_const(links))
    {
        qsizetype previous = link.first;
        for (qsizetype column = columnOf(link.first) + 1; column < columnOf(link.second); ++column)
        {
            const qsizetype bend = slots.size();
            slots.append({ .Node = -1, .Height = BEND_HEIGHT });
            columnSlots[column].append(bend);
            slots[previous].After.append(bend);
            slots[bend].Before.append(previous);
            previous = bend;
        }
        slots[previous].After.append(link.second);
        slots[link.second].Before.append(previous);
    }
    for (LayeredSlot& slot : slots)
    {
        SortUnique(slot.Before);
        SortUnique(slot.After);
    }
    QVector<float64> fractions(slots.size(), 0.5);
    const auto refreshFractions = [&](qsizetype column) {
        const QVector<qsizetype>& members = columnSlots.at(column);
        for (qsizetype rank = 0; rank < members.size(); ++rank)
        {
            fractions[members.at(rank)] = (static_cast<float64>(rank) + 0.5) / static_cast<float64>(members.size());
        }
    };
    for (qsizetype column = 0; column < count; ++column)
    {
        refreshFractions(column);
    }
    const auto sortColumn = [&](qsizetype column, bool isForward) {
        QVector<qsizetype>& members = columnSlots[column];
        QVector<RankedSlot> keyed;
        for (qsizetype rank = 0; rank < members.size(); ++rank)
        {
            const LayeredSlot& slot = slots.at(members.at(rank));
            const float64 own = fractions.at(members.at(rank));
            keyed.append(
            {
                .Key = GetBarycentre(isForward == true ? slot.Before : slot.After, fractions, own),
                .Rank = rank,
            });
        }
        // Stable on the previous rank, so ties never reshuffle.
        std::stable_sort(keyed.begin(), keyed.end(), [](const RankedSlot& left, const RankedSlot& right) {
            return left.Key < right.Key;
        });
        QVector<qsizetype> reordered;
        for (const RankedSlot& entry : std::as_const(keyed))
        {
            reordered.append(members.at(entry.Rank));
        }
        members = reordered;
        refreshFractions(column);
    };
    for (qsizetype sweep = 0; sweep < CROSSING_SWEEPS; ++sweep)
    {
        for (qsizetype column = 1; column < count; ++column)
        {
            sortColumn(column, true);
        }
        for (qsizetype column = count - 2; column >= 0; --column)
        {
            sortColumn(column, false);
        }
    }
    // Rows: stack each column, then pull cards toward their neighbours' middle while keeping order.
    const auto stack = [&](qsizetype column, bool isForward) {
        float64 minimumTop = std::numeric_limits<float64>::lowest();
        for (const qsizetype slotIndex : std::as_const(columnSlots.at(column)))
        {
            LayeredSlot& slot = slots[slotIndex];
            const QVector<qsizetype>& neighbours = isForward == true ? slot.Before : slot.After;
            float64 wanted = slot.Top;
            if (neighbours.isEmpty() == false)
            {
                float64 sum = 0;
                for (const qsizetype neighbour : neighbours)
                {
                    sum += slots.at(neighbour).Top + slots.at(neighbour).Height * 0.5;
                }
                wanted = sum / static_cast<float64>(neighbours.size()) - slot.Height * 0.5;
            }
            slot.Top = qMax(wanted, minimumTop);
            minimumTop = slot.Top + slot.Height + ROW_GAP;
        }
    };
    for (qsizetype column = 0; column < count; ++column)
    {
        float64 top = 0;
        for (const qsizetype slotIndex : std::as_const(columnSlots.at(column)))
        {
            slots[slotIndex].Top = top;
            top += slots.at(slotIndex).Height + ROW_GAP;
        }
    }
    for (qsizetype sweep = 0; sweep < PLACEMENT_SWEEPS; ++sweep)
    {
        for (qsizetype column = 1; column < count; ++column)
        {
            stack(column, true);
        }
        for (qsizetype column = count - 2; column >= 0; --column)
        {
            stack(column, false);
        }
    }
    QVector<float64> widths(count, 0);
    for (qsizetype index = 0; index < count; ++index)
    {
        widths[columnOf(index)] = qMax(widths.at(columnOf(index)), nodes.at(index).Bounds.width());
    }
    QVector<float64> offsets(count, 0);
    for (qsizetype column = 1; column < count; ++column)
    {
        offsets[column] = offsets.at(column - 1) + widths.at(column - 1) + COLUMN_GAP;
    }
    float64 highest = std::numeric_limits<float64>::max();
    for (qsizetype index = 0; index < count; ++index)
    {
        highest = qMin(highest, slots.at(index).Top);
    }
    QRectF bounds;
    for (const RenderGraphLayoutNode& node : nodes)
    {
        bounds = bounds.united(node.Bounds);
    }
    for (qsizetype index = 0; index < count; ++index)
    {
        outPositions.append(bounds.topLeft() + QPointF(offsets.at(columnOf(index)), slots.at(index).Top - highest));
    }
}

void ArrangeNodes(const QVector<RenderGraphLayoutNode>& nodes,
                  const QVector<RenderGraphLayoutEdge>& edges,
                  QVector<QPointF>& outPositions)
{
    const qsizetype count = nodes.size();
    QHash<QString, qsizetype> indices;
    QVector<QVector<uint8>> reachable(count, QVector<uint8>(count, 0));
    for (qsizetype index = 0; index < count; ++index)
    {
        indices.insert(nodes.at(index).Identity, index);
        reachable[index][index] = 1;
    }
    for (const RenderGraphLayoutEdge& edge : edges)
    {
        if (indices.contains(edge.Source) == true && indices.contains(edge.Destination) == true)
        {
            reachable[indices.value(edge.Source)][indices.value(edge.Destination)] = 1;
        }
    }
    // Bounded transitive closure (MAX_NODES = 128) makes cyclic draft components explicit.
    // Mutual reachability gives one representative per component, without recursive traversal.
    for (qsizetype via = 0; via < count; ++via)
    {
        for (qsizetype source = 0; source < count; ++source)
        {
            if (reachable[source][via] == 0)
            {
                continue;
            }
            for (qsizetype destination = 0; destination < count; ++destination)
            {
                reachable[source][destination] |= reachable[via][destination];
            }
        }
    }
    QVector<qsizetype> components(count);
    QVector<qsizetype> columns(count, 0);
    for (qsizetype index = 0; index < count; ++index)
    {
        components[index] = index;
        for (qsizetype previous = 0; previous < index; ++previous)
        {
            if (reachable[index][previous] != 0 && reachable[previous][index] != 0)
            {
                components[index] = components[previous];
                break;
            }
        }
    }
    // The component graph is acyclic. Longest-path columns therefore converge within count passes.
    for (qsizetype pass = 0; pass < count; ++pass)
    {
        bool hasChanged = false;
        for (const RenderGraphLayoutEdge& edge : edges)
        {
            if (indices.contains(edge.Source) == false || indices.contains(edge.Destination) == false)
            {
                continue;
            }
            const qsizetype source = components.at(indices.value(edge.Source));
            const qsizetype destination = components.at(indices.value(edge.Destination));
            if (source != destination && columns.at(destination) <= columns.at(source))
            {
                columns[destination] = columns.at(source) + 1;
                hasChanged = true;
            }
        }
        if (hasChanged == false)
        {
            break;
        }
    }
    PlaceLayeredNodes(nodes, edges, indices, components, columns, outPositions);
}
} // namespace

bool ComputeRenderGraphLayout(const QVector<RenderGraphLayoutNode>& nodes,
                              const QVector<RenderGraphLayoutEdge>& edges,
                              RenderGraphLayoutOperation operation,
                              QVector<RenderGraphNodePosition>& outPositions)
{
    if (nodes.isEmpty() == true || nodes.size() > RenderGraphDocument::MAX_NODES ||
        (operation != RenderGraphLayoutOperation::Arrange && nodes.size() < 2) ||
        ((operation == RenderGraphLayoutOperation::DistributeHorizontal ||
          operation == RenderGraphLayoutOperation::DistributeVertical) &&
         nodes.size() < MINIMUM_RENDER_GRAPH_DISTRIBUTION_COUNT))
    {
        return false;
    }
    QHash<QString, QRectF> cards;
    QStringList identities;
    QRectF bounds;
    for (const RenderGraphLayoutNode& node : nodes)
    {
        if (node.Identity.isEmpty() == true || cards.contains(node.Identity) == true ||
            AreBoundsValid(node.Bounds) == false)
        {
            return false;
        }
        cards.insert(node.Identity, node.Bounds);
        identities.append(node.Identity);
        bounds = bounds.united(node.Bounds);
    }
    identities.sort(Qt::CaseSensitive);
    QVector<RenderGraphLayoutNode> ordered;
    for (const QString& identity : identities)
    {
        ordered.append({identity, cards.value(identity)});
    }
    QVector<QPointF> positions;
    if (operation == RenderGraphLayoutOperation::Arrange)
    {
        ArrangeNodes(ordered, edges, positions);
    }
    else if (operation == RenderGraphLayoutOperation::DistributeHorizontal ||
             operation == RenderGraphLayoutOperation::DistributeVertical)
    {
        const bool isHorizontal = operation == RenderGraphLayoutOperation::DistributeHorizontal;
        // Stable insertion by centre; identity order breaks ties independently of hash/selection order.
        QVector<RenderGraphLayoutNode> sorted;
        for (const RenderGraphLayoutNode& node : ordered)
        {
            const float64 centre = isHorizontal == true ? node.Bounds.center().x() : node.Bounds.center().y();
            qsizetype index = 0;
            while (index < sorted.size() && (isHorizontal == true ? sorted.at(index).Bounds.center().x()
                                                                  : sorted.at(index).Bounds.center().y()) <= centre)
            {
                ++index;
            }
            sorted.insert(index, node);
        }
        ordered = sorted;
        const float64 first =
            isHorizontal == true ? ordered.first().Bounds.center().x() : ordered.first().Bounds.center().y();
        const float64 last =
            isHorizontal == true ? ordered.last().Bounds.center().x() : ordered.last().Bounds.center().y();
        const float64 step = (last - first) / static_cast<float64>(ordered.size() - 1);
        for (qsizetype index = 0; index < ordered.size(); ++index)
        {
            const QRectF card = ordered.at(index).Bounds;
            const float64 centre = first + static_cast<float64>(index) * step;
            positions.append(isHorizontal == true ? QPointF(centre - card.width() * 0.5, card.y())
                                                  : QPointF(card.x(), centre - card.height() * 0.5));
        }
    }
    else
    {
        for (const RenderGraphLayoutNode& node : ordered)
        {
            const QRectF card = node.Bounds;
            QPointF position = card.topLeft();
            switch (operation)
            {
                case RenderGraphLayoutOperation::AlignLeft:
                    position.setX(bounds.left());
                    break;
                case RenderGraphLayoutOperation::AlignHorizontalCentre:
                    position.setX(bounds.center().x() - card.width() * 0.5);
                    break;
                case RenderGraphLayoutOperation::AlignRight:
                    position.setX(bounds.right() - card.width());
                    break;
                case RenderGraphLayoutOperation::AlignTop:
                    position.setY(bounds.top());
                    break;
                case RenderGraphLayoutOperation::AlignVerticalCentre:
                    position.setY(bounds.center().y() - card.height() * 0.5);
                    break;
                case RenderGraphLayoutOperation::AlignBottom:
                    position.setY(bounds.bottom() - card.height());
                    break;
                default:
                    return false;
            }
            positions.append(position);
        }
    }
    float64 minimumX = positions.first().x();
    float64 maximumX = minimumX;
    float64 minimumY = positions.first().y();
    float64 maximumY = minimumY;
    for (const QPointF position : positions)
    {
        minimumX = qMin(minimumX, position.x());
        maximumX = qMax(maximumX, position.x());
        minimumY = qMin(minimumY, position.y());
        maximumY = qMax(maximumY, position.y());
    }
    constexpr float64 LIMIT = RenderGraphDocument::MAX_COORDINATE;
    if ((operation != RenderGraphLayoutOperation::Arrange &&
         (minimumX < -LIMIT || maximumX > LIMIT || minimumY < -LIMIT || maximumY > LIMIT)) ||
        maximumX - minimumX > 2 * LIMIT || maximumY - minimumY > 2 * LIMIT)
    {
        return false;
    }
    const QPointF translation(qBound(-LIMIT - minimumX, 0.0, LIMIT - maximumX),
                              qBound(-LIMIT - minimumY, 0.0, LIMIT - maximumY));
    QVector<RenderGraphNodePosition> result;
    for (qsizetype index = 0; index < ordered.size(); ++index)
    {
        const QPointF position = positions.at(index) + translation;
        result.append({ordered.at(index).Identity, qRound(position.x()), qRound(position.y())});
    }
    outPositions = result;
    return true;
}

void ResolveRenderGraphOverlaps(const QVector<QRectF>& fixedCards, float64 gap, QVector<QRectF>& inoutMovableCards)
{
    QVector<QRectF> placed = fixedCards;
    for (QRectF& card : inoutMovableCards)
    {
        // Each push clears at least one card, so the loop ends within one pass per placed card.
        for (qsizetype pass = 0; pass <= placed.size(); ++pass)
        {
            const QRectF padded = card.adjusted(-gap / 2, -gap / 2, gap / 2, gap / 2);
            float64 clearTop = card.top();
            bool hasOverlap = false;
            for (const QRectF& other : std::as_const(placed))
            {
                if (padded.intersects(other.adjusted(-gap / 2, -gap / 2, gap / 2, gap / 2)) == true)
                {
                    hasOverlap = true;
                    clearTop = qMax(clearTop, other.bottom() + gap);
                }
            }
            if (hasOverlap == false)
            {
                break;
            }
            // Authored positions use whole scene units; preserve the original border offset.
            const float64 offsetScene = math::Ceil(clearTop - card.top());
            card.translate(0, offsetScene);
        }
        placed.append(card);
    }
}
} // namespace ludus::editor
