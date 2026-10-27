#include "internal/render_graph_layout.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QHash>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr float64 COLUMN_GAP = 60;
constexpr float64 ROW_GAP = 28;
constexpr qsizetype MINIMUM_DISTRIBUTION_COUNT = 3;

bool AreBoundsValid(const QRectF& bounds)
{
    return math::IsFinite(bounds.x()) == true && math::IsFinite(bounds.y()) == true &&
           math::IsFinite(bounds.width()) == true && math::IsFinite(bounds.height()) == true && bounds.width() > 0 &&
           bounds.height() > 0 && qAbs(bounds.x()) <= RenderGraphDocument::MAX_COORDINATE &&
           qAbs(bounds.y()) <= RenderGraphDocument::MAX_COORDINATE &&
           bounds.width() <= RenderGraphDocument::MAX_COORDINATE &&
           bounds.height() <= RenderGraphDocument::MAX_COORDINATE;
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
    QVector<float64> widths(count, 0);
    QVector<float64> nextRows(count, 0);
    for (qsizetype index = 0; index < count; ++index)
    {
        const qsizetype column = columns.at(components.at(index));
        widths[column] = qMax(widths.at(column), nodes.at(index).Bounds.width());
    }
    QVector<float64> offsets(count, 0);
    for (qsizetype column = 1; column < count; ++column)
    {
        offsets[column] = offsets.at(column - 1) + widths.at(column - 1) + COLUMN_GAP;
    }
    QRectF bounds;
    for (const RenderGraphLayoutNode& node : nodes)
    {
        bounds = bounds.united(node.Bounds);
    }
    for (qsizetype index = 0; index < count; ++index)
    {
        const qsizetype column = columns.at(components.at(index));
        outPositions.append(bounds.topLeft() + QPointF(offsets.at(column), nextRows.at(column)));
        nextRows[column] += nodes.at(index).Bounds.height() + ROW_GAP;
    }
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
         nodes.size() < MINIMUM_DISTRIBUTION_COUNT))
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
} // namespace ludus::editor
