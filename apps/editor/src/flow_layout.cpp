#include "internal/flow_layout.h"

#include <algorithm>

#include <QWidget>

namespace ludus::editor
{
using foundation::int32;

FlowLayout::FlowLayout(QWidget* parentOrNull, int32 spacingPixels) : QLayout(parentOrNull)
{
    setContentsMargins(0, 0, 0, 0);
    setSpacing(spacingPixels);
}

FlowLayout::~FlowLayout()
{
    for (QLayoutItem* item : mItems)
    {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem* item)
{
    mItems.append(item);
    invalidate();
}

int FlowLayout::count() const
{
    return static_cast<int>(mItems.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const
{
    return mItems.value(index, nullptr);
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= mItems.size())
    {
        return nullptr;
    }
    QLayoutItem* item = mItems.takeAt(index);
    invalidate();
    return item;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return arrange(QRect(0, 0, width, 0), false);
}

void FlowLayout::setGeometry(const QRect& rectangle)
{
    QLayout::setGeometry(rectangle);
    static_cast<void>(arrange(rectangle, true));
}

QSize FlowLayout::sizeHint() const
{
    int32 width = 0;
    int32 height = 0;
    int32 shown = 0;
    for (const QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == true)
        {
            continue;
        }
        const QSize hint = item->sizeHint();
        width += hint.width() + (shown > 0 ? spacing() : 0);
        height = std::max(height, hint.height());
        ++shown;
    }
    const QMargins margins = contentsMargins();
    return {width + margins.left() + margins.right(), height + margins.top() + margins.bottom()};
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == false)
        {
            size = size.expandedTo(item->minimumSize());
        }
    }
    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

int32 FlowLayout::arrange(const QRect& area, bool shouldPlace) const
{
    const QMargins margins = contentsMargins();
    const QRect inner = area.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    const int32 available = std::max(inner.width(), 0);
    QList<QLayoutItem*> shown;
    int32 rowWidth = 0;
    int32 cellWidth = 0;
    for (QLayoutItem* item : mItems)
    {
        if (item->isEmpty() == true)
        {
            continue;
        }
        const int32 width = item->sizeHint().width();
        rowWidth += width + (shown.isEmpty() == false ? spacing() : 0);
        cellWidth = std::max(cellWidth, width);
        shown.append(item);
    }
    if (shown.isEmpty() == true)
    {
        return margins.top() + margins.bottom();
    }
    // Everything fits: one row at the items' own widths.
    if (rowWidth <= available)
    {
        int32 x = inner.x();
        int32 rowHeight = 0;
        for (QLayoutItem* item : std::as_const(shown))
        {
            const QSize hint = item->sizeHint();
            if (shouldPlace == true)
            {
                item->setGeometry(QRect(QPoint(x, inner.y()), hint));
            }
            x += hint.width() + spacing();
            rowHeight = std::max(rowHeight, hint.height());
        }
        return inner.y() + rowHeight - area.y() + margins.bottom();
    }
    // Otherwise a grid of equal cells as wide as the widest item, stretched to fill each row, so wrapped buttons
    // line up instead of leaving a ragged edge.
    cellWidth = std::min(cellWidth, available);
    const qsizetype columns =
        std::clamp<qsizetype>((available + spacing()) / std::max(cellWidth + spacing(), 1), 1, shown.size());
    const int32 columnCount = static_cast<int32>(columns);
    const int32 stretchedWidth = std::max(cellWidth, (available - (columnCount - 1) * spacing()) / columnCount);
    int32 y = inner.y();
    for (qsizetype first = 0; first < shown.size(); first += columns)
    {
        int32 rowHeight = 0;
        for (qsizetype index = first; index < std::min(first + columns, shown.size()); ++index)
        {
            rowHeight = std::max(rowHeight, shown.at(index)->sizeHint().height());
        }
        for (qsizetype index = first; index < std::min(first + columns, shown.size()); ++index)
        {
            const int32 column = static_cast<int32>(index - first);
            if (shouldPlace == true)
            {
                shown.at(index)->setGeometry(
                    QRect(inner.x() + column * (stretchedWidth + spacing()), y, stretchedWidth, rowHeight));
            }
        }
        y += rowHeight + spacing();
    }
    return y - spacing() - area.y() + margins.bottom();
}
} // namespace ludus::editor
